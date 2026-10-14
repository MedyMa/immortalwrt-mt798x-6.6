/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _HNAT_ACCOUNT_H
#define _HNAT_ACCOUNT_H
struct hnat_account_ctx;
struct nf_conn;
struct seq_file;
void hnat_account_admin_lock(void);
void hnat_account_admin_unlock(void);
int hnat_account_init(struct mtk_hnat *h);
void hnat_account_cleanup(struct mtk_hnat *h);
bool hnat_account_enabled(struct mtk_hnat *h);
int hnat_account_drain(struct mtk_hnat *h, u32 ppe, u32 slot,
		       struct hnat_accounting *total, struct hnat_accounting *delta);
bool hnat_account_entry_snapshot(struct mtk_hnat *h, u32 ppe, u32 slot,
				 struct foe_entry *entry, u64 *generation);
bool hnat_account_commit(struct mtk_hnat *h, u32 ppe, u32 slot,
			 struct sk_buff *skb, const struct foe_entry *entry,
			 bool prefill, bool confirm, u64 expected_generation);
void hnat_account_invalidate(struct mtk_hnat *h, u32 ppe, u32 slot);
bool hnat_account_stop_entry(struct mtk_hnat *h, u32 ppe, u32 slot);
bool hnat_account_clear(struct mtk_hnat *h, u32 ppe, u32 slot, bool erase);
/* True includes a stale token already handled without descriptor mutation. */
bool hnat_account_clear_if_generation(struct mtk_hnat *h, u32 ppe, u32 slot,
				      bool erase, u64 generation);
bool hnat_account_clear_snapshot(struct mtk_hnat *h, u32 ppe, u32 slot,
				 struct foe_entry *entry, u64 *generation);
bool hnat_account_clear_entry(struct mtk_hnat *h, struct foe_entry *entry);
void hnat_account_pause(struct mtk_hnat *h, u32 ppe);
void hnat_account_resume(struct mtk_hnat *h, u32 ppe);
/* Caller holds admin serialization. Leaves the table paused for resume(). */
bool hnat_account_reset_table(struct mtk_hnat *h, u32 ppe);
bool hnat_account_publish_delta(struct mtk_hnat *h, u32 ppe, u32 slot,
				struct sk_buff *skb);
int hnat_account_show(struct seq_file *m, void *unused);
#endif
