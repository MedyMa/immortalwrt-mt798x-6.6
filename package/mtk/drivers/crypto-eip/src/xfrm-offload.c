// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2023 MediaTek Inc.
 *
 * Author: Chris.Chou <chris.chou@mediatek.com>
 *         Ren-Ting Wang <ren-ting.wang@mediatek.com>
 */

#include <linux/bitops.h>
#include <linux/module.h>
#include <net/gso.h>
#include <linux/spinlock.h>
#include <linux/netdevice.h>
#include <linux/udp.h>
#include <net/ip6_route.h>

#include <mtk_eth_soc.h>

#if IS_ENABLED(CONFIG_NET_MEDIATEK_HNAT)
#include <mtk_hnat/hnat.h>
#include <mtk_hnat/nf_hnat_mtk.h>
#elif defined(CONFIG_CRYPTO_OFFLOAD_INLINE_FLOWBLOCK)
#include <mtk_ppe.h>
#endif

#include <pce/cdrt.h>
#include <pce/cls.h>
#include <pce/netsys.h>

#include <crypto-eip/ddk/kit/iotoken/iotoken.h>
#include <crypto-eip/ddk/kit/iotoken/iotoken_ext.h>

#include "crypto-eip/crypto-eip.h"
#include "crypto-eip/ddk-wrapper.h"
#include "crypto-eip/internal.h"

static DEFINE_MUTEX(sa_setup_lock);
static LIST_HEAD(quarantined_states);
static void mtk_xfrm_params_free(struct mtk_xfrm_params *params);

static struct xfrm_params_list xfrm_params_list = {
	.list = LIST_HEAD_INIT(xfrm_params_list.list),
	.lock = __SPIN_LOCK_UNLOCKED(xfrm_params_list.lock),
};

#if IS_ENABLED(CONFIG_NET_MEDIATEK_HNAT)
extern int (*ra_sw_nat_hook_tx)(struct sk_buff *skb, int gmac_no);

static inline bool is_tops_tunnel(struct sk_buff *skb)
{
	return skb_hnat_tops(skb) && (ntohs(skb->protocol) == ETH_P_IP) &&
		(ip_hdr(skb)->protocol == IPPROTO_UDP ||
		 ip_hdr(skb)->protocol == IPPROTO_GRE);
}

static inline bool is_hnat_rate_reach(struct sk_buff *skb)
{
	return is_magic_tag_valid(skb) && (skb_hnat_reason(skb) == HIT_UNBIND_RATE_REACH);
}
#endif // HNAT

#if defined(CONFIG_CRYPTO_OFFLOAD_INLINE_FLOWBLOCK)
int mtk_crypto_xfrm_get_cdrt(struct xfrm_state *xs)
{
	struct mtk_xfrm_params *xfrm_params;
	struct mtk_xfrm_params *temp;

	xfrm_params = (struct mtk_xfrm_params *)xs->xso.offload_handle;

	if (!xfrm_params)
		return 0;

	spin_lock_bh(&xfrm_params_list.lock);
	list_for_each_entry(temp, &xfrm_params_list.list, node) {
		if (xfrm_params == temp) {
			spin_unlock_bh(&xfrm_params_list.lock);
			return xfrm_params->cdrt->idx;
		}
	}
	spin_unlock_bh(&xfrm_params_list.lock);

	return 0;
}
#endif // Flow_Block

bool mtk_crypto_decrypt_sec_path_setup(struct sk_buff *skb, u16 cdrt)
{
	struct mtk_xfrm_params *xfrm_params;
	struct xfrm_state *x = NULL;
	struct xfrm_offload *xo;
	struct sec_path *sp;

	if (!cdrt)
		return false;

	spin_lock_bh(&xfrm_params_list.lock);
	list_for_each_entry(xfrm_params, &xfrm_params_list.list, node) {
		if (!xfrm_params->dead && cdrt == xfrm_params->cdrt->idx) {
			if (xfrm_params->xs->km.state == XFRM_STATE_VALID &&
			    xfrm_params->xs->xso.type == XFRM_DEV_OFFLOAD_PACKET &&
			    xfrm_params->xs->xso.dir == XFRM_DEV_OFFLOAD_IN &&
			    skb->dev == xfrm_params->xs->xso.dev &&
			    net_eq(xs_net(xfrm_params->xs), dev_net(skb->dev)) &&
			    refcount_inc_not_zero(&xfrm_params->xs->refcnt)) {
				x = xfrm_params->xs;
			}
			break;
		}
	}
	spin_unlock_bh(&xfrm_params_list.lock);

	if (!x)
		return false;

	/* only attach secpath for resolved states, an empty secpath
	 * (len == 0) makes xfrm_input_state() read xvec[-1]
	 */
	sp = secpath_set(skb);
	if (!sp || sp->len >= XFRM_MAX_OFFLOAD_DEPTH) {
		xfrm_state_put(x);
		return false;
	}

	sp->xvec[sp->len++] = x;
	sp->olen++;
	xo = xfrm_offload(skb);
	if (!xo) {
		secpath_reset(skb);
		return false;
	}
	xo->flags = CRYPTO_DONE;
	xo->status = CRYPTO_SUCCESS;
	return true;
}

struct xfrm_params_list *mtk_xfrm_params_list_get(void)
{
	return &xfrm_params_list;
}

static bool mtk_xfrm_offload_cdrt_tear_down(struct mtk_xfrm_params *xfrm_params)
{
	if (!xfrm_params->cdrt)
		return true;

	memset(&xfrm_params->cdrt->desc, 0, sizeof(struct cdrt_desc));

	return !mtk_pce_cdrt_entry_write(xfrm_params->cdrt);
}

static int mtk_xfrm_offload_cdrt_setup(struct mtk_xfrm_params *xfrm_params)
{
	struct cdrt_desc *cdesc = &xfrm_params->cdrt->desc;

	cdesc->desc1.common.type = 3;
	cdesc->desc1.token_len = 48;
	/* All use small transform record now */
	cdesc->desc1.p_tr[0] = lower_32_bits(xfrm_params->tr_dma) | xfrm_params->tr_type;
	cdesc->desc1.p_tr[1] = upper_32_bits(xfrm_params->tr_dma);

	cdesc->desc2.hw_srv = 3;
	cdesc->desc2.allow_pad = 1;
	cdesc->desc2.strip_pad = 1;

	return mtk_pce_cdrt_entry_write(xfrm_params->cdrt);
}

static bool mtk_xfrm_offload_cls_entry_tear_down(struct mtk_xfrm_params *xfrm_params)
{
	if (!xfrm_params->cdrt || !xfrm_params->cdrt->cls)
		return true;

	memset(&xfrm_params->cdrt->cls->cdesc, 0, sizeof(struct cls_desc));

	if (mtk_pce_cls_entry_write(xfrm_params->cdrt->cls))
		return false;

	mtk_pce_cls_entry_free(xfrm_params->cdrt->cls);
	xfrm_params->cdrt->cls = NULL;
	return true;
}

static int mtk_xfrm_offload_cls_entry_setup(struct mtk_xfrm_params *xfrm_params)
{
	struct cls_desc *cdesc;
	struct xfrm_state *xs = xfrm_params->xs;
	u32 ppe_port;

	xfrm_params->cdrt->cls = mtk_pce_cls_entry_alloc();
	if (IS_ERR(xfrm_params->cdrt->cls)) {
		int ret = PTR_ERR(xfrm_params->cdrt->cls);
		xfrm_params->cdrt->cls = NULL;
		return ret;
	}

	cdesc = &xfrm_params->cdrt->cls->cdesc;

	if (mac_filter_enable) {
		CLS_DESC_DATA(cdesc, fport, PSE_PORT_TDMA);
		CLS_DESC_DATA(cdesc, tport_idx, 0x3);
	} else {
		ppe_port = mtk_crypto_ppe_get_num(xs->xso.dev);
		CLS_DESC_DATA(cdesc, fport, ppe_port);
		CLS_DESC_DATA(cdesc, tport_idx, 0x2);
	}
	CLS_DESC_DATA(cdesc, cdrt_idx, xfrm_params->cdrt->idx);

	if (xs->encap) {
		CLS_DESC_MASK_DATA(cdesc, tag,
			   CLS_DESC_TAG_MASK, CLS_DESC_TAG_MATCH_L4_USR);
		CLS_DESC_MASK_DATA(cdesc, l4_type,
				CLS_DESC_L4_TYPE_MASK, IPPROTO_UDP);
		CLS_DESC_MASK_DATA(cdesc, l4_valid,
			   CLS_DESC_L4_VALID_MASK,
			   CLS_DESC_VALID_UPPER_HALF_WORD_BIT |
			   CLS_DESC_VALID_LOWER_HALF_WORD_BIT |
			   CLS_DESC_VALID_DPORT_BIT);
		CLS_DESC_MASK_DATA(cdesc, l4_dport, CLS_DESC_L4_DPORT_MASK,
							be16_to_cpu(xs->encap->encap_dport));
	} else {
		CLS_DESC_MASK_DATA(cdesc, tag,
			   CLS_DESC_TAG_MASK, CLS_DESC_TAG_MATCH_L4_HDR);
		CLS_DESC_MASK_DATA(cdesc, l4_udp_hdr_nez,
			   CLS_DESC_UDPLITE_L4_HDR_NEZ_MASK,
			   CLS_DESC_UDPLITE_L4_HDR_NEZ_MASK);
		CLS_DESC_MASK_DATA(cdesc, l4_type,
			   CLS_DESC_L4_TYPE_MASK, IPPROTO_ESP);
		CLS_DESC_MASK_DATA(cdesc, l4_valid,
			   0x3,
			   CLS_DESC_VALID_UPPER_HALF_WORD_BIT |
			   CLS_DESC_VALID_LOWER_HALF_WORD_BIT);
	}

	CLS_DESC_MASK_DATA(cdesc, l4_hdr_usr_data,
					0xFFFFFFFF, be32_to_cpu(xfrm_params->xs->id.spi));

	{
		int ret = mtk_pce_cls_entry_write(xfrm_params->cdrt->cls);
		return ret;
	}
}

static int mtk_xfrm_offload_context_setup(struct mtk_xfrm_params *xfrm_params)
{
	void *tr;
	int ret;

	switch (xfrm_params->xs->outer_mode.encap) {
	case XFRM_MODE_TUNNEL:
		tr = mtk_ddk_tr_ipsec_build(xfrm_params, SAB_IPSEC_TUNNEL);
		break;
	case XFRM_MODE_TRANSPORT:
		tr = mtk_ddk_tr_ipsec_build(xfrm_params, SAB_IPSEC_TRANSPORT);
		break;
	default:
		ret = -EINVAL;
		goto err_out;
	}

	if (!tr) {
		ret = -EINVAL;
		goto err_out;
	}

	xfrm_params->p_tr = tr;

	return mtk_xfrm_offload_cdrt_setup(xfrm_params);

err_out:
	return ret;
}

int mtk_xfrm_offload_state_add(struct xfrm_state *xs, struct netlink_ext_ack *extack)
{
        struct mtk_xfrm_params *params, *other;
        bool inbound = xs->xso.dir == XFRM_DEV_OFFLOAD_IN;
        int ret;

        if (xs->xso.type != XFRM_DEV_OFFLOAD_PACKET)
                return -EOPNOTSUPP;
        if ((xs->props.family != AF_INET && xs->props.family != AF_INET6) ||
            xs->id.proto != IPPROTO_ESP ||
            (xs->props.mode != XFRM_MODE_TUNNEL && xs->props.mode != XFRM_MODE_TRANSPORT) ||
            (xs->xso.dir != XFRM_DEV_OFFLOAD_IN && xs->xso.dir != XFRM_DEV_OFFLOAD_OUT) ||
            !net_eq(xs_net(xs), &init_net) || !mtk_crypto_supported_dev(xs->xso.dev)) {
                NL_SET_ERR_MSG_MOD(extack, "EIP packet offload requires IPv4/IPv6 ESP on a physical MT7988 GMAC");
                return -EINVAL;
        }
        if (xs->props.flags & XFRM_STATE_ESN || xs->if_id || xs->tfcpad ||
            xs->props.replay_window > 32 || xs->replay.seq || xs->replay.oseq ||
            xs->props.flags & XFRM_STATE_AF_UNSPEC) {
                NL_SET_ERR_MSG_MOD(extack, "EIP does not support ESN, XFRM interfaces, replay windows above 32 or resumed sequences");
                return -EINVAL;
        }
        if (xs->encap &&
            (xs->encap->encap_type != UDP_ENCAP_ESPINUDP ||
             !xs->encap->encap_sport || !xs->encap->encap_dport)) {
                NL_SET_ERR_MSG_MOD(extack, "EIP supports UDP ESP-in-UDP encapsulation only");
                return -EINVAL;
        }
        params = kzalloc(sizeof(*params), GFP_KERNEL);
        if (!params) {
                return -ENOMEM;
        }
        params->xs = xs;
        params->spi = xs->id.spi;
        params->inbound = inbound;
        params->dir = inbound ? SAB_DIRECTION_INBOUND : SAB_DIRECTION_OUTBOUND;
        INIT_LIST_HEAD(&params->node);
        mutex_lock(&sa_setup_lock);
        /* The official PCE SPI classifier does not match an outer address.
         * Reject ambiguous inbound SPIs rather than select another peer's SA.
         */
        spin_lock_bh(&xfrm_params_list.lock);
        ret = 0;
        if (inbound) {
                list_for_each_entry(other, &xfrm_params_list.list, node)
                        if (other->inbound && other->spi == params->spi)
                                ret = -EEXIST;
                list_for_each_entry(other, &quarantined_states, node)
                        if (other->inbound && other->spi == params->spi)
                                ret = -EEXIST;
        }
        spin_unlock_bh(&xfrm_params_list.lock);
        if (ret)
                goto err;
        params->cdrt = mtk_pce_cdrt_entry_alloc(inbound ? CDRT_DECRYPT : CDRT_ENCRYPT);
        if (IS_ERR(params->cdrt)) {
                ret = PTR_ERR(params->cdrt);
                params->cdrt = NULL;
                goto err;
        }
        ret = mtk_xfrm_offload_context_setup(params);
        if (ret)
                goto err;
        if (inbound) {
                ret = mtk_xfrm_offload_cls_entry_setup(params);
                if (ret)
                        goto err;
        }
        spin_lock_bh(&xfrm_params_list.lock);
        xs->xso.offload_handle = (unsigned long)params;
        list_add_tail(&params->node, &xfrm_params_list.list);
        spin_unlock_bh(&xfrm_params_list.lock);
        if (extack) {
                BUILD_BUG_ON(sizeof(params->cdrt->idx) > sizeof(extack->cookie));
                memcpy(extack->cookie, &params->cdrt->idx, sizeof(params->cdrt->idx));
                extack->cookie_len = sizeof(params->cdrt->idx);
        }
        mutex_unlock(&sa_setup_lock);
        return 0;
err:
        mtk_xfrm_params_free(params);
        mutex_unlock(&sa_setup_lock);
        return ret;
}

void mtk_xfrm_offload_state_delete(struct xfrm_state *xs)
{
        struct mtk_xfrm_params *params;

        spin_lock_bh(&xfrm_params_list.lock);
        params = (void *)xs->xso.offload_handle;
        if (params) {
                params->dead = true;
                foe_clear_crypto_entry(params->cdrt->idx);
        }
        spin_unlock_bh(&xfrm_params_list.lock);
}

static void mtk_xfrm_params_free(struct mtk_xfrm_params *params)
{
        if (!mtk_xfrm_offload_cls_entry_tear_down(params) ||
            !mtk_xfrm_offload_cdrt_tear_down(params))
                goto quarantine;
        if (params->p_handle) {
                if (!mtk_ddk_invalidate_rec(params->p_handle, true))
                        goto quarantine;
                memzero_explicit(params->p_tr, params->tr_len);
                if (!crypto_free_sa(params->p_handle, 0))
                        goto quarantine;
        }
        if (params->cdrt)
                mtk_pce_cdrt_entry_free(params->cdrt);
        kfree(params);
        return;
quarantine:
        __module_get(THIS_MODULE);
        /* Retain the allocation, classifier/CDRT and module reference when
         * hardware retirement cannot be confirmed. Never recycle a live key.
         */
        params->dead = true;
        params->xs = NULL;
        spin_lock_bh(&xfrm_params_list.lock);
        list_add_tail(&params->node, &quarantined_states);
        spin_unlock_bh(&xfrm_params_list.lock);
        CRYPTO_ERR("SA retirement failed; context quarantined until reboot\n");
}

void mtk_xfrm_offload_state_free(struct xfrm_state *xs)
{
        struct mtk_xfrm_params *params;

        mutex_lock(&sa_setup_lock);
        spin_lock_bh(&xfrm_params_list.lock);
        params = (void *)xs->xso.offload_handle;
        if (params) {
                xs->xso.offload_handle = 0;
                params->dead = true;
                list_del_init(&params->node);
                foe_clear_crypto_entry(params->cdrt->idx);
        }
        spin_unlock_bh(&xfrm_params_list.lock);
        if (params)
                mtk_xfrm_params_free(params);
        mutex_unlock(&sa_setup_lock);
}

void mtk_xfrm_offload_state_tear_down(void)
{
        struct mtk_xfrm_params *params;
        LIST_HEAD(retired);

        mutex_lock(&sa_setup_lock);
        spin_lock_bh(&xfrm_params_list.lock);
        list_splice_init(&xfrm_params_list.list, &retired);
        list_for_each_entry(params, &retired, node) {
                params->xs->xso.offload_handle = 0;
                params->dead = true;
                foe_clear_crypto_entry(params->cdrt->idx);
        }
        spin_unlock_bh(&xfrm_params_list.lock);
        while (!list_empty(&retired)) {
                params = list_first_entry(&retired, struct mtk_xfrm_params, node);
                list_del_init(&params->node);
                mtk_xfrm_params_free(params);
        }
        mutex_unlock(&sa_setup_lock);
}

int mtk_xfrm_offload_policy_add(struct xfrm_policy *xp, struct netlink_ext_ack *extack)
{
        if (!net_eq(xp_net(xp), &init_net) ||
            !mtk_crypto_supported_dev(xp->xdo.dev))
                return -EOPNOTSUPP;
        /* RX secpath restores normal Linux policy checks. Do not retain a
         * pointer to an SA in a policy: rekeying can free the SA first.
         */
        xp->xdo.offload_handle = 1;
        return 0;
}

void mtk_xfrm_offload_policy_delete(struct xfrm_policy *xp)
{
        struct mtk_xfrm_params *params;

        spin_lock_bh(&xfrm_params_list.lock);
        xp->xdo.offload_handle = 0;
        list_for_each_entry(params, &xfrm_params_list.list, node)
                foe_clear_crypto_entry(params->cdrt->idx);
        spin_unlock_bh(&xfrm_params_list.lock);
}

void mtk_xfrm_offload_policy_free(struct xfrm_policy *xp)
{
}

/* This eligibility callback never consumes or modifies the skb. */
bool mtk_xfrm_offload_ok(struct sk_buff *skb, struct xfrm_state *xs)
{
        return xs->xso.type == XFRM_DEV_OFFLOAD_PACKET &&
               xs->xso.dir == XFRM_DEV_OFFLOAD_OUT &&
               READ_ONCE(xs->km.state) == XFRM_STATE_VALID &&
               READ_ONCE(xs->xso.offload_handle) &&
               mtk_crypto_supported_dev(xs->xso.dev);
}

static int mtk_xfrm_packet_xmit_one(struct sk_buff *skb, struct xfrm_state *xs)
{
        struct dst_entry *dst = xfrm_dst_path(skb_dst(skb));
        struct sec_path *sp;
        int ret;

        if (!dst || dst->dev != xs->xso.dev ||
            skb->len > dst_mtu(skb_dst(skb))) {
                ret = -EMSGSIZE;
                goto drop;
        }
        if (skb_cow_head(skb, LL_RESERVED_SPACE(dst->dev) + NET_SKB_PAD)) {
                ret = -ENOMEM;
                goto drop;
        }
        if (skb->ip_summed == CHECKSUM_PARTIAL) {
                ret = skb_checksum_help(skb);
                if (ret)
                        goto drop;
        }
        secpath_reset(skb);
        sp = secpath_set(skb);
        if (!sp) {
                ret = -ENOMEM;
                goto drop;
        }
        xfrm_state_hold(xs);
        sp->xvec[sp->len++] = xs;
        sp->olen = 1;
        xfrm_offload(skb)->flags = XFRM_XMIT;
        dst_hold(dst);
        skb_dst_drop(skb);
        skb_dst_set(skb, dst);
        skb->dev = dst->dev;
        skb->protocol = htons(xs->props.family == AF_INET ? ETH_P_IP : ETH_P_IPV6);
        /* Keep the normal neighbour queue, including ARP/ND resolution. The
         * TX hook resolves CDRT from the referenced SA after any skb copies.
         */
        {
                const void *daddr = xs->props.family == AF_INET ?
                        (const void *)&xs->id.daddr.a4 : (const void *)&xs->id.daddr.a6;
                struct neighbour *neigh = dst_neigh_lookup(dst, daddr);

                if (!neigh) {
                        ret = -EHOSTUNREACH;
                        goto drop;
                }
                ret = neigh_output(neigh, skb, neigh->tbl->family != xs->props.family);
                neigh_release(neigh);
                return ret;
        }
drop:
        kfree_skb(skb);
        return ret;
}

int mtk_xfrm_packet_xmit(struct sk_buff *skb, struct xfrm_state *xs)
{
        struct sk_buff *segs, *next;
        int ret = 0;

        if (!skb_is_gso(skb))
                return mtk_xfrm_packet_xmit_one(skb, xs);
        segs = skb_gso_segment(skb, 0);
        if (!segs)
                return mtk_xfrm_packet_xmit_one(skb, xs);
        consume_skb(skb);
        if (IS_ERR(segs))
                return PTR_ERR(segs);
        skb_list_walk_safe(segs, segs, next) {
                skb_mark_not_on_list(segs);
                ret = mtk_xfrm_packet_xmit_one(segs, xs);
                if (ret) {
                        kfree_skb_list(next);
                        break;
                }
        }
        return ret;
}

bool mtk_crypto_tx_prepare(struct sk_buff *skb, struct net_device *dev, u8 *cdrt)
{
        struct xfrm_state *xs = xfrm_input_state(skb);
        struct mtk_xfrm_params *params;
        bool ok = false;

        spin_lock_bh(&xfrm_params_list.lock);
        params = (void *)xs->xso.offload_handle;
        if (params && !params->dead && xs->xso.dev == dev &&
            xs->km.state == XFRM_STATE_VALID) {
                *cdrt = params->cdrt->idx;
                atomic64_add(skb->len, &params->bytes);
                atomic64_inc(&params->packets);
                ok = true;
        }
        spin_unlock_bh(&xfrm_params_list.lock);
        return ok;
}

void mtk_xfrm_update_stats(struct xfrm_state *xs)
{
        struct mtk_xfrm_params *params;

        spin_lock_bh(&xfrm_params_list.lock);
        params = (void *)xs->xso.offload_handle;
        if (params) {
                xs->curlft.bytes = atomic64_read(&params->bytes);
                xs->curlft.packets = atomic64_read(&params->packets);
        }
        spin_unlock_bh(&xfrm_params_list.lock);
}

int mtk_xfrm_cls_notify(struct notifier_block *nb, unsigned long event, void *data)
{
        struct mtk_xfrm_params *params;
        struct cls_entry *cls = data;

        if (event != CLS_NOTIFY_DELETE_ENTRY || !cls)
                return NOTIFY_DONE;
        spin_lock_bh(&xfrm_params_list.lock);
        list_for_each_entry(params, &xfrm_params_list.list, node) {
                if (params->cdrt->cls == cls) {
                        params->dead = true;
                        params->cdrt->cls = NULL;
                }
        }
        spin_unlock_bh(&xfrm_params_list.lock);
        return NOTIFY_DONE;
}
