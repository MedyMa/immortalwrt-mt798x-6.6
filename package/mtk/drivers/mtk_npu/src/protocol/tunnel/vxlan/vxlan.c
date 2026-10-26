// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2025 MediaTek Inc. All Rights Reserved.
 *
 * Author: Frank-zj Lin <rank-zj.lin@mediatek.com>
 */

#include <linux/netdevice.h>
#include <net/vxlan.h>

#include <pce/cls.h>
#include <pce/netsys.h>
#include <pce/pce.h>

#include "npu/internal.h"
#include "npu/netsys.h"
#include "npu/protocol/transport/udp.h"
#include "npu/protocol/tunnel/vxlan/vxlan.h"
#include "npu/tunnel.h"

struct vxlan_encap_statistic {
	u64 offload_invalid;
	u64 null_hdr_ptr;
	u64 success;
};

struct vxlan_decap_statistic {
	u64 offload_invalid;
	u64 null_hdr_ptr;
	u64 success;
};

struct vxlan_statistic {
	struct npu_tnl_statistic ts;
	struct vxlan_encap_statistic encap;
	struct vxlan_decap_statistic decap;
};

static struct vxlan_statistic vxlan_ts;

static inline void inc_vxlan_statistic_encap_offload_invalid(void)
{
	if (!mtk_npu_tnl_statistic_is_enabled())
		return;

	vxlan_ts.encap.offload_invalid++;
}

static inline void inc_vxlan_statistic_encap_null_hdr_ptr(void)
{
	if (!mtk_npu_tnl_statistic_is_enabled())
		return;

	vxlan_ts.encap.null_hdr_ptr++;
}

static inline void inc_vxlan_statistic_encap_success(void)
{
	if (!mtk_npu_tnl_statistic_is_enabled())
		return;

	vxlan_ts.encap.success++;
}

static inline void inc_vxlan_statistic_decap_offload_invalid(void)
{
	if (!mtk_npu_tnl_statistic_is_enabled())
		return;

	vxlan_ts.decap.offload_invalid++;
}

static inline void inc_vxlan_statistic_decap_null_hdr_ptr(void)
{
	if (!mtk_npu_tnl_statistic_is_enabled())
		return;

	vxlan_ts.decap.null_hdr_ptr++;
}

static inline void inc_vxlan_statistic_decap_success(void)
{
	if (!mtk_npu_tnl_statistic_is_enabled())
		return;

	vxlan_ts.decap.success++;
}

static int vxlan_cls_entry_setup(struct npu_tnl_info *tnl_info,
				 struct cls_desc *cdesc)
{
	CLS_DESC_DATA(cdesc, fport, PSE_PORT_TDMA);
	CLS_DESC_MASK_DATA(cdesc, tag, CLS_DESC_TAG_MASK, CLS_DESC_TAG_MATCH_L4_USR);
	CLS_DESC_MASK_DATA(cdesc, dip_match, CLS_DESC_DIP_MATCH, CLS_DESC_DIP_MATCH);
	CLS_DESC_MASK_DATA(cdesc, l4_type, CLS_DESC_L4_TYPE_MASK, IPPROTO_UDP);
	CLS_DESC_MASK_DATA(cdesc, l4_valid,
			   CLS_DESC_L4_VALID_MASK,
			   CLS_DESC_VALID_UPPER_HALF_WORD_BIT |
			   CLS_DESC_VALID_LOWER_HALF_WORD_BIT |
			   CLS_DESC_VALID_DPORT_BIT);
	CLS_DESC_MASK_DATA(cdesc, l4_dport, CLS_DESC_L4_DPORT_MASK, 4789);
	CLS_DESC_MASK_DATA(cdesc, l4_hdr_usr_data, 0xFFFFFFFF, 0x08000000);

	return 0;
}

static inline bool vxlan_offload_valid(struct sk_buff *skb)
{
	struct vxlanhdr *vxlan;
	struct vxlanhdr vxlanh;

	vxlan = skb_header_pointer(skb, 0, sizeof(struct vxlanhdr), &vxlanh);
	if (!vxlan)
		return false;

	return vxlan->vx_flags == VXLAN_HF_VNI &&
	       !(vxlan->vx_vni & htonl(0xff));
}

static int vxlan_udp_decap_param_setup(struct sk_buff *skb, struct npu_params *params)
{
	struct npu_udp_params *udpp = &params->transport.udp;
	struct udphdr *udp;
	struct udphdr udph;
	int ret;

	skb_push(skb, sizeof(struct udphdr));
	udp = skb_header_pointer(skb, 0, sizeof(struct udphdr), &udph);
	if (unlikely(!udp)) {
		ret = -EINVAL;
		goto out;
	}

	params->transport.type = NPU_TRANSPORT_UDP;

	udpp->sport = udp->dest;
	udpp->dport = udp->dest;

	ret = mtk_npu_network_decap_param_setup(skb, params);
out:
	skb_pull(skb, sizeof(struct udphdr));

	return ret;
}

static void vxlan_udp_encap_param_post_setup(struct npu_params *params)
{
	params->transport.udp.sport = params->transport.udp.dport;
}

static int vxlan_tnl_decap_param_setup(struct sk_buff *skb, struct npu_params *params)
{
	struct npu_vxlan_params *vxlanp;
	struct vxlanhdr *vxlan;
	struct vxlanhdr vxlanh;
	int ret;

	/* vxlan */
	skb_push(skb, sizeof(struct vxlanhdr));
	if (unlikely(!vxlan_offload_valid(skb))) {
		ret = -EINVAL;
		inc_vxlan_statistic_decap_offload_invalid();
		goto restore_vxlan;
	}

	vxlan = skb_header_pointer(skb, 0, sizeof(struct vxlanhdr), &vxlanh);
	if (unlikely(!vxlan)) {
		ret = -EINVAL;
		inc_vxlan_statistic_decap_null_hdr_ptr();
		goto restore_vxlan;
	}

	vxlanp = &params->tunnel.vxlan;
	vxlanp->vni = vxlan_vni(vxlan->vx_vni);

	params->tunnel.type = NPU_TUNNEL_VXLAN;

	ret = vxlan_udp_decap_param_setup(skb, params);
restore_vxlan:
	skb_pull(skb, sizeof(struct vxlanhdr));

	if (!ret)
		inc_vxlan_statistic_decap_success();

	return ret;
}

static int vxlan_tnl_encap_param_setup(struct sk_buff *skb, struct npu_params *params)
{
	struct npu_vxlan_params *vxlanp;
	struct vxlanhdr *vxlan;
	struct vxlanhdr vxlanh;

	vxlan_udp_encap_param_post_setup(params);

	if (unlikely(!vxlan_offload_valid(skb))) {
		inc_vxlan_statistic_encap_offload_invalid();
		return -EINVAL;
	}

	vxlan = skb_header_pointer(skb, 0, sizeof(struct vxlanhdr), &vxlanh);
	if (unlikely(!vxlan)) {
		inc_vxlan_statistic_encap_null_hdr_ptr();
		return -EINVAL;
	}

	vxlanp = &params->tunnel.vxlan;
	vxlanp->vni = vxlan_vni(vxlan->vx_vni);
	params->tunnel.type = NPU_TUNNEL_VXLAN;

	inc_vxlan_statistic_encap_success();

	return 0;
}

static int vxlan_tnl_l2_param_update(struct npu_params *params,
				      struct ethhdr *eth)
{
	memcpy(params->mac.eth.h_source, eth->h_source, ETH_ALEN);
	memcpy(params->mac.eth.h_dest, eth->h_dest, ETH_ALEN);

	return 1;
}

static bool vxlan_tnl_decap_offloadable(struct sk_buff *skb)
{
	struct udphdr udp_copy, *udp;
	struct vxlanhdr vx_copy, *vx;
	const struct iphdr *ip = ip_hdr(skb);
	unsigned int offset = ip->ihl * 4;

	if (ip->protocol != IPPROTO_UDP)
		return false;
	udp = skb_header_pointer(skb, offset, sizeof(udp_copy), &udp_copy);
	vx = skb_header_pointer(skb, offset + sizeof(udp_copy), sizeof(vx_copy), &vx_copy);
	return udp && vx && udp->dest == htons(UDP_VXLAN_PORT) &&
	       ntohs(udp->len) >= sizeof(udp_copy) + sizeof(vx_copy) + ETH_HLEN &&
	       vx->vx_flags == VXLAN_HF_VNI && !(vx->vx_vni & htonl(0xff));
}

static void vxlan_tnl_param_restore(struct npu_params *old, struct npu_params *new)
{
	if (old->tunnel.vxlan.vni)
		new->tunnel.vxlan.vni = old->tunnel.vxlan.vni;
}

static bool vxlan_tnl_param_match(struct npu_params *p, struct npu_params *target)
{
	return (p->tunnel.vxlan.vni == target->tunnel.vxlan.vni);
}

static void vxlan_tnl_param_dump(struct seq_file *s, struct npu_params *params)
{
	struct npu_vxlan_params *vxlanp = &params->tunnel.vxlan;

	seq_puts(s, "\tTunnel Type: VXLAN ");
	seq_printf(s, "VNI: %06u ", ntohl(vxlanp->vni));
}

static void vxlan_tnl_statistic_encap_dump(struct seq_file *s, struct npu_tnl_type *tnl_type)
{
	seq_printf(s, "success: %llu|null header pointer: %llu|offload invalid: %llu|\n",
		   vxlan_ts.encap.success,
		   vxlan_ts.encap.null_hdr_ptr,
		   vxlan_ts.encap.offload_invalid);
}

static void vxlan_tnl_statistic_decap_dump(struct seq_file *s, struct npu_tnl_type *tnl_type)
{
	seq_printf(s, "success: %llu|null header pointer: %llu|offload invalid: %llu|\n",
		   vxlan_ts.decap.success,
		   vxlan_ts.decap.null_hdr_ptr,
		   vxlan_ts.decap.offload_invalid);
}

static void vxlan_tnl_statistic_clear(struct npu_tnl_type *tnl_type)
{
	memset(&vxlan_ts, 0, sizeof(struct vxlan_statistic));
}

static struct npu_tnl_type vxlan_type = {
	.type_name = TYPE_NAME_VXLAN,
	.cls_entry_setup = vxlan_cls_entry_setup,
	.tnl_decap_param_setup = vxlan_tnl_decap_param_setup,
	.tnl_encap_param_setup = vxlan_tnl_encap_param_setup,
	.tnl_decap_offloadable = vxlan_tnl_decap_offloadable,
	.tnl_l2_param_update = vxlan_tnl_l2_param_update,
	.tnl_param_restore = vxlan_tnl_param_restore,
	.tnl_param_match = vxlan_tnl_param_match,
	.tnl_param_dump = vxlan_tnl_param_dump,
	.tnl_proto_type = NPU_TUNNEL_VXLAN,
	.has_inner_eth = true,
	.max_mtu = 1450,
	.ts = &vxlan_ts.ts,
	.tnl_statistic_encap_dump = vxlan_tnl_statistic_encap_dump,
	.tnl_statistic_decap_dump = vxlan_tnl_statistic_decap_dump,
	.tnl_statistic_clear = vxlan_tnl_statistic_clear,
};

int mtk_npu_vxlan_init(void)
{
	return mtk_npu_tnl_type_register(&vxlan_type);
}
EXPORT_SYMBOL(mtk_npu_vxlan_init);

void mtk_npu_vxlan_deinit(void)
{
	mtk_npu_tnl_type_unregister(&vxlan_type);
}
EXPORT_SYMBOL(mtk_npu_vxlan_deinit);
