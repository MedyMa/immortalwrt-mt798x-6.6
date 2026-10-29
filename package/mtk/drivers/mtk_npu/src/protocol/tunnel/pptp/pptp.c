// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2025 MediaTek Inc. All Rights Reserved.
 *
 * Author: Frank-zj Lin <frank-zj.lin@mediatek.com>
 */

#include <linux/if_pppox.h>
#include <linux/ppp_channel.h>

#include <net/gre.h>
#include <net/ip.h>
#include <net/pptp.h>
#include <net/sock.h>

#include <pce/cls.h>
#include <pce/netsys.h>
#include <pce/pce.h>

#include "npu/netsys.h"
#include "npu/protocol/mac/ppp.h"
#include "npu/protocol/tunnel/pptp/pptp.h"
#include "npu/seq_gen.h"
#include "npu/tunnel.h"

struct pptp_encap_statistic {
	u64 offload_invalid;
	u64 ppp_invalid;
	u64 success;
};

struct pptp_decap_statistic {
	u64 ppp_invalid;
	u64 pptp_gre_len_invalid;
	u64 offload_invalid;
	u64 success;
};

struct pptp_statistic {
	struct npu_tnl_statistic ts;
	struct pptp_encap_statistic encap;
	struct pptp_decap_statistic decap;
};

static struct pptp_statistic pptp_ts;

static inline void inc_pptp_statistic_encap_offload_invalid(void)
{
	if (!mtk_npu_tnl_statistic_is_enabled())
		return;

	pptp_ts.encap.offload_invalid++;
}


static inline void inc_pptp_statistic_encap_ppp_invalid(void)
{
	if (!mtk_npu_tnl_statistic_is_enabled())
		return;

	pptp_ts.encap.ppp_invalid++;
}


static inline void inc_pptp_statistic_encap_success(void)
{
	if (!mtk_npu_tnl_statistic_is_enabled())
		return;

	pptp_ts.encap.success++;
}

static inline void inc_pptp_statistic_decap_ppp_invalid(void)
{
	if (!mtk_npu_tnl_statistic_is_enabled())
		return;

	pptp_ts.decap.ppp_invalid++;
}

static inline void inc_pptp_statistic_decap_pptp_gre_len_invalid(void)
{
	if (!mtk_npu_tnl_statistic_is_enabled())
		return;

	pptp_ts.decap.pptp_gre_len_invalid++;
}


static inline void inc_pptp_statistic_decap_offload_invalid(void)
{
	if (!mtk_npu_tnl_statistic_is_enabled())
		return;

	pptp_ts.decap.offload_invalid++;
}


static inline void inc_pptp_statistic_decap_success(void)
{
	if (!mtk_npu_tnl_statistic_is_enabled())
		return;

	pptp_ts.decap.success++;
}

static int pptp_cls_entry_setup(struct npu_tnl_info *tnl_info,
				struct cls_desc *cdesc)
{
	CLS_DESC_DATA(cdesc, fport, PSE_PORT_TDMA);
	CLS_DESC_MASK_DATA(cdesc, tag, CLS_DESC_TAG_MASK, CLS_DESC_TAG_MATCH_L4_HDR);
	CLS_DESC_MASK_DATA(cdesc, dip_match, CLS_DESC_DIP_MATCH, CLS_DESC_DIP_MATCH);
	CLS_DESC_MASK_DATA(cdesc, l4_type, CLS_DESC_L4_TYPE_MASK, IPPROTO_GRE);
	CLS_DESC_MASK_DATA(cdesc, l4_udp_hdr_nez,
			   CLS_DESC_UDPLITE_L4_HDR_NEZ_MASK,
			   CLS_DESC_UDPLITE_L4_HDR_NEZ_MASK);
	CLS_DESC_MASK_DATA(cdesc, l4_valid,
			   CLS_DESC_L4_VALID_MASK,
			   CLS_DESC_VALID_UPPER_HALF_WORD_BIT |
			   CLS_DESC_VALID_LOWER_HALF_WORD_BIT);
	CLS_DESC_MASK_DATA(cdesc, l4_hdr_usr_data, 0x0000FFFF, 0x0000880B);

	return 0;
}

/* Validate the complete RFC 2637 GRE/PPP header before moving skb->data. */
static int pptp_gre_header(struct sk_buff *skb, int offset,
                           struct pptp_gre_header *header)
{
	const struct pptp_gre_header *p;
	unsigned int len = sizeof(*p) - sizeof(p->ack);

	if (offset < 0 && skb_headroom(skb) < -offset + 16)
		return -EINVAL;
	p = skb_header_pointer(skb, offset, len, header);
	if (!p || p->gre_hd.protocol != GRE_PROTO_PPP ||
	    (p->gre_hd.flags & ~GRE_ACK) != (GRE_KEY | GRE_SEQ | GRE_VERSION_1))
		return -EINVAL;
	if (GRE_IS_ACK(p->gre_hd.flags))
		len += sizeof(p->ack);
	p = skb_header_pointer(skb, offset, len, header);
	if (!p || ntohs(p->payload_len) < sizeof(struct ppp_hdr) ||
	    offset + len + ntohs(p->payload_len) > skb->len)
		return -EINVAL;
	if (p != header)
		memcpy(header, p, len);
	return len;
}

static int pptp_gre_len_evaluate(struct sk_buff *skb)
{
	struct pptp_gre_header gre;
	struct iphdr ip, *p;
	int len;

	for (len = sizeof(gre) - sizeof(gre.ack); len <= sizeof(gre); len += 4) {
		if (skb_headroom(skb) < len + sizeof(ip) + 16 ||
		    pptp_gre_header(skb, -len, &gre) != len)
			continue;
		p = skb_header_pointer(skb, -len - sizeof(ip), sizeof(ip), &ip);
		if (p && p->version == 4 && p->ihl == 5 && p->protocol == IPPROTO_GRE &&
		    !ip_is_fragment(p) && ntohs(p->tot_len) ==
		    sizeof(ip) + len + ntohs(gre.payload_len))
			return len;
	}
	return -EINVAL;
}

static int pptp_tnl_decap_param_setup(struct sk_buff *skb, struct npu_params *params)
{
	struct pptp_gre_header gre;
	int len, ret;

	if (skb_headroom(skb) < 16 + sizeof(struct ppp_hdr) + sizeof(gre) +
				 sizeof(struct iphdr))
		return -EINVAL;
	skb_push(skb, sizeof(struct ppp_hdr));
	if (!mtk_npu_ppp_valid(skb)) {
		inc_pptp_statistic_decap_ppp_invalid();
		ret = -EINVAL;
		goto out_ppp;
	}
	len = pptp_gre_len_evaluate(skb);
	if (len < 0) {
		inc_pptp_statistic_decap_pptp_gre_len_invalid();
		ret = len;
		goto out_ppp;
	}
	skb_push(skb, len);
	ret = pptp_gre_header(skb, 0, &gre);
	if (ret < 0) {
		inc_pptp_statistic_decap_offload_invalid();
		goto out_gre;
	}
	params->tunnel.type = NPU_TUNNEL_PPTP;
	params->tunnel.pptp.dl_call_id = gre.call_id;
	/* Native scalar IDs and cookie are obtained after restoring the skb. */
	ret = mtk_npu_network_decap_param_setup(skb, params);
	if (!ret)
		inc_pptp_statistic_decap_success();
out_gre:
	skb_pull(skb, len);
out_ppp:
	skb_pull(skb, sizeof(struct ppp_hdr));
	return ret;
}

static int pptp_tnl_encap_param_setup(struct sk_buff *skb, struct npu_params *params)
{
	struct pptp_gre_header gre;
	int len, ret = 0;

	len = pptp_gre_header(skb, 0, &gre);
	if (len < 0) {
		inc_pptp_statistic_encap_offload_invalid();
		return len;
	}
	if (!pskb_may_pull(skb, len + sizeof(struct ppp_hdr)))
		return -EINVAL;
	skb_pull(skb, len);
	if (!mtk_npu_ppp_valid(skb)) {
		inc_pptp_statistic_encap_ppp_invalid();
		ret = -EINVAL;
	} else {
		params->tunnel.type = NPU_TUNNEL_PPTP;
		params->tunnel.pptp.ul_call_id = gre.call_id;
		inc_pptp_statistic_encap_success();
	}
	skb_push(skb, len);
	return ret;
}

static bool pptp_tnl_decap_offloadable(struct sk_buff *skb)
{
	struct pptp_gre_header gre;
	struct iphdr ip, *p;
	int len;
	bool valid;

	p = skb_header_pointer(skb, 0, sizeof(ip), &ip);
	if (!p || p->version != 4 || p->ihl != 5 ||
	    p->protocol != IPPROTO_GRE || ip_is_fragment(p))
		return false;
	len = pptp_gre_header(skb, sizeof(ip), &gre);
	if (len < 0 || !pskb_may_pull(skb, sizeof(ip) + len + sizeof(struct ppp_hdr)))
		return false;
	skb_pull(skb, sizeof(ip) + len);
	valid = mtk_npu_ppp_valid(skb);
	skb_push(skb, sizeof(ip) + len);
	return valid;
}

static void pptp_tnl_param_restore(struct npu_params *old, struct npu_params *new)
{
	if (old->tunnel.type != NPU_TUNNEL_PPTP)
		return;
	new->tunnel.pptp = old->tunnel.pptp; /* Counter index zero is valid. */
}

static bool pptp_tnl_param_match(struct npu_params *p, struct npu_params *target)
{
	return p->tunnel.pptp.ul_call_id == target->tunnel.pptp.ul_call_id &&
	       p->tunnel.pptp.dl_call_id == target->tunnel.pptp.dl_call_id;
}

static void pptp_tnl_param_dump(struct seq_file *s, struct npu_params *params)
{
	struct npu_pptp_params *pptpp = &params->tunnel.pptp;

	seq_puts(s, "\tTunnel Type: PPTP ");
	seq_printf(s, "DL Call ID: %05u UL Call ID: %05u SEQ_GEN_IDX: %05u\n",
		   ntohs(pptpp->dl_call_id), ntohs(pptpp->ul_call_id),
		   pptpp->seq_gen_idx);
}

static void pptp_tnl_statistic_encap_dump(struct seq_file *s, struct npu_tnl_type *tnl_type)
{
	seq_printf(s, "success: %llu|offload invalid: %llu|ppp invalid: %llu|\n",
		   pptp_ts.encap.success,
		   pptp_ts.encap.offload_invalid,
		   pptp_ts.encap.ppp_invalid);
}

static void pptp_tnl_statistic_decap_dump(struct seq_file *s, struct npu_tnl_type *tnl_type)
{
	seq_printf(s, "success: %llu|offload invalid: %llu|\n"
		      "      |ppp invalid: %llu|invalid pptp gre header length: %llu|\n",
		   pptp_ts.decap.success,
		   pptp_ts.decap.offload_invalid,
		   pptp_ts.decap.ppp_invalid,
		   pptp_ts.decap.pptp_gre_len_invalid);
}

static void pptp_tnl_statistic_clear(struct npu_tnl_type *tnl_type)
{
	memset(&pptp_ts, 0, sizeof(struct pptp_statistic));
}

#if IS_ENABLED(CONFIG_MTK_NPU_FLOW)
static void pptp_tnl_flow_param_setup(const struct net_device_path *path,
                                     struct npu_params *params)
{
	params->mac.type = NPU_MAC_ETH;
	ether_addr_copy(params->mac.eth.h_source, path->tunnel.h_source);
	ether_addr_copy(params->mac.eth.h_dest, path->tunnel.h_dest);
	params->mac.eth.h_proto = htons(ETH_P_IP);
	params->network.type = NPU_NETWORK_IP;
	params->network.ip.sip = path->tunnel.sip.ip;
	params->network.ip.dip = path->tunnel.dip.ip;
	params->network.ip.proto = IPPROTO_GRE;
	params->network.ip.ttl = path->tunnel.ttl;
	params->network.ip.tos = path->tunnel.tos;
	params->tunnel.type = NPU_TUNNEL_PPTP;
	params->tunnel.pptp.ul_call_id = htons(path->tunnel.pptp.peer_call_id);
	params->tunnel.pptp.dl_call_id = htons(path->tunnel.pptp.call_id);
}
#endif

static struct npu_tnl_type pptp_type = {
	.type_name = TYPE_NAME_PPTP,
#if IS_ENABLED(CONFIG_MTK_NPU_FLOW)
	.ndev_path_type = DEV_PATH_PPTP,
	.tnl_flow_param_setup = pptp_tnl_flow_param_setup,
#endif
	.cls_entry_setup = pptp_cls_entry_setup,
	.tnl_decap_param_setup = pptp_tnl_decap_param_setup,
	.tnl_encap_param_setup = pptp_tnl_encap_param_setup,
	.tnl_decap_offloadable = pptp_tnl_decap_offloadable,
	.tnl_param_restore = pptp_tnl_param_restore,
	.tnl_param_match = pptp_tnl_param_match,
	.tnl_session_prepare = mtk_npu_pptp_session_prepare,
	.tnl_session_get = mtk_npu_pptp_session_get,
	.tnl_session_put = mtk_npu_pptp_session_put,
	.tnl_param_dump = pptp_tnl_param_dump,
	.tnl_proto_type = NPU_TUNNEL_PPTP,
	.has_inner_eth = false,
	/* IPv4 + maximal GRE (including ACK) + full PPP header. */
	.max_mtu = 1460,
	.ts = &pptp_ts.ts,
	.tnl_statistic_encap_dump = pptp_tnl_statistic_encap_dump,
	.tnl_statistic_decap_dump = pptp_tnl_statistic_decap_dump,
	.tnl_statistic_clear = pptp_tnl_statistic_clear,
};

int mtk_npu_pptp_init(void)
{
	int ret = 0;

	ret = mtk_npu_tnl_type_register(&pptp_type);
	if (ret)
		return ret;

	ret = mtk_npu_pptp_seq_init(&pptp_type);
	if (ret)
		mtk_npu_tnl_type_unregister(&pptp_type);
	return ret;
}
EXPORT_SYMBOL(mtk_npu_pptp_init);

void mtk_npu_pptp_deinit(void)
{
	mtk_npu_pptp_seq_deinit();

	mtk_npu_tnl_type_unregister(&pptp_type);
}
EXPORT_SYMBOL(mtk_npu_pptp_deinit);
