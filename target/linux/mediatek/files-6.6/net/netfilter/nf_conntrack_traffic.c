// SPDX-License-Identifier: GPL-2.0
/* Bounded final CT snapshots for a passive traffic collector in init_net.
 * No packet hook, counter reset or hardware register access is added here.
 */
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/vmalloc.h>
#include <net/net_namespace.h>
#include <net/netfilter/nf_conntrack_acct.h>
#include <net/netfilter/nf_conntrack_zones.h>

#ifdef CONFIG_NF_CONNTRACK_PROCFS
#define CT_CLOSED_CAPACITY 2048
#define CT_CLOSED_WATCH (30 * HZ)

struct ct_closed_entry {
	struct nf_conntrack_tuple tuple[IP_CT_DIR_MAX];
	u64 packets[IP_CT_DIR_MAX];
	u64 bytes[IP_CT_DIR_MAX];
	u64 id, sequence, monotonic_ms;
	u32 status;
	u16 zone;
};

struct ct_closed_snapshot {
	u64 sequence, gaps;
	struct ct_closed_entry entries[CT_CLOSED_CAPACITY];
};

static DEFINE_SPINLOCK(ct_closed_lock);
static struct ct_closed_snapshot ct_closed;
static unsigned long ct_closed_watch_until;

void nf_ct_acct_record_destroy(const struct nf_conn *ct)
{
	const struct nf_conn_acct *acct;
	struct ct_closed_entry record = {};
	unsigned long until = READ_ONCE(ct_closed_watch_until);
	unsigned int dir;

	/* The final reference is gone. Accounting owners have released their
	 * references, so their last verified increments are already in this CT.
	 */
	if (!until || time_after(jiffies, until) || nf_ct_net(ct) != &init_net ||
	    !nf_ct_is_confirmed(ct))
		return;
	acct = nf_conn_acct_find(ct);
	if (!acct || !acct->total)
		return;
	if (nf_ct_l3num(ct) != NFPROTO_IPV4 && nf_ct_l3num(ct) != NFPROTO_IPV6)
		return;
	for (dir = 0; dir < IP_CT_DIR_MAX; dir++) {
		record.tuple[dir] = ct->tuplehash[dir].tuple;
		nf_ct_acct_read_totals(acct, dir, &record.packets[dir], &record.bytes[dir]);
	}
	record.id = acct->total->id;
	record.zone = nf_ct_zone_id(nf_ct_zone(ct), IP_CT_DIR_ORIGINAL);
	record.status = ct->status;
	record.monotonic_ms = div_u64(ktime_get_boottime_ns(), NSEC_PER_MSEC);
	spin_lock_bh(&ct_closed_lock);
	record.sequence = ++ct_closed.sequence;
	ct_closed.entries[(record.sequence - 1) % CT_CLOSED_CAPACITY] = record;
	spin_unlock_bh(&ct_closed_lock);
}

static void ct_closed_tuple_show(struct seq_file *s,
				 const struct nf_conntrack_tuple *tuple)
{
	if (tuple->src.l3num == NFPROTO_IPV4)
		seq_printf(s, "src=%pI4 dst=%pI4 ", &tuple->src.u3.ip, &tuple->dst.u3.ip);
	else
		seq_printf(s, "src=%pI6 dst=%pI6 ", tuple->src.u3.ip6, tuple->dst.u3.ip6);
	if (tuple->dst.protonum == IPPROTO_TCP || tuple->dst.protonum == IPPROTO_UDP)
		seq_printf(s, "sport=%u dport=%u ", ntohs(tuple->src.u.all),
			   ntohs(tuple->dst.u.all));
}

static int ct_closed_show(struct seq_file *s, void *unused)
{
	const struct ct_closed_snapshot *snap = s->private;
	u64 first = snap->sequence > CT_CLOSED_CAPACITY ?
		    snap->sequence - CT_CLOSED_CAPACITY + 1 : 1;
	u64 seq;

	seq_printf(s, "schema=1 sequence=%llu capacity=%u scope=init_net unit=ct_lifetime gaps=%llu\n",
		   snap->sequence, CT_CLOSED_CAPACITY, snap->gaps);
	for (seq = first; seq <= snap->sequence; seq++) {
		const struct ct_closed_entry *r = &snap->entries[(seq - 1) % CT_CLOSED_CAPACITY];
		u8 proto = r->tuple[0].dst.protonum;
		const char *name = proto == IPPROTO_TCP ? "tcp" :
				   proto == IPPROTO_UDP ? "udp" : "unknown";

		seq_printf(s, "%s %u %s %u 0 CLOSED ",
			   r->tuple[0].src.l3num == NFPROTO_IPV4 ? "ipv4" : "ipv6",
			   r->tuple[0].src.l3num, name, proto);
		ct_closed_tuple_show(s, &r->tuple[0]);
		ct_closed_tuple_show(s, &r->tuple[1]);
		seq_printf(s, "traffic_id=%llu packets_total=%llu bytes_total=%llu "
			   "packets_total=%llu bytes_total=%llu zone=%u "
			   "closed_seq=%llu closed_mono_ms=%llu\n",
			   r->id, r->packets[0], r->bytes[0], r->packets[1], r->bytes[1],
			   r->zone, r->sequence, r->monotonic_ms);
	}
	return 0;
}

static int ct_closed_open(struct inode *inode, struct file *file)
{
	struct ct_closed_snapshot *snap;
	int err;

	snap = kvzalloc(sizeof(*snap), GFP_KERNEL);
	if (!snap)
		return -ENOMEM;
	spin_lock_bh(&ct_closed_lock);
	if (ct_closed_watch_until && time_after(jiffies, ct_closed_watch_until))
		ct_closed.gaps++;
	WRITE_ONCE(ct_closed_watch_until, jiffies + CT_CLOSED_WATCH);
	memcpy(snap, &ct_closed, sizeof(*snap));
	spin_unlock_bh(&ct_closed_lock);
	err = single_open(file, ct_closed_show, snap);
	if (err)
		kvfree(snap);
	return err;
}

static int ct_closed_release(struct inode *inode, struct file *file)
{
	struct seq_file *seq = file->private_data;

	kvfree(seq->private);
	return single_release(inode, file);
}

static const struct proc_ops ct_closed_ops = {
	.proc_open = ct_closed_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = ct_closed_release,
};

void nf_ct_acct_closed_init(struct net *net)
{
	if (net == &init_net &&
	    !proc_create("nf_conntrack_closed", 0400, net->proc_net, &ct_closed_ops))
		pr_warn("conntrack: final traffic snapshot unavailable\n");
}

void nf_ct_acct_closed_fini(struct net *net)
{
	if (net == &init_net) {
		WRITE_ONCE(ct_closed_watch_until, 0);
		remove_proc_entry("nf_conntrack_closed", net->proc_net);
	}
}
#endif
