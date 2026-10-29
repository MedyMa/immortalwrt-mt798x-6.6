/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 MediaTek Inc.
 */

#ifndef MTK_TNL_H
#define MTK_TNL_H

#include <linux/skbuff.h>
#include <linux/mtk_npu_rx.h>

struct mtk_tnl_desc {
	u32 entry : 15;
	u32 filled : 3;
	u32 crsn : 5;
	u32 resv1 : 3;
	u32 sport : 4;
	u32 resv2 : 1;
	u32 alg : 1;
	u32 iface : 8;
	u32 wdmaid : 2;
	u32 rxid : 2;
	u32 wcid : 16;
	u32 bssid : 8;
	u32 usr_info : 16;
	u32 tid : 4;
	u32 is_fixedrate : 1;
	u32 is_prior : 1;
	u32 is_sp : 1;
	u32 hf : 1;
	u32 amsdu : 1;
	u32 tops : 6;
	u32 is_decap : 1;
	u32 cdrt : 8;
	u32 is_decrypt : 1;
	u32 is_pppoe : 1;
	u32 resv3 : 2;
	u32 magic_tag_protect : 16;
} __packed;

#define TNL_MAGIC_TAG 0x6789
#define skb_tnl_cdrt(skb) (((struct mtk_tnl_desc *)((skb)->head))->cdrt)
#define skb_tnl_tops(skb) (((struct mtk_tnl_desc *)((skb)->head))->tops)
#define skb_tnl_set_cdrt(skb, cdrt) ((skb_tnl_cdrt(skb)) = (cdrt))
#define skb_tnl_magic_tag(skb) (((struct mtk_tnl_desc *)((skb)->head))->magic_tag_protect)
#define is_tnl_tag_valid(skb) (skb_tnl_magic_tag(skb) == TNL_MAGIC_TAG)
#define skb_tnl_is_pppoe(skb) (((struct mtk_tnl_desc *)((skb)->head))->is_pppoe)

/* Firmware decapsulation reasons carry a 32-entry tunnel index.  get_dev
 * returns a held device reference, or NULL. Unregister drains RX callbacks.
 */
#define MTK_TOPS_RX_TUNNEL_FIRST 0x10
#define MTK_TOPS_RX_TUNNEL_LAST  0x2f
struct net_device;
struct mtk_tnl_rx_ops {
	struct net_device *(*get_dev)(u8 reason, struct mtk_npu_rx_meta *meta);
};
bool mtk_tnl_rx_meta_current(const struct mtk_npu_rx_meta *meta);
int mtk_tnl_rx_register(const struct mtk_tnl_rx_ops *ops);
void mtk_tnl_rx_unregister(const struct mtk_tnl_rx_ops *ops);

#endif /* MTK_TNL_H */
