// SPDX-License-Identifier: GPL-2.0-or-later
/* PPTP software/firmware sequence ownership and native socket rundown. */
#include <linux/hashtable.h>
#include <linux/ppp_channel.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <net/net_namespace.h>
#include <net/pptp.h>
#include <mtk_hnat/nf_hnat_mtk.h>
#include "npu/hwspinlock.h"
#include "npu/protocol/tunnel/pptp/pptp.h"
#include "npu/seq_gen.h"
#include "npu/tunnel.h"

struct pptp_seq {
	struct hlist_node node;
	struct pptp_offload_key key;
	u32 last;
	unsigned int users;
	int index;
	bool hardware, closed;
};
static DEFINE_HASHTABLE(pptp_sequences, 4);
static DEFINE_SPINLOCK(pptp_sequence_lock);
static DECLARE_WAIT_QUEUE_HEAD(pptp_close_wait);
static struct npu_tnl_type *pptp_type;
static bool pptp_online, pptp_stopping = true;

static struct pptp_seq *pptp_seq_find(u64 cookie)
{
	struct pptp_seq *seq;

	hash_for_each_possible(pptp_sequences, seq, node, cookie)
		if (seq->key.cookie == cookie)
			return seq;
	return NULL;
}

static bool pptp_key_equal(const struct pptp_offload_key *a,
			   const struct pptp_offload_key *b)
{
	return a->cookie == b->cookie && a->netns == b->netns &&
	       a->local_addr == b->local_addr && a->peer_addr == b->peer_addr &&
	       a->local_id == b->local_id && a->peer_id == b->peer_id;
}

static struct pptp_seq *pptp_seq_create(const struct pptp_offload_key *key, u32 last)
{
	struct pptp_seq *seq;

	if (pptp_stopping || key->netns != init_net.ns.inum || !key->cookie)
		return ERR_PTR(-EOPNOTSUPP);
	/* The scalar snapshot may have raced channel teardown. Verification
	 * while holding our map lock pairs with the native close callback.
	 */
	if (!pptp_offload_session_live(key))
		return ERR_PTR(-ENOTCONN);
	seq = kzalloc(sizeof(*seq), GFP_ATOMIC);
	if (!seq)
		return ERR_PTR(-ENOMEM);
	seq->key = *key;
	seq->last = last;
	seq->index = -1;
	hash_add(pptp_sequences, &seq->node, key->cookie);
	return seq;
}

static void pptp_seq_destroy(struct pptp_seq *seq)
{
	/* Firmware templates and native transmitters must have been drained. */
	if (seq->index >= 0)
		mtk_npu_seq_gen_free(seq->index);
	pptp_offload_callid_release(&seq->key);
	hash_del(&seq->node);
	kfree(seq);
	wake_up_all(&pptp_close_wait);
}

static int pptp_counter_next(struct pptp_seq *seq, u32 *value)
{
	int ret;

	ret = mtk_npu_hwspin_lock(HWSPINLOCK_GROUP_CLUST,
				HWSPINLOCK_CLUST_SLOT_PPTP_SEQ);
	if (ret)
		return ret;
	ret = mtk_npu_seq_gen_next_32(seq->index, value);
	mtk_npu_hwspin_unlock(HWSPINLOCK_GROUP_CLUST,
			       HWSPINLOCK_CLUST_SLOT_PPTP_SEQ);
	return ret;
}

static int pptp_native_next(const struct pptp_offload_key *key, u32 last, u32 *value)
{
	struct pptp_seq *seq;
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&pptp_sequence_lock, flags);
	seq = pptp_seq_find(key->cookie);
	if (!seq) {
		seq = pptp_seq_create(key, last);
		if (IS_ERR(seq)) {
			/* No hardware state exists; native software allocation is safe. */
			ret = -EOPNOTSUPP;
			goto out;
		}
	}
	if (seq->closed || !pptp_key_equal(&seq->key, key)) {
		ret = -ESHUTDOWN;
		goto out;
	}
	if (seq->hardware)
		ret = pptp_counter_next(seq, value);
	else
		*value = seq->last + 1;
	if (!ret)
		seq->last = *value;
out:
	spin_unlock_irqrestore(&pptp_sequence_lock, flags);
	return ret;
}

static int pptp_native_snapshot(const struct pptp_offload_key *key, u32 *value)
{
	struct pptp_seq *seq;
	unsigned long flags;
	int ret = -ENOENT;

	spin_lock_irqsave(&pptp_sequence_lock, flags);
	seq = pptp_seq_find(key->cookie);
	if (seq && pptp_key_equal(&seq->key, key)) {
		/* Unregister only after all firmware sequence consumers are stopped. */
		ret = seq->hardware ? -EBUSY : 0;
		if (!ret)
			*value = seq->last;
	}
	spin_unlock_irqrestore(&pptp_sequence_lock, flags);
	return ret;
}

static bool pptp_session_gone(u64 cookie)
{
	unsigned long flags;
	bool gone;

	spin_lock_irqsave(&pptp_sequence_lock, flags);
	gone = !pptp_seq_find(cookie);
	spin_unlock_irqrestore(&pptp_sequence_lock, flags);
	return gone;
}

static int pptp_native_close(const struct pptp_offload_key *key)
{
	struct pptp_seq *seq;
	unsigned long flags;
	bool pending = false;

	spin_lock_irqsave(&pptp_sequence_lock, flags);
	seq = pptp_seq_find(key->cookie);
	if (seq && pptp_key_equal(&seq->key, key)) {
		seq->closed = true;
		if (seq->users)
			pending = true;
		else
			pptp_seq_destroy(seq);
	}
	spin_unlock_irqrestore(&pptp_sequence_lock, flags);
	if (!pending)
		return 0;
	mtk_npu_tnl_info_retire_session(pptp_type, key->cookie);
	/* A failed deletion retains the native Call ID and counter reservation.
	 * Successful retry/recovery or final shutdown releases it later.
	 */
	return wait_event_timeout(pptp_close_wait,
				  pptp_session_gone(key->cookie), 2 * HZ) ? 0 : -ETIMEDOUT;
}

static const struct pptp_offload_ops pptp_native_ops = {
	.next = pptp_native_next,
	.snapshot = pptp_native_snapshot,
	.close = pptp_native_close,
};

static bool pptp_wire_matches(struct pptp_seq *seq, struct npu_params *params)
{
	return seq->key.local_addr == params->network.ip.sip &&
	       seq->key.peer_addr == params->network.ip.dip &&
	       seq->key.peer_id == ntohs(params->tunnel.pptp.ul_call_id);
}

#if IS_ENABLED(CONFIG_MTK_NPU_FLOW)
static int pptp_flow_session_prepare(struct npu_tnl_input *input,
				      struct npu_params *params)
{
	const struct net_device_path *path = input->flow.path;
	struct pptp_offload_key key = {};
	struct pptp_seq *seq;
	unsigned long flags;
	int ret = 0;

	if (!path || path->type != DEV_PATH_PPTP ||
	    dev_net(path->dev) != &init_net || params->network.type != NPU_NETWORK_IP)
		return -EINVAL;
	key.cookie = path->tunnel.pptp.cookie;
	key.netns = init_net.ns.inum;
	key.local_addr = path->tunnel.sip.ip;
	key.peer_addr = path->tunnel.dip.ip;
	key.local_id = path->tunnel.pptp.call_id;
	key.peer_id = path->tunnel.pptp.peer_call_id;
	spin_lock_irqsave(&pptp_sequence_lock, flags);
	seq = pptp_seq_find(key.cookie);
	if (!seq)
		seq = pptp_seq_create(&key, path->tunnel.pptp.seq_sent);
	if (IS_ERR(seq)) {
		ret = PTR_ERR(seq);
	} else if (seq->closed || !pptp_key_equal(&seq->key, &key) ||
		   !pptp_wire_matches(seq, params)) {
		ret = -ESTALE;
	} else {
		input->session_cookie = key.cookie;
	}
	spin_unlock_irqrestore(&pptp_sequence_lock, flags);
	return ret;
}
#endif

int mtk_npu_pptp_session_prepare(struct npu_tnl_input *input, struct npu_params *params)
{
	struct sk_buff *skb;
	struct ppp_tunnel_info info;
	struct pptp_offload_key key;
	struct pptp_seq *seq, *found = NULL;
	unsigned long flags;
	int bucket, ret = -ENOENT;

#if IS_ENABLED(CONFIG_MTK_NPU_FLOW)
	if (input->kind == NPU_TNL_INPUT_FLOW)
		return pptp_flow_session_prepare(input, params);
#endif
	if (input->kind != NPU_TNL_INPUT_HNAT)
		return -EINVAL;
	skb = input->hnat.skb;
	if (!skb || params->network.type != NPU_NETWORK_IP)
		return -EINVAL;
	if (skb_hnat_is_decap(skb)) {
		ret = ppp_netdev_get_tunnel_info(skb->dev, &info);
		if (ret)
			return ret;
		if (info.kind != PPP_TUNNEL_PPTP || info.version != 1 ||
		    htons(info.tunnel_id) != params->tunnel.pptp.dl_call_id ||
		    info.local_addr != params->network.ip.sip ||
		    info.peer_addr != params->network.ip.dip || dev_net(skb->dev) != &init_net)
			return -ESTALE;
		memset(&key, 0, sizeof(key));
		key.cookie = info.cookie;
		key.netns = init_net.ns.inum;
		key.local_addr = info.local_addr;
		key.peer_addr = info.peer_addr;
		key.local_id = info.tunnel_id;
		key.peer_id = info.peer_tunnel_id;
		spin_lock_irqsave(&pptp_sequence_lock, flags);
		seq = pptp_seq_find(info.cookie);
		if (!seq)
			seq = pptp_seq_create(&key, info.sequence);
		if (IS_ERR(seq)) {
			ret = PTR_ERR(seq);
			goto out;
		}
		if (seq->closed || !pptp_key_equal(&seq->key, &key)) {
			ret = -ESTALE;
			goto out;
		}
		found = seq;
	} else {
		/* Native transmit created this record before GRE encapsulation.
		 * Never re-enter the PPP transmit lock to obtain a socket snapshot.
		 */
		spin_lock_irqsave(&pptp_sequence_lock, flags);
		hash_for_each(pptp_sequences, bucket, seq, node) {
			if (!pptp_wire_matches(seq, params))
				continue;
			if (found || seq->closed) {
				ret = -EBUSY;
				goto out;
			}
			found = seq;
		}
	}
	if (!found || pptp_stopping) {
		ret = -EOPNOTSUPP;
		goto out;
	}
	input->session_cookie = found->key.cookie;
	params->tunnel.pptp.ul_call_id = htons(found->key.peer_id);
	params->tunnel.pptp.dl_call_id = htons(found->key.local_id);
	ret = 0;
out:
	spin_unlock_irqrestore(&pptp_sequence_lock, flags);
	return ret;
}

int mtk_npu_pptp_session_get(struct npu_tnl_info *info, u64 cookie,
			    struct npu_params *params)
{
	struct pptp_seq *seq;
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&pptp_sequence_lock, flags);
	seq = pptp_seq_find(cookie);
	if (!seq || seq->closed || pptp_stopping || !pptp_online ||
	    !pptp_wire_matches(seq, params) ||
	    htons(seq->key.local_id) != params->tunnel.pptp.dl_call_id ||
	    (info->session_cookie && info->session_cookie != cookie)) {
		ret = -ESTALE;
		goto out;
	}
	if (seq->index < 0) {
		ret = mtk_npu_seq_gen_alloc(&seq->index);
		if (ret) {
			seq->index = -1;
			goto out;
		}
		/* The hardware read returns/reserves its current value. */
		ret = mtk_npu_seq_gen_set_32(seq->index, seq->last + 1);
		if (ret) {
			mtk_npu_seq_gen_free(seq->index);
			seq->index = -1;
			goto out;
		}
		seq->hardware = true;
	}
	if (!info->session_cookie) {
		seq->users++;
		info->session_cookie = cookie;
	}
	params->tunnel.pptp.seq_gen_idx = seq->index;
out:
	spin_unlock_irqrestore(&pptp_sequence_lock, flags);
	return ret;
}

void mtk_npu_pptp_session_put(struct npu_tnl_info *info)
{
	struct pptp_seq *seq;
	unsigned long flags;

	spin_lock_irqsave(&pptp_sequence_lock, flags);
	seq = pptp_seq_find(info->session_cookie);
	info->session_cookie = 0;
	if (seq && seq->users) {
		seq->users--;
		if (seq->closed && !seq->users)
			pptp_seq_destroy(seq);
	}
	spin_unlock_irqrestore(&pptp_sequence_lock, flags);
}

void mtk_npu_pptp_seq_suspend_stalled(void)
{
	struct pptp_seq *seq;
	unsigned long flags;
	u32 next;
	int bucket;

	/* All firmware cores are stalled. A dead core may own its semaphore.
	 * Reserve an unused value before gating MMIO; software continues above it.
	 */
	spin_lock_irqsave(&pptp_sequence_lock, flags);
	pptp_online = false;
	hash_for_each(pptp_sequences, bucket, seq, node) {
		if (seq->hardware) {
			if (!mtk_npu_seq_gen_next_32(seq->index, &next))
				seq->last = next;
			seq->hardware = false;
		}
	}
	spin_unlock_irqrestore(&pptp_sequence_lock, flags);
}

void mtk_npu_pptp_seq_resume_stalled(void)
{
	struct pptp_seq *seq;
	unsigned long flags;
	int bucket;

	spin_lock_irqsave(&pptp_sequence_lock, flags);
	hash_for_each(pptp_sequences, bucket, seq, node)
		if (seq->index >= 0 && !mtk_npu_seq_gen_set_32(seq->index, seq->last + 1))
			seq->hardware = true;
	pptp_online = true;
	spin_unlock_irqrestore(&pptp_sequence_lock, flags);
}

int mtk_npu_pptp_seq_init(struct npu_tnl_type *type)
{
	int ret;

	if (!mtk_npu_seq_gen_available())
		return -ENODEV;
	pptp_type = type;
	pptp_stopping = false;
	ret = pptp_offload_ops_register(&pptp_native_ops);
	if (ret)
		pptp_stopping = true;
	return ret;
}

void mtk_npu_pptp_seq_deinit(void)
{
	struct pptp_seq *seq;
	struct hlist_node *tmp;
	unsigned long flags;
	int bucket;

	spin_lock_irqsave(&pptp_sequence_lock, flags);
	pptp_stopping = true;
	spin_unlock_irqrestore(&pptp_sequence_lock, flags);
	pptp_offload_ops_unregister(&pptp_native_ops);
	spin_lock_irqsave(&pptp_sequence_lock, flags);
	hash_for_each_safe(pptp_sequences, bucket, tmp, seq, node)
		pptp_seq_destroy(seq);
	spin_unlock_irqrestore(&pptp_sequence_lock, flags);
	pptp_type = NULL;
}
