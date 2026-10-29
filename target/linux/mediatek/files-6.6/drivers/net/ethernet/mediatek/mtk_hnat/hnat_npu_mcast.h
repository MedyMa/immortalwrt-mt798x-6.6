/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _HNAT_NPU_MCAST_H
#define _HNAT_NPU_MCAST_H

#include <linux/if_ether.h>
#include <linux/in6.h>
#include <linux/errno.h>

struct mtk_hnat;
struct net_device;
struct sk_buff;
struct foe_entry;

/* Shared with the public MTK NPU multicast adapter. No device pointers are
 * retained in firmware parameters. Destination IDs are firmware ABI values.
 */
struct mcast_offload_info {
	union { __be32 ip; struct in6_addr ipv6; } dst;
	union { __be32 ip; struct in6_addr ipv6; } src;
	u32 dest;
	u8 daddr[ETH_ALEN];
	u8 pqid;
	u8 dsa_port;
	bool is_ipv6;
	bool m2u_en;
	u32 npu_grp_idx;
};

struct npu_hnat_mcast_ops {
	int (*npu_mcast_client_insert)(struct mcast_offload_info *info);
	int (*npu_mcast_client_delete)(struct mcast_offload_info *info);
	int (*npu_mcast_client_update)(struct mcast_offload_info *info);
	int (*group_delete)(u8 index);
	int (*resolve_client)(struct net_device *dev, struct mcast_offload_info *info);
	bool (*ready)(void);
};

#if IS_ENABLED(CONFIG_MEDIATEK_NETSYS_V3)
int mtk_npu_hnat_mcast_ops_register(const struct npu_hnat_mcast_ops *ops);
int mtk_npu_hnat_mcast_ops_unregister(const struct npu_hnat_mcast_ops *ops);
void hnat_npu_mcast_pause(const struct npu_hnat_mcast_ops *ops);
void hnat_npu_mcast_resume(const struct npu_hnat_mcast_ops *ops);
void hnat_npu_mcast_remove(struct mtk_hnat *h);
bool hnat_npu_mcast_active(void);
int hnat_npu_mcast_prepare(struct sk_buff *skb, const struct net_device *out,
			 struct foe_entry *entry, u32 ppe, u32 slot);
void hnat_notify_mcast_sw_path(int index);
void hnat_notify_mcast_hw_path(int index);
#else
static inline void hnat_npu_mcast_remove(struct mtk_hnat *h) { }
static inline bool hnat_npu_mcast_active(void) { return false; }
static inline int hnat_npu_mcast_prepare(struct sk_buff *skb,
	const struct net_device *out, struct foe_entry *entry, u32 ppe, u32 slot)
{
	return -EOPNOTSUPP;
}
#endif

#endif
