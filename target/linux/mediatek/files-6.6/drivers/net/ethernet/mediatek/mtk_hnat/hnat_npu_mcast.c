// SPDX-License-Identifier: GPL-2.0-only
/* TOPS multicast uses the public MTK client/table ABI. MDB membership is
 * learned in process context; packet binding only consumes a verified cache.
 */
#include <linux/etherdevice.h>
#include <linux/if_bridge.h>
#include <linux/inet.h>
#include <linux/inetdevice.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/netlink.h>
#include <linux/rcupdate.h>
#include <linux/rtnetlink.h>
#include <linux/slab.h>
#include <linux/sizes.h>
#include <linux/workqueue.h>
#include <net/addrconf.h>
#include <net/ip.h>
#include <net/netfilter/nf_conntrack.h>
#include <net/sock.h>
#include <net/switchdev.h>
#include "hnat.h"
#include "hnat_npu_mcast.h"

#define NPU_MC_GROUPS 64
#define NPU_MC_MEMBERS 64
#define NPU_MC_CLIENTS 10
#define NPU_MC_INVALID 0xff
#define NPU_MC_TAG 30
#define NPU_MC_PORT 10 /* Public MT7988 PSE TDMA destination. */
#define NPU_MC_LOST 0
#define NPU_MC_RETRY (HZ / 5 + 1)

struct npu_mc_group {
	__be16 proto;
	union { __be32 ip; struct in6_addr ip6; } dst;
	u32 bridge;
	u32 member[NPU_MC_MEMBERS];
	u32 generation;
	unsigned long retry_at;
	u16 vid;
	u8 npu_index;
	bool used, unsupported, dirty, ready;
};

struct npu_mc_ctx {
	struct mtk_hnat *h;
	const struct npu_hnat_mcast_ops *ops;
	struct socket *sock;
	u8 *receive_buffer;
	struct workqueue_struct *queue;
	struct delayed_work work;
	struct notifier_block netdev_nb, switchdev_nb;
	struct npu_mc_group groups[NPU_MC_GROUPS];
	u8 *lease[MAX_PPE_NUM];
	u32 lease_count[NPU_MC_GROUPS], lease_total;
	u32 slots, ppes, sequence;
	unsigned long flags;
	unsigned long refresh_at, dump_deadline;
	bool stopping, paused, dumping, overflow;
};

static DEFINE_MUTEX(npu_mc_admin);
static struct npu_mc_ctx __rcu *npu_mc_active;

static bool npu_mc_entry(const struct foe_entry *entry)
{
	if (entry->bfib1.state != BIND)
		return false;
	if (IS_IPV4_HNAPT(entry) || IS_IPV4_HNAT(entry))
		return entry->ipv4_hnapt.tops_entry == NPU_MC_TAG;
	if (IS_IPV6_5T_ROUTE(entry))
		return entry->ipv6_5t_route.tops_entry == NPU_MC_TAG;
	return false;
}

/* The owner ledger is retired through the normal deletion path. Split scans
 * into bounded critical sections; no MCU callback runs under entry_lock.
 */
static void npu_mc_invalidate(struct npu_mc_ctx *ctx, int group)
{
	u32 ppe, slot, batch;
	u64 start;

	spin_lock_bh(&ctx->h->entry_lock);
	if (!(group < 0 ? ctx->lease_total : ctx->lease_count[group])) {
		spin_unlock_bh(&ctx->h->entry_lock);
		return;
	}
	spin_unlock_bh(&ctx->h->entry_lock);
	for (ppe = 0; ppe < ctx->ppes; ppe++) {
		for (slot = 0; slot < ctx->slots;) {
			spin_lock_bh(&ctx->h->entry_lock);
			start = ktime_get_ns();
			for (batch = 0; slot < ctx->slots && batch < 64; slot++, batch++) {
				u8 owner = ctx->lease[ppe][slot];

				if (owner == NPU_MC_INVALID || (group >= 0 && owner != group))
					continue;
				ctx->lease[ppe][slot] = NPU_MC_INVALID;
				ctx->lease_count[owner]--;
				ctx->lease_total--;
				if (ctx->h->foe_table_cpu[ppe] &&
				    npu_mc_entry(&ctx->h->foe_table_cpu[ppe][slot]))
					entry_delete(ppe, slot);
				if (ktime_get_ns() - start >= 2000000) {
					slot++;
					break;
				}
			}
			spin_unlock_bh(&ctx->h->entry_lock);
			cond_resched();
		}
	}
}

static void npu_mc_dirty(struct npu_mc_group *g)
{
	g->ready = false;
	g->dirty = true;
	g->generation++;
}

static bool npu_mc_address_equal(const struct npu_mc_group *a,
				 const struct npu_mc_group *b)
{
	return a->proto == b->proto &&
		!memcmp(&a->dst, &b->dst, sizeof(a->dst));
}

static struct npu_mc_group *npu_mc_find(struct npu_mc_ctx *ctx,
				      const struct npu_mc_group *key, bool create)
{
	struct npu_mc_group *free = NULL;
	u32 i;

	for (i = 0; i < NPU_MC_GROUPS; i++) {
		struct npu_mc_group *g = &ctx->groups[i];

		if (!g->used) {
			if (!free)
				free = g;
			continue;
		}
		if (g->bridge == key->bridge && g->vid == key->vid &&
		    npu_mc_address_equal(g, key))
			return g;
	}
	if (!create)
		return NULL;
	if (!free) {
		ctx->overflow = true;
		return NULL;
	}
	*free = *key;
	free->used = true;
	free->npu_index = NPU_MC_INVALID;
	npu_mc_dirty(free);
	for (i = 0; i < NPU_MC_GROUPS; i++)
		if (ctx->groups[i].used && npu_mc_address_equal(&ctx->groups[i], key))
			npu_mc_dirty(&ctx->groups[i]);
	return free;
}

static int npu_mc_mdb(struct npu_mc_ctx *ctx, struct nlmsghdr *nlh,
			 const struct br_port_msg *bpm, struct nlattr *info)
{
	struct npu_mc_group key = {}, *g;
	struct br_mdb_entry entry;
	struct nlattr *attr;
	int remaining, member = -1, free = -1;
	bool unsupported = false;
	u32 i;

	if (nla_len(info) < sizeof(entry))
		return -EINVAL;
	memcpy(&entry, nla_data(info), sizeof(entry));
	if (entry.addr.proto != htons(ETH_P_IP) && entry.addr.proto != htons(ETH_P_IPV6))
		return 0;
	key.proto = entry.addr.proto;
	key.bridge = bpm->ifindex;
	key.vid = entry.vid;
	if (key.proto == htons(ETH_P_IP)) {
		key.dst.ip = entry.addr.u.ip4;
		if (!ipv4_is_multicast(key.dst.ip) || ipv4_is_local_multicast(key.dst.ip))
			return 0;
	} else {
		key.dst.ip6 = entry.addr.u.ip6;
		if (!ipv6_addr_is_multicast(&key.dst.ip6) ||
		    ipv6_addr_is_ll_all_nodes(&key.dst.ip6) ||
		    (key.dst.ip6.s6_addr[1] & 0xf) <= 2)
			return 0;
	}
	/* This firmware ABI cannot express source-filter exceptions or a CPU
	 * listener. Keep the complete group in software in those cases.
	 */
	unsupported = entry.vid || entry.ifindex == bpm->ifindex ||
		(entry.flags & MDB_FLAGS_BLOCKED);
	nla_for_each_attr(attr, (void *)((u8 *)nla_data(info) + NLA_ALIGN(sizeof(entry))),
			  nla_len(info) - NLA_ALIGN(sizeof(entry)), remaining) {
		if (nla_type(attr) == MDBA_MDB_EATTR_SOURCE ||
		    (nla_type(attr) == MDBA_MDB_EATTR_SRC_LIST && nla_len(attr)))
			unsupported = true;
		if (nla_type(attr) == MDBA_MDB_EATTR_GROUP_MODE &&
		    (nla_len(attr) != 1 || nla_get_u8(attr) != MCAST_EXCLUDE))
			unsupported = true;
	}
	if (remaining)
		return -EINVAL;
	spin_lock_bh(&ctx->h->entry_lock);
	g = npu_mc_find(ctx, &key, nlh->nlmsg_type == RTM_NEWMDB);
	if (!g)
		goto unlock;
	for (i = 0; i < NPU_MC_MEMBERS; i++) {
		if (g->member[i] == entry.ifindex)
			member = i;
		if (!g->member[i] && free < 0)
			free = i;
	}
	if (unsupported)
		g->unsupported = true;
	if (nlh->nlmsg_type == RTM_NEWMDB) {
		if (member < 0 && free >= 0)
			g->member[free] = entry.ifindex;
		else if (member < 0)
			ctx->overflow = true;
	} else if (!unsupported && member >= 0) {
		g->member[member] = 0;
	}
	npu_mc_dirty(g);
unlock:
	spin_unlock_bh(&ctx->h->entry_lock);
	return 0;
}

static int npu_mc_message(struct npu_mc_ctx *ctx, struct nlmsghdr *nlh)
{
	struct br_port_msg *bpm;
	struct nlattr *mdb, *group, *info;
	int rem1, rem2, ret;

	/* Old dump fragments must not repopulate a newer membership epoch. */
	if (nlh->nlmsg_seq && nlh->nlmsg_seq != ctx->sequence)
		return 0;
	if (nlh->nlmsg_type == NLMSG_OVERRUN)
		return -ENOBUFS;
	if (nlh->nlmsg_type == RTM_NEWLINK || nlh->nlmsg_type == RTM_DELLINK) {
		u32 i;

		spin_lock_bh(&ctx->h->entry_lock);
		for (i = 0; i < NPU_MC_GROUPS; i++)
			if (ctx->groups[i].used)
				npu_mc_dirty(&ctx->groups[i]);
		spin_unlock_bh(&ctx->h->entry_lock);
		return 0;
	}
	if (nlh->nlmsg_type == NLMSG_DONE && nlh->nlmsg_seq == ctx->sequence) {
		if (nlh->nlmsg_flags & NLM_F_DUMP_INTR)
			return -EAGAIN;
		if (nlmsg_len(nlh) >= sizeof(int)) {
			int error;

			memcpy(&error, nlmsg_data(nlh), sizeof(error));
			if (error)
				return error;
		}
		WRITE_ONCE(ctx->dumping, false);
		return 0;
	}
	if (nlh->nlmsg_type == NLMSG_ERROR) {
		if (nlmsg_len(nlh) < sizeof(struct nlmsgerr))
			return -EINVAL;
		return ((struct nlmsgerr *)nlmsg_data(nlh))->error;
	}
	if (nlh->nlmsg_type != RTM_NEWMDB && nlh->nlmsg_type != RTM_DELMDB)
		return 0;
	if (nlmsg_len(nlh) < sizeof(*bpm))
		return -EINVAL;
	bpm = nlmsg_data(nlh);
	/* Router-port replication is not represented by a plain MDB member.
	 * A router notification forces a fresh snapshot and software forwarding.
	 */
	info = nlmsg_find_attr(nlh, sizeof(*bpm), MDBA_ROUTER);
	if (info && nla_len(info)) {
		u32 i;

		spin_lock_bh(&ctx->h->entry_lock);
		ctx->overflow = true;
		for (i = 0; i < NPU_MC_GROUPS; i++)
			if (ctx->groups[i].used)
				npu_mc_dirty(&ctx->groups[i]);
		spin_unlock_bh(&ctx->h->entry_lock);
	}
	mdb = nlmsg_find_attr(nlh, sizeof(*bpm), MDBA_MDB);
	if (!mdb)
		return 0;
	nla_for_each_nested(group, mdb, rem1) {
		if (nla_type(group) != MDBA_MDB_ENTRY)
			continue;
		nla_for_each_nested(info, group, rem2) {
			if (nla_type(info) != MDBA_MDB_ENTRY_INFO)
				continue;
			ret = npu_mc_mdb(ctx, nlh, bpm, info);
			if (ret)
				return ret;
		}
		if (rem2)
			return -EINVAL;
	}
	return rem1 ? -EINVAL : 0;
}

/* Use recvmsg so netlink can release receive accounting and continue a
 * multipart dump. Dequeuing sk_receive_queue directly truncates large dumps.
 */
static void npu_mc_receive(struct npu_mc_ctx *ctx)
{
	struct sockaddr_nl addr;
	struct nlmsghdr *nlh;
	struct kvec iov = { .iov_base = ctx->receive_buffer, .iov_len = SZ_32K };
	int remaining, len, budget = 64;

	while (budget--) {
		struct msghdr msg = { .msg_name = &addr, .msg_namelen = sizeof(addr) };

		len = kernel_recvmsg(ctx->sock, &msg, &iov, 1, SZ_32K, MSG_DONTWAIT);
		if (len == -EAGAIN || len == -EWOULDBLOCK)
			break;
		if (len <= 0 || (msg.msg_flags & MSG_TRUNC)) {
			set_bit(NPU_MC_LOST, &ctx->flags);
			break;
		}
		if (msg.msg_namelen < sizeof(addr) || addr.nl_pid)
			continue;
		nlmsg_for_each_msg(nlh, (void *)ctx->receive_buffer, len, remaining)
			if (npu_mc_message(ctx, nlh))
				set_bit(NPU_MC_LOST, &ctx->flags);
		if (remaining)
			set_bit(NPU_MC_LOST, &ctx->flags);
	}
}

static int npu_mc_dump(struct npu_mc_ctx *ctx)
{
	struct {
		struct nlmsghdr nlh;
		struct br_port_msg bpm;
	} request = {
		.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct br_port_msg)),
		.nlh.nlmsg_type = RTM_GETMDB,
		.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP,
		.bpm.family = AF_BRIDGE,
	};
	struct sockaddr_nl addr = { .nl_family = AF_NETLINK };
	struct msghdr msg = { .msg_name = &addr, .msg_namelen = sizeof(addr) };
	struct kvec iov = { .iov_base = &request, .iov_len = sizeof(request) };
	int ret;

	request.nlh.nlmsg_seq = ++ctx->sequence;
	WRITE_ONCE(ctx->dumping, true);
	ctx->dump_deadline = jiffies + 5 * HZ;
	ret = kernel_sendmsg(ctx->sock, &msg, &iov, 1, sizeof(request));
	if (ret != sizeof(request)) {
		WRITE_ONCE(ctx->dumping, false);
		return ret < 0 ? ret : -EIO;
	}
	return 0;
}

static void npu_mc_data_ready(struct sock *sk)
{
	struct npu_mc_ctx *ctx;

	rcu_read_lock();
	ctx = rcu_dereference_sk_user_data(sk);
	if (ctx && !READ_ONCE(ctx->stopping) && !READ_ONCE(ctx->paused))
		mod_delayed_work(ctx->queue, &ctx->work, 0);
	rcu_read_unlock();
}

static void npu_mc_error(struct sock *sk)
{
	struct npu_mc_ctx *ctx;

	rcu_read_lock();
	ctx = rcu_dereference_sk_user_data(sk);
	if (ctx && !READ_ONCE(ctx->stopping) && !READ_ONCE(ctx->paused)) {
		set_bit(NPU_MC_LOST, &ctx->flags);
		mod_delayed_work(ctx->queue, &ctx->work, 0);
	}
	rcu_read_unlock();
}

static bool npu_mc_conflict(struct npu_mc_ctx *ctx, u32 index)
{
	u32 i;

	for (i = 0; i < NPU_MC_GROUPS; i++)
		if (i != index && ctx->groups[i].used &&
		    npu_mc_address_equal(&ctx->groups[i], &ctx->groups[index]))
			return true;
	return false;
}

static void npu_mc_program(struct npu_mc_ctx *ctx, u32 index)
{
	struct mcast_offload_info clients[NPU_MC_CLIENTS] = {};
	struct npu_mc_group snapshot;
	struct net_device *dev, *master;
	u32 i, n = 0, new_index = NPU_MC_INVALID;
	bool usable;
	bool members = false;
	int ret = 0;

	spin_lock_bh(&ctx->h->entry_lock);
	snapshot = ctx->groups[index];
	usable = !ctx->paused && !ctx->overflow && !ctx->dumping &&
		!snapshot.unsupported && !npu_mc_conflict(ctx, index);
	ctx->groups[index].ready = false;
	ctx->groups[index].dirty = false;
	spin_unlock_bh(&ctx->h->entry_lock);
	npu_mc_invalidate(ctx, index);
	for (i = 0; i < NPU_MC_MEMBERS; i++)
		members |= snapshot.member[i] != 0;
	if (snapshot.npu_index != NPU_MC_INVALID) {
		ret = ctx->ops->group_delete(snapshot.npu_index);
		if (ret && ret != -ENOENT) {
			new_index = snapshot.npu_index;
			goto publish;
		}
		ret = 0;
	}
	if (!usable || !ctx->ops->ready())
		goto publish;
	for (i = 0; i < NPU_MC_MEMBERS; i++) {
		struct mcast_offload_info *info;

		if (!snapshot.member[i])
			continue;
		if (n == NPU_MC_CLIENTS) {
			ret = -ENOSPC;
			break;
		}
		dev = dev_get_by_index(&init_net, snapshot.member[i]);
		if (!dev) {
			ret = -ENODEV;
			break;
		}
		rcu_read_lock();
		master = netdev_master_upper_dev_get_rcu(dev);
		usable = master && master->ifindex == snapshot.bridge &&
			netif_is_bridge_master(master) && netif_running(dev);
		rcu_read_unlock();
		info = &clients[n];
		ret = usable ? ctx->ops->resolve_client(dev, info) : -ENODEV;
		dev_put(dev);
		if (ret)
			break;
		info->is_ipv6 = snapshot.proto == htons(ETH_P_IPV6);
		if (info->is_ipv6) {
			info->dst.ipv6 = snapshot.dst.ip6;
			ipv6_eth_mc_map(&snapshot.dst.ip6, info->daddr);
		} else {
			info->dst.ip = snapshot.dst.ip;
			ip_eth_mc_map(snapshot.dst.ip, info->daddr);
		}
		n++;
	}
	if (ret || !n)
		goto publish;
	for (i = 0; i < n; i++) {
		clients[i].npu_grp_idx = NPU_MC_INVALID;
		ret = ctx->ops->npu_mcast_client_insert(&clients[i]);
		/* A failed insertion can retain an empty table if firmware
		 * cleanup also failed. Preserve that ownership for retry.
		 */
		if (clients[i].npu_grp_idx < NPU_MC_GROUPS)
			new_index = clients[i].npu_grp_idx;
		if (ret == -EEXIST)
			ret = 0;
		if (ret && ret != -EEXIST)
			break;
		if (clients[i].npu_grp_idx >= NPU_MC_GROUPS) {
			ret = -ERANGE;
			break;
		}
		new_index = clients[i].npu_grp_idx;
	}
	if (ret && new_index != NPU_MC_INVALID) {
		/* Keep an index whose firmware deletion failed for the next retry. */
		if (!ctx->ops->group_delete(new_index))
			new_index = NPU_MC_INVALID;
	}
publish:
	spin_lock_bh(&ctx->h->entry_lock);
	/* Even a stale generation must retain ownership of a newly allocated
	 * firmware slot, so the next pass can delete it before rebuilding.
	 */
	ctx->groups[index].npu_index = new_index;
	if (ctx->groups[index].generation == snapshot.generation && !ctx->paused) {
		ctx->groups[index].ready = !ret && new_index != NPU_MC_INVALID;
		if (ret) {
			ctx->groups[index].dirty = true;
			ctx->groups[index].retry_at = jiffies + HZ;
		}
		if (!members && !snapshot.unsupported && !ret) {
			ctx->groups[index].used = false;
			for (i = 0; i < NPU_MC_GROUPS; i++)
				if (ctx->groups[i].used &&
				    npu_mc_address_equal(&ctx->groups[i], &snapshot))
					npu_mc_dirty(&ctx->groups[i]);
		}
	} else {
		ctx->groups[index].dirty = true;
	}
	spin_unlock_bh(&ctx->h->entry_lock);
}

static void npu_mc_work(struct work_struct *work)
{
	struct npu_mc_ctx *ctx = container_of(to_delayed_work(work), struct npu_mc_ctx, work);
	u32 i;
	bool dirty;
	int ret;

	if (READ_ONCE(ctx->stopping) || READ_ONCE(ctx->paused))
		return;
	npu_mc_receive(ctx);
	if (READ_ONCE(ctx->dumping) && time_after_eq(jiffies, ctx->dump_deadline))
		set_bit(NPU_MC_LOST, &ctx->flags);
	if (time_after_eq(jiffies, ctx->refresh_at) && !READ_ONCE(ctx->dumping)) {
		bool refresh = READ_ONCE(ctx->overflow);

		spin_lock_bh(&ctx->h->entry_lock);
		for (i = 0; i < NPU_MC_GROUPS; i++)
			refresh |= ctx->groups[i].used && ctx->groups[i].unsupported;
		spin_unlock_bh(&ctx->h->entry_lock);
		if (refresh)
			set_bit(NPU_MC_LOST, &ctx->flags);
		ctx->refresh_at = jiffies + 30 * HZ;
	}
	if (test_and_clear_bit(NPU_MC_LOST, &ctx->flags)) {
		spin_lock_bh(&ctx->h->entry_lock);
		for (i = 0; i < NPU_MC_GROUPS; i++)
			ctx->groups[i].ready = false;
		spin_unlock_bh(&ctx->h->entry_lock);
		npu_mc_invalidate(ctx, -1);
		/* Never forget a live firmware group on a failed delete: it could
		 * otherwise be reused with stale destinations after the new dump.
		 */
		for (i = 0; i < NPU_MC_GROUPS; i++) {
			if (!ctx->groups[i].used || ctx->groups[i].npu_index == NPU_MC_INVALID)
				continue;
			ret = ctx->ops->group_delete(ctx->groups[i].npu_index);
			if (ret && ret != -ENOENT) {
				set_bit(NPU_MC_LOST, &ctx->flags);
				goto requeue;
			}
			ctx->groups[i].npu_index = NPU_MC_INVALID;
		}
		spin_lock_bh(&ctx->h->entry_lock);
		memset(ctx->groups, 0, sizeof(ctx->groups));
		ctx->overflow = false;
		spin_unlock_bh(&ctx->h->entry_lock);
		if (npu_mc_dump(ctx))
			set_bit(NPU_MC_LOST, &ctx->flags);
	}
	if (!READ_ONCE(ctx->paused) && !READ_ONCE(ctx->dumping) && ctx->ops->ready()) {
		for (i = 0; i < NPU_MC_GROUPS; i++) {
			spin_lock_bh(&ctx->h->entry_lock);
				dirty = ctx->groups[i].used && ctx->groups[i].dirty &&
					time_after_eq(jiffies, ctx->groups[i].retry_at);
			spin_unlock_bh(&ctx->h->entry_lock);
			if (dirty)
				npu_mc_program(ctx, i);
		}
	}
requeue:
	if (!READ_ONCE(ctx->stopping) && !READ_ONCE(ctx->paused))
		mod_delayed_work(ctx->queue, &ctx->work, NPU_MC_RETRY);
}

static int npu_mc_netdev(struct notifier_block *nb, unsigned long event, void *ptr)
{
	struct npu_mc_ctx *ctx = container_of(nb, struct npu_mc_ctx, netdev_nb);
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);
	u32 i, member;
	bool remove = event == NETDEV_UNREGISTER;

	if (dev_net(dev) != &init_net)
		return NOTIFY_DONE;
	if (event == NETDEV_CHANGEUPPER) {
		const struct netdev_notifier_changeupper_info *info = ptr;

		remove = !info->linking;
	}
	if (event != NETDEV_DOWN && event != NETDEV_UNREGISTER &&
	    event != NETDEV_CHANGEUPPER && event != NETDEV_UP && event != NETDEV_CHANGE)
		return NOTIFY_DONE;
	spin_lock_bh(&ctx->h->entry_lock);
	for (i = 0; i < NPU_MC_GROUPS; i++) {
		struct npu_mc_group *g = &ctx->groups[i];

		if (!g->used)
			continue;
		if (remove)
			for (member = 0; member < NPU_MC_MEMBERS; member++)
				if (g->bridge == dev->ifindex || g->member[member] == dev->ifindex)
					g->member[member] = 0;
		npu_mc_dirty(g);
	}
	spin_unlock_bh(&ctx->h->entry_lock);
	mod_delayed_work(ctx->queue, &ctx->work, 0);
	return NOTIFY_DONE;
}

static int npu_mc_switchdev(struct notifier_block *nb, unsigned long event, void *ptr)
{
	struct npu_mc_ctx *ctx = container_of(nb, struct npu_mc_ctx, switchdev_nb);
	const struct switchdev_notifier_port_attr_info *info = ptr;
	u32 i;

	if (event != SWITCHDEV_PORT_ATTR_SET || !info->attr)
		return NOTIFY_DONE;
	if (info->attr->id != SWITCHDEV_ATTR_ID_PORT_STP_STATE &&
	    info->attr->id != SWITCHDEV_ATTR_ID_BRIDGE_VLAN_FILTERING &&
	    info->attr->id != SWITCHDEV_ATTR_ID_BRIDGE_MC_DISABLED)
		return NOTIFY_DONE;
	spin_lock_bh(&ctx->h->entry_lock);
	for (i = 0; i < NPU_MC_GROUPS; i++)
		if (ctx->groups[i].used)
			npu_mc_dirty(&ctx->groups[i]);
	spin_unlock_bh(&ctx->h->entry_lock);
	mod_delayed_work(ctx->queue, &ctx->work, 0);
	/* Observe the bridge event without claiming to implement its switch. */
	return NOTIFY_DONE;
}

int mtk_npu_hnat_mcast_ops_register(const struct npu_hnat_mcast_ops *ops)
{
	struct sockaddr_nl addr = { .nl_family = AF_NETLINK, .nl_groups = RTMGRP_MDB | BIT(RTNLGRP_LINK - 1) };
	struct npu_mc_ctx *ctx;
	u32 i;
	int ret = -ENODEV;

	if (!ops || !ops->npu_mcast_client_insert || !ops->group_delete ||
	    !ops->resolve_client || !ops->ready)
		return -EINVAL;
	mutex_lock(&npu_mc_admin);
	if (rcu_access_pointer(npu_mc_active)) {
		ret = -EBUSY;
		goto unlock;
	}
	if (!hnat_priv || hnat_priv->data->version != MTK_HNAT_V3)
		goto unlock;
	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		ret = -ENOMEM;
		goto unlock;
	}
	ctx->receive_buffer = kmalloc(SZ_32K, GFP_KERNEL);
	if (!ctx->receive_buffer) {
		ret = -ENOMEM;
		goto free;
	}
	ctx->h = hnat_priv;
	ctx->ops = ops;
	ctx->refresh_at = jiffies + 30 * HZ;
	ctx->slots = ctx->h->foe_etry_num;
	ctx->ppes = min_t(u32, ctx->h->ppe_num, MAX_PPE_NUM);
	if (!ctx->slots || !ctx->ppes) {
		ret = -ENODEV;
		goto free;
	}
	for (i = 0; i < ctx->ppes; i++) {
		ctx->lease[i] = kmalloc(ctx->slots, GFP_KERNEL);
		if (!ctx->lease[i]) {
			ret = -ENOMEM;
			goto free;
		}
		memset(ctx->lease[i], NPU_MC_INVALID, ctx->slots);
	}
	INIT_DELAYED_WORK(&ctx->work, npu_mc_work);
	ctx->queue = alloc_ordered_workqueue("hnat_npu_mcast", WQ_MEM_RECLAIM);
	if (!ctx->queue) {
		ret = -ENOMEM;
		goto free;
	}
	ret = sock_create_kern(&init_net, AF_NETLINK, SOCK_RAW, NETLINK_ROUTE, &ctx->sock);
	if (ret)
		goto queue;
	rcu_assign_sk_user_data(ctx->sock->sk, ctx);
	ctx->sock->sk->sk_data_ready = npu_mc_data_ready;
	ctx->sock->sk->sk_error_report = npu_mc_error;
	ret = kernel_bind(ctx->sock, (struct sockaddr *)&addr, sizeof(addr));
	if (ret)
		goto socket;
	ctx->netdev_nb.notifier_call = npu_mc_netdev;
	ret = register_netdevice_notifier(&ctx->netdev_nb);
	if (ret)
		goto socket;
	ctx->switchdev_nb.notifier_call = npu_mc_switchdev;
	ret = register_switchdev_blocking_notifier(&ctx->switchdev_nb);
	if (ret) {
		unregister_netdevice_notifier(&ctx->netdev_nb);
		goto socket;
	}
	set_bit(NPU_MC_LOST, &ctx->flags);
	rcu_assign_pointer(npu_mc_active, ctx);
	mod_delayed_work(ctx->queue, &ctx->work, 0);
	mutex_unlock(&npu_mc_admin);
	return 0;
socket:
	WRITE_ONCE(ctx->stopping, true);
	rcu_assign_sk_user_data(ctx->sock->sk, NULL);
	synchronize_rcu();
	cancel_delayed_work_sync(&ctx->work);
	sock_release(ctx->sock);
queue:
	destroy_workqueue(ctx->queue);
free:
	for (i = 0; i < MAX_PPE_NUM; i++)
		kfree(ctx->lease[i]);
	kfree(ctx->receive_buffer);
	kfree(ctx);
unlock:
	mutex_unlock(&npu_mc_admin);
	return ret;
}
EXPORT_SYMBOL_GPL(mtk_npu_hnat_mcast_ops_register);

static void npu_mc_stop(struct npu_mc_ctx *ctx)
{
	u32 i;

	WRITE_ONCE(ctx->stopping, true);
	rcu_assign_pointer(npu_mc_active, NULL);
	rcu_assign_sk_user_data(ctx->sock->sk, NULL);
	unregister_switchdev_blocking_notifier(&ctx->switchdev_nb);
	unregister_netdevice_notifier(&ctx->netdev_nb);
	synchronize_rcu();
	cancel_delayed_work_sync(&ctx->work);
	sock_release(ctx->sock);
	npu_mc_invalidate(ctx, -1);
	for (i = 0; i < NPU_MC_GROUPS; i++)
		if (ctx->groups[i].used && ctx->groups[i].npu_index != NPU_MC_INVALID)
			ctx->ops->group_delete(ctx->groups[i].npu_index);
	destroy_workqueue(ctx->queue);
	for (i = 0; i < MAX_PPE_NUM; i++)
		kfree(ctx->lease[i]);
	kfree(ctx->receive_buffer);
	kfree(ctx);
}

int mtk_npu_hnat_mcast_ops_unregister(const struct npu_hnat_mcast_ops *ops)
{
	struct npu_mc_ctx *ctx;
	int ret = 0;

	mutex_lock(&npu_mc_admin);
	ctx = rcu_dereference_protected(npu_mc_active, lockdep_is_held(&npu_mc_admin));
	if (ctx && ctx->ops == ops)
		npu_mc_stop(ctx);
	else if (ctx)
		ret = -EINVAL;
	mutex_unlock(&npu_mc_admin);
	return ret;
}
EXPORT_SYMBOL_GPL(mtk_npu_hnat_mcast_ops_unregister);

void hnat_npu_mcast_remove(struct mtk_hnat *h)
{
	struct npu_mc_ctx *ctx;

	mutex_lock(&npu_mc_admin);
	ctx = rcu_dereference_protected(npu_mc_active, lockdep_is_held(&npu_mc_admin));
	if (ctx && ctx->h == h)
		npu_mc_stop(ctx);
	mutex_unlock(&npu_mc_admin);
}

void hnat_npu_mcast_pause(const struct npu_hnat_mcast_ops *ops)
{
	struct npu_mc_ctx *ctx;
	u32 i;

	mutex_lock(&npu_mc_admin);
	ctx = rcu_dereference_protected(npu_mc_active, lockdep_is_held(&npu_mc_admin));
	if (ctx && ctx->ops == ops) {
		WRITE_ONCE(ctx->paused, true);
		cancel_delayed_work_sync(&ctx->work);
		spin_lock_bh(&ctx->h->entry_lock);
		for (i = 0; i < NPU_MC_GROUPS; i++) {
			npu_mc_dirty(&ctx->groups[i]);
			ctx->groups[i].npu_index = NPU_MC_INVALID;
		}
		spin_unlock_bh(&ctx->h->entry_lock);
		npu_mc_invalidate(ctx, -1);
	}
	mutex_unlock(&npu_mc_admin);
}
EXPORT_SYMBOL_GPL(hnat_npu_mcast_pause);

void hnat_npu_mcast_resume(const struct npu_hnat_mcast_ops *ops)
{
	struct npu_mc_ctx *ctx;

	mutex_lock(&npu_mc_admin);
	ctx = rcu_dereference_protected(npu_mc_active, lockdep_is_held(&npu_mc_admin));
	if (ctx && ctx->ops == ops) {
		WRITE_ONCE(ctx->paused, false);
		mod_delayed_work(ctx->queue, &ctx->work, 0);
	}
	mutex_unlock(&npu_mc_admin);
}
EXPORT_SYMBOL_GPL(hnat_npu_mcast_resume);

bool hnat_npu_mcast_active(void)
{
	return rcu_access_pointer(npu_mc_active) != NULL;
}

/* entry_lock serializes cache validation and the caller's BIND publication. */
int hnat_npu_mcast_prepare(struct sk_buff *skb, const struct net_device *out,
			 struct foe_entry *entry, u32 ppe, u32 slot)
{
	struct npu_mc_group key = {}, *g;
	struct npu_mc_ctx *ctx;
	struct net_device *br;
	u32 j;
	int ret = -EOPNOTSUPP;

	lockdep_assert_held(&hnat_priv->entry_lock);
	rcu_read_lock();
	ctx = rcu_dereference(npu_mc_active);
	if (!ctx || READ_ONCE(ctx->paused) || ctx->overflow || READ_ONCE(ctx->dumping) ||
	    dev_net(out) != &init_net || ppe >= ctx->ppes || slot >= ctx->slots)
		goto unlock;
	br = netdev_master_upper_dev_get_rcu((struct net_device *)out);
	if (!br || !netif_is_bridge_master(br) || br_vlan_enabled(br) ||
	    skb_vlan_tag_present(skb) || eth_type_vlan(skb->protocol))
		goto unlock;
	key.bridge = br->ifindex;
	if (IS_IPV4_HNAPT(entry) || IS_IPV4_HNAT(entry)) {
		key.proto = htons(ETH_P_IP);
		key.dst.ip = htonl(entry->ipv4_hnapt.dip);
	} else if (IS_IPV6_5T_ROUTE(entry)) {
		key.proto = htons(ETH_P_IPV6);
		key.dst.ip6.s6_addr32[0] = htonl(entry->ipv6_5t_route.ipv6_dip0);
		key.dst.ip6.s6_addr32[1] = htonl(entry->ipv6_5t_route.ipv6_dip1);
		key.dst.ip6.s6_addr32[2] = htonl(entry->ipv6_5t_route.ipv6_dip2);
		key.dst.ip6.s6_addr32[3] = htonl(entry->ipv6_5t_route.ipv6_dip3);
	} else {
		goto unlock;
	}
	g = npu_mc_find(ctx, &key, false);
	if (!g || !g->ready)
		goto unlock;
	/* The firmware group is shared by all sources. Decline a sender which
	 * is itself a listener: per-ingress split horizon needs a separate ABI.
	 */
	for (j = 0; j < NPU_MC_MEMBERS; j++)
		if (g->member[j] && g->member[j] == skb->skb_iif)
			goto unlock;
	if (!skb->skb_iif || skb->skb_iif == br->ifindex)
		goto unlock;
	if (key.proto == htons(ETH_P_IP)) {
		entry->ipv4_hnapt.iblk2.dp = NPU_MC_PORT;
		entry->ipv4_hnapt.iblk2.mcast = 0;
		entry->ipv4_hnapt.iblk2.fqos = 0;
		entry->ipv4_hnapt.tport_id = 0;
		entry->ipv4_hnapt.tops_entry = NPU_MC_TAG;
		entry->ipv4_hnapt.etype = key.proto;
	} else {
		entry->ipv6_5t_route.iblk2.dp = NPU_MC_PORT;
		entry->ipv6_5t_route.iblk2.mcast = 0;
		entry->ipv6_5t_route.iblk2.fqos = 0;
		entry->ipv6_5t_route.tport_id = 0;
		entry->ipv6_5t_route.tops_entry = NPU_MC_TAG;
		entry->ipv6_5t_route.etype = key.proto;
	}
	entry->bfib1.vlan_layer = 0;
	entry->bfib1.vpm = 0;
	if (ctx->lease[ppe][slot] != g - ctx->groups) {
		u8 old = ctx->lease[ppe][slot];

		if (old != NPU_MC_INVALID)
			ctx->lease_count[old]--;
		else
			ctx->lease_total++;
		ctx->lease[ppe][slot] = g - ctx->groups;
		ctx->lease_count[g - ctx->groups]++;
	}
	ret = 0;
unlock:
	rcu_read_unlock();
	return ret;
}

void hnat_notify_mcast_sw_path(int index)
{
	struct npu_mc_ctx *ctx;
	u32 i;

	rcu_read_lock();
	ctx = rcu_dereference(npu_mc_active);
	if (ctx) {
		spin_lock_bh(&ctx->h->entry_lock);
		for (i = 0; i < NPU_MC_GROUPS; i++)
			if (ctx->groups[i].npu_index == index)
				npu_mc_dirty(&ctx->groups[i]);
		spin_unlock_bh(&ctx->h->entry_lock);
		mod_delayed_work(ctx->queue, &ctx->work, 0);
	}
	rcu_read_unlock();
}
EXPORT_SYMBOL_GPL(hnat_notify_mcast_sw_path);

void hnat_notify_mcast_hw_path(int index)
{
	hnat_notify_mcast_sw_path(index);
}
EXPORT_SYMBOL_GPL(hnat_notify_mcast_hw_path);
