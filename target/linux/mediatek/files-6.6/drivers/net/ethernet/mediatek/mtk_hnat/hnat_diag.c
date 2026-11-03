// SPDX-License-Identifier: GPL-2.0-only
/* Bounded host decision history. Never reads or clears hardware counters. */
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/ktime.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/udp.h>
#include <linux/debugfs.h>
#include <net/net_namespace.h>
#include <net/netfilter/nf_conntrack.h>
#include <net/netfilter/nf_conntrack_zones.h>
#include "hnat.h"
#include "hnat_diag.h"

#define HNAT_DIAG_ENTRIES 32
#define HNAT_DIAG_MAX_AGE_MS 60000

struct hnat_diag_row {
	struct hnat_diag_key key;
	u64 seen_ms, events, tunnel_generation, foe_generation;
	u32 ppe, slot, reason;
	int candidate_ppe, candidate_slot;
	int tunnel, classifier, error;
	char function[48];
	u32 line;
	u32 npu_reason;
	int npu_error;
	bool npu_observed;
	u8 paths;
	bool used, linked;
};
static DEFINE_SPINLOCK(diag_lock);
static struct hnat_diag_row diag_rows[HNAT_DIAG_ENTRIES];
static bool diag_has_rows;
static const char *const reasons[] = {
	"eligible", "parameters", "sync_pending", "npu_not_ready", "security_policy",
	"invalid_metadata", "unsupported_protocol", "parameter_error", "no_tunnel",
	"no_memory", "mark_excluded", "algorithm_excluded", "no_mac_header",
	"no_flow_hash", "no_route", "unsupported_device", "vlan_policy", "dpi_wait",
	"bind_threshold", "fragment", "mtu", "account_owner", "account_unavailable",
	"prefill", "bound", "decap_pending", "rejected",
};

static void diag_ct_key(struct hnat_diag_key *key, struct nf_conn *ct, u8 info)
{
	const struct nf_conntrack_zone *zone = nf_ct_zone(ct);

	key->tuple = ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple;
	key->ct_id = nf_ct_get_id(ct);
	key->netns = nf_ct_net(ct)->ns.inum;
	key->zone = zone->id;
	key->zone_dir = zone->dir;
	key->zone_flags = zone->flags;
	key->direction = CTINFO2DIR(info);
	key->tracked = true;
}

static bool diag_key(struct sk_buff *skb, struct hnat_diag_key *key)
{
	enum ip_conntrack_info info;
	struct nf_conn *ct = nf_ct_get(skb, &info);
	struct iphdr ip, *iph;
	__be16 ports[2], *p;
	int offset = skb_network_offset(skb);

	memset(key, 0, sizeof(*key));
	if (ct && !nf_ct_is_template(ct)) {
		diag_ct_key(key, ct, info);
		return true;
	}
	/* Untracked rows are packet tuples; no reverse-direction inference. */
	if (offset < 0)
		return false;
	iph = skb_header_pointer(skb, offset, sizeof(ip), &ip);
	if (!iph || iph->version != 4 || iph->ihl < 5)
		return false;
	key->tuple.src.l3num = AF_INET;
	key->tuple.src.u3.ip = iph->saddr;
	key->tuple.dst.u3.ip = iph->daddr;
	key->tuple.dst.protonum = iph->protocol;
	if (iph->protocol == IPPROTO_TCP || iph->protocol == IPPROTO_UDP) {
		p = skb_header_pointer(skb, offset + iph->ihl * 4, sizeof(ports), ports);
		if (!p)
			return false;
		key->tuple.src.u.all = p[0];
		key->tuple.dst.u.all = p[1];
	}
	return true;
}

static bool same_key(const struct hnat_diag_key *a, const struct hnat_diag_key *b)
{
	return a->tracked == b->tracked && a->ct_id == b->ct_id &&
		a->netns == b->netns && a->zone == b->zone &&
		a->zone_dir == b->zone_dir && a->zone_flags == b->zone_flags &&
		a->direction == b->direction && nf_ct_tuple_equal(&a->tuple, &b->tuple);
}

static struct hnat_diag_row *diag_row(const struct hnat_diag_key *key, bool create)
{
	struct hnat_diag_row *oldest = &diag_rows[0];
	int i;

	for (i = 0; i < HNAT_DIAG_ENTRIES; i++) {
		if (diag_rows[i].used && same_key(&diag_rows[i].key, key))
			return &diag_rows[i];
		if (!diag_rows[i].used || diag_rows[i].seen_ms < oldest->seen_ms)
			oldest = &diag_rows[i];
	}
	if (!create)
		return NULL;
	memset(oldest, 0, sizeof(*oldest));
	oldest->key = *key;
	oldest->tunnel = oldest->classifier = -1;
	oldest->candidate_ppe = oldest->candidate_slot = -1;
	oldest->used = true;
	WRITE_ONCE(diag_has_rows, true);
	return oldest;
}

static bool tunnel_candidate(struct sk_buff *skb)
{
	struct iphdr ip, *iph;
	struct ipv6hdr ip6, *ip6h;
	__be16 ports[2], *p;
	int offset;

	if (IS_SPACE_AVAILABLE_HEAD(skb) && is_magic_tag_valid(skb) &&
	    (skb_hnat_tops(skb) || skb_hnat_is_decap(skb)))
		return true;
	offset = skb_network_offset(skb);
	if (offset < 0)
		return false;
	iph = skb_header_pointer(skb, offset, sizeof(ip), &ip);
	if (!iph)
		return false;
	if (iph->version == 6) {
		ip6h = skb_header_pointer(skb, offset, sizeof(ip6), &ip6);
		if (!ip6h)
			return false;
		if (ip6h->nexthdr == IPPROTO_GRE)
			return true;
		if (ip6h->nexthdr != IPPROTO_UDP)
			return false;
		offset += sizeof(ip6);
		goto ports;
	}
	if (iph->version != 4 || iph->ihl < 5)
		return false;
	if (iph->protocol == IPPROTO_GRE)
		return true;
	if (iph->protocol != IPPROTO_UDP)
		return false;
	offset += iph->ihl * 4;
ports:
	p = skb_header_pointer(skb, offset, sizeof(ports), ports);
	return p && (p[0] == htons(4789) || p[1] == htons(4789) ||
		     p[0] == htons(1701) || p[1] == htons(1701));
}

void hnat_diag_note_at(struct sk_buff *skb, enum hnat_diag_reason reason, int error,
		       u8 path, int tunnel, u64 tunnel_generation, int classifier,
		       const char *function, unsigned int line)
{
	struct hnat_diag_key key;
	struct hnat_diag_row *r;
	unsigned long flags;
	bool candidate, metadata;
	int ppe = -1, slot = -1;

	if (!skb || (unsigned int)reason >= HNAT_DIAG_REASON_MAX)
		return;
	metadata = IS_SPACE_AVAILABLE_HEAD(skb) && is_magic_tag_valid(skb);
	if (metadata) {
		if (!path && skb_hnat_tops(skb))
			path = skb_hnat_is_decap(skb) ? 2 : 1;
#if defined(CONFIG_MEDIATEK_NETSYS_V2) || defined(CONFIG_MEDIATEK_NETSYS_V3)
		if (skb_hnat_entry(skb) < 0x7fff &&
#else
		if (skb_hnat_entry(skb) < 0x3fff &&
#endif
		    skb_hnat_ppe(skb) < MAX_PPE_NUM && skb_hnat_ppe(skb) < CFG_PPE_NUM) {
			ppe = skb_hnat_ppe(skb);
			slot = skb_hnat_entry(skb);
		}
	}
	candidate = path || tunnel >= 0 || tunnel_candidate(skb);
	if (!candidate && !READ_ONCE(diag_has_rows))
		return;
	if (!diag_key(skb, &key))
		return;
	spin_lock_irqsave(&diag_lock, flags);
	r = diag_row(&key, candidate);
	if (r) {
		r->events++;
		r->seen_ms = ktime_to_ms(ktime_get());
		r->reason = reason;
		r->error = error;
		/* Metadata is a candidate location only; it never validates an owner. */
		r->candidate_ppe = ppe;
		r->candidate_slot = slot;
		strscpy(r->function, function, sizeof(r->function));
		r->line = line;
		if (tunnel >= 0) {
			if (r->tunnel != tunnel || r->tunnel_generation != tunnel_generation) {
				r->linked = false;
				r->paths = 0;
				r->npu_observed = false;
			}
			r->tunnel = tunnel;
			r->tunnel_generation = tunnel_generation;
			r->classifier = classifier;
		}
		r->paths |= path & 3;
		if (!strncmp(function, "mtk_npu_", 8) || !strncmp(function, "npu_", 4)) {
			r->npu_observed = true;
			r->npu_reason = reason;
			r->npu_error = error;
		}
	}
	spin_unlock_irqrestore(&diag_lock, flags);
}
EXPORT_SYMBOL_GPL(hnat_diag_note_at);

void hnat_diag_bind(struct nf_conn *ct, u8 info, u32 ppe, u32 slot,
		    u64 generation, const struct foe_entry *entry, bool prefill)
{
	struct hnat_diag_key key = {};
	struct hnat_diag_row *r;
	unsigned long flags;
	bool candidate;

	if (!ct || nf_ct_is_template(ct))
		return;
	diag_ct_key(&key, ct, info);
	candidate = hnat_diag_npu_entry(entry);
	spin_lock_irqsave(&diag_lock, flags);
	r = diag_row(&key, candidate);
	if (r) {
		r->events++;
		r->seen_ms = ktime_to_ms(ktime_get());
		r->ppe = ppe;
		r->slot = slot;
		r->foe_generation = generation;
		r->linked = true;
		r->reason = prefill ? HNAT_DIAG_PREFILL : HNAT_DIAG_BOUND;
		r->error = 0;
		strscpy(r->function, "hnat_account_commit", sizeof(r->function));
		r->line = 0;
	}
	spin_unlock_irqrestore(&diag_lock, flags);
}

bool hnat_diag_npu_entry(const struct foe_entry *entry)
{
#if defined(CONFIG_MEDIATEK_NETSYS_V3)
	/* NETSYSv3 TPORT selectors: TDMA direct=4, TDMA via QDMA=5. */
	return IS_IPV4_HNAPT(entry) && entry->ipv4_hnapt.tops_entry &&
		(entry->ipv4_hnapt.tport_id == 4 || entry->ipv4_hnapt.tport_id == 5);
#else
	return false;
#endif
}

/* The caller holds the native tunnel lease and a reference to the INNER CT. */
void hnat_diag_link_ct(struct nf_conn *ct, u8 info, u32 tunnel,
		       u64 generation, u8 path)
{
	struct hnat_diag_key key = {};
	struct hnat_diag_row *row;
	unsigned long flags;

	if (!ct || nf_ct_is_template(ct) || tunnel >= 32 || !generation)
		return;
	diag_ct_key(&key, ct, info);
	spin_lock_irqsave(&diag_lock, flags);
	row = diag_row(&key, true);
	if (row->tunnel != tunnel || row->tunnel_generation != generation) {
		row->linked = false;
		row->classifier = -1;
		row->paths = 0;
		row->npu_observed = false;
		row->reason = HNAT_DIAG_PARAMETERS;
		row->error = 0;
	}
	row->tunnel = tunnel;
	row->tunnel_generation = generation;
	row->paths |= path & 3;
	row->seen_ms = ktime_to_ms(ktime_get());
	spin_unlock_irqrestore(&diag_lock, flags);
}

void hnat_diag_note_ct_at(struct nf_conn *ct, u8 info,
			  enum hnat_diag_reason reason, int error, u8 path,
			  const char *function, unsigned int line)
{
	struct hnat_diag_key key = {};
	struct hnat_diag_row *row;
	unsigned long flags;

	if (!ct || nf_ct_is_template(ct) || (unsigned int)reason >= HNAT_DIAG_REASON_MAX)
		return;
	diag_ct_key(&key, ct, info);
	spin_lock_irqsave(&diag_lock, flags);
	row = diag_row(&key, true);
	row->events++;
	row->seen_ms = ktime_to_ms(ktime_get());
	row->reason = reason;
	row->error = error;
	row->paths |= path & 3;
	strscpy(row->function, function, sizeof(row->function));
	row->line = line;
	spin_unlock_irqrestore(&diag_lock, flags);
}

int hnat_diag_show(struct seq_file *s, void *unused)
{
	struct hnat_diag_row row;
	struct hnat_diag_sample sample;
	unsigned long flags;
	u64 now = ktime_to_ms(ktime_get());
	bool comma = false;
	int i;

	seq_printf(s, "{\"schema\":1,\"source\":\"hnat_owner_ledger\",\"max_entries\":32,"
		   "\"sampled_ms\":\"%llu\",\"max_age_ms\":60000,\"entries\":[", now);
	for (i = 0; i < HNAT_DIAG_ENTRIES; i++) {
		spin_lock_irqsave(&diag_lock, flags);
		row = diag_rows[i];
		spin_unlock_irqrestore(&diag_lock, flags);
		if (!row.used || now < row.seen_ms || now - row.seen_ms > HNAT_DIAG_MAX_AGE_MS)
			continue;
		memset(&sample, 0, sizeof(sample));
		/* Drop the diagnostic lock before taking the owner-ledger lock. */
		if (row.linked)
			hnat_account_diag_sample(hnat_priv, &row.key, row.ppe, row.slot,
					 row.foe_generation, &sample);
		seq_printf(s, "%s{\"family\":%u,\"protocol\":%u,\"ct_id\":%u,\"netns\":%u,"
			   "\"zone\":%u,\"tracked\":%s,\"reply\":%s,\"source\":\"",
			   comma ? "," : "", row.key.tuple.src.l3num,
			   row.key.tuple.dst.protonum, row.key.ct_id, row.key.netns,
			   row.key.zone, row.key.tracked ? "true" : "false",
			   row.key.direction == IP_CT_DIR_REPLY ? "true" : "false");
		if (row.key.tuple.src.l3num == AF_INET) {
			seq_printf(s, "%pI4\",\"destination\":\"%pI4", &row.key.tuple.src.u3.ip,
				   &row.key.tuple.dst.u3.ip);
		} else {
			seq_printf(s, "%pI6c\",\"destination\":\"%pI6c", &row.key.tuple.src.u3.in6,
				   &row.key.tuple.dst.u3.in6);
		}
		seq_printf(s, "\",\"decision_site\":\"%s\",\"line\":%u,\"source_port\":%u,\"destination_port\":%u,\"paths\":%u,"
			   "\"tunnel\":%d,\"tunnel_generation\":\"%llu\",\"classifier\":%d,"
			   "\"events\":\"%llu\",\"age_ms\":%llu,\"reason\":\"%s\",\"error\":%d,"
			   "\"binding_verified\":%s,\"hardware_count_available\":%s,"
			   "\"npu_path\":%s,\"counter_unit\":\"ppe_mib_bytes\"",
			   row.function, row.line, ntohs(row.key.tuple.src.u.all), ntohs(row.key.tuple.dst.u.all), row.paths,
			   row.tunnel, row.tunnel_generation, row.classifier, row.events,
			   now - row.seen_ms, reasons[row.reason], row.error,
			   sample.valid ? "true" : "false", sample.available ? "true" : "false",
			   sample.npu_path ? "true" : "false");
		if (row.candidate_ppe >= 0)
			seq_printf(s, ",\"candidate_ppe\":%d,\"candidate_foe\":%d",
				   row.candidate_ppe, row.candidate_slot);
		if (row.linked)
			seq_printf(s, ",\"ppe\":%u,\"foe\":%u,\"foe_generation\":\"%llu\"",
				   row.ppe, row.slot, row.foe_generation);
		if (row.npu_observed)
			seq_printf(s, ",\"npu_reason\":\"%s\",\"npu_error\":%d",
				   reasons[row.npu_reason], row.npu_error);
		if (sample.available)
			seq_printf(s, ",\"hardware_bytes\":\"%llu\",\"hardware_packets\":\"%llu\","
				   "\"counter_sampled_ms\":\"%llu\"", sample.bytes, sample.packets,
				   sample.sampled_ms);
		seq_puts(s, "}");
		comma = true;
	}
	seq_puts(s, "]}\n");
	return 0;
}

DEFINE_SHOW_ATTRIBUTE(hnat_diag);
static struct dentry *diag_file;

void hnat_diag_create(struct dentry *root)
{
	unsigned long flags;

	spin_lock_irqsave(&diag_lock, flags);
	memset(diag_rows, 0, sizeof(diag_rows));
	WRITE_ONCE(diag_has_rows, false);
	spin_unlock_irqrestore(&diag_lock, flags);
	diag_file = debugfs_create_file("flow_diagnostics", 0400, root, NULL, &hnat_diag_fops);
}

void hnat_diag_remove(void)
{
	debugfs_remove(diag_file);
	diag_file = NULL;
}
