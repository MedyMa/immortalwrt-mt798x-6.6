/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MTK_HNAT_DIAG_H
#define _MTK_HNAT_DIAG_H
#include <linux/types.h>
#include <net/netfilter/nf_conntrack_tuple.h>

struct sk_buff;
struct seq_file;
struct nf_conn;
struct mtk_hnat;
struct foe_entry;
struct dentry;

enum hnat_diag_reason {
	HNAT_DIAG_ELIGIBLE, HNAT_DIAG_PARAMETERS, HNAT_DIAG_SYNC_PENDING,
	HNAT_DIAG_NPU_NOT_READY, HNAT_DIAG_SECURITY_POLICY, HNAT_DIAG_INVALID_METADATA,
	HNAT_DIAG_UNSUPPORTED_PROTOCOL, HNAT_DIAG_PARAMETER_ERROR, HNAT_DIAG_NO_TUNNEL,
	HNAT_DIAG_NO_MEMORY, HNAT_DIAG_MARK_EXCLUDED, HNAT_DIAG_ALGORITHM_EXCLUDED,
	HNAT_DIAG_NO_MAC_HEADER, HNAT_DIAG_NO_FLOW_HASH, HNAT_DIAG_NO_ROUTE,
	HNAT_DIAG_UNSUPPORTED_DEVICE, HNAT_DIAG_VLAN_POLICY, HNAT_DIAG_DPI_WAIT,
	HNAT_DIAG_BIND_THRESHOLD, HNAT_DIAG_FRAGMENT, HNAT_DIAG_MTU,
	HNAT_DIAG_ACCOUNT_OWNER, HNAT_DIAG_ACCOUNT_UNAVAILABLE, HNAT_DIAG_PREFILL,
	HNAT_DIAG_BOUND, HNAT_DIAG_DECAP_PENDING, HNAT_DIAG_REJECTED,
	HNAT_DIAG_REASON_MAX,
};

struct hnat_diag_key {
	struct nf_conntrack_tuple tuple;
	u32 ct_id, netns;
	u16 zone;
	u8 direction, zone_dir, zone_flags;
	bool tracked;
};

struct hnat_diag_sample {
	u64 bytes, packets, sampled_ms;
	bool valid, available, npu_path;
};

/* Host events only. path: 0 unspecified, 1 encapsulation, 2 decapsulation. */
void hnat_diag_note_at(struct sk_buff *skb, enum hnat_diag_reason reason, int error,
		       u8 path, int tunnel, u64 tunnel_generation, int classifier,
		       const char *function, unsigned int line);
#define hnat_diag_note(s, r, e, p, t, g, c) \
	hnat_diag_note_at(s, r, e, p, t, g, c, __func__, __LINE__)
#define hnat_diag_fail(s, r) (hnat_diag_note(s, r, 0, 0, -1, 0, -1), -1)
#define hnat_diag_skip(s, r) (hnat_diag_note(s, r, 0, 0, -1, 0, -1), 0)
void hnat_diag_create(struct dentry *root);
void hnat_diag_remove(void);
bool hnat_diag_npu_entry(const struct foe_entry *entry);
void hnat_diag_bind(struct nf_conn *ct, u8 ctinfo, u32 ppe, u32 slot,
		    u64 generation, const struct foe_entry *entry, bool prefill);
void hnat_diag_link_ct(struct nf_conn *ct, u8 ctinfo, u32 tunnel,
		       u64 generation, u8 path);
void hnat_diag_note_ct_at(struct nf_conn *ct, u8 ctinfo,
			  enum hnat_diag_reason reason, int error, u8 path,
			  const char *function, unsigned int line);
#define hnat_diag_note_ct(ct, i, r, e, p) \
	hnat_diag_note_ct_at(ct, i, r, e, p, __func__, __LINE__)
bool hnat_account_diag_sample(struct mtk_hnat *h, const struct hnat_diag_key *key,
			      u32 ppe, u32 slot, u64 generation,
			      struct hnat_diag_sample *sample);
int hnat_diag_show(struct seq_file *s, void *unused);
#endif
