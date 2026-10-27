/* SPDX-License-Identifier: GPL-2.0 */
/* Exact function from 1ea2dd3a50e166445d8997211840f519285dd114; services supplied by the host harness. */
static void account_poll(struct work_struct *work)
{
	struct hnat_account_ctx *ctx = container_of(to_delayed_work(work),
						struct hnat_account_ctx, poll);
	struct mtk_hnat *h = ctx->h;
	u32 ppe_count, slot_count, n, budget;
	u64 started = ktime_get_ns();

	if (READ_ONCE(ctx->stopping) || !account_poll_interval_ms)
		return;
	ppe_count = min_t(u32, h->ppe_num,
		min_t(u32, ctx->ppe_num, min_t(u32, MAX_PPE_NUM, CFG_PPE_NUM)));
	slot_count = min(h->foe_etry_num, ctx->slot_count);
	if (!ppe_count || !slot_count || !READ_ONCE(h->nf_stat_en))
		goto requeue;
	budget = min_t(u64, account_poll_budget, (u64)ppe_count * slot_count);
	for (n = 0; n < budget; n++) {
		struct hnat_account_ppe *p;
		struct hnat_slot *s;
		struct hnat_accounting d;
		u32 ppe = ctx->poll_ppe % ppe_count;
		u32 slot = ctx->poll_slot % slot_count;
		int err = 0;

		/* A service-time budget, not a guarantee about scheduler/lock waits.
		 * One already-started SER transaction can exceed the deadline.
		 */
		if (READ_ONCE(ctx->stopping) ||
		    ktime_get_ns() - started >= HNAT_ACCOUNT_POLL_TIME_NS)
			break;
		ctx->poll_ppe = ppe + 1;
		if (ctx->poll_ppe == ppe_count) {
			ctx->poll_ppe = 0;
			ctx->poll_slot = (slot + 1) % slot_count;
		}
		if (!account_valid(h, ppe, slot))
			continue;
		p = &ctx->ppe[ppe];
		s = &p->slots[slot];
		/* Background sampling must not wait for a forwarding-path reader. */
		if (!spin_trylock_bh(&p->lock)) {
			atomic64_inc(&ctx->poll_lock_skips[ppe]);
			continue;
		}
		/* Check again after acquisition; never start a late SER transaction. */
		if (READ_ONCE(ctx->stopping) ||
		    ktime_get_ns() - started >= HNAT_ACCOUNT_POLL_TIME_NS) {
			spin_unlock_bh(&p->lock);
			break;
		}
		p->poll_scanned++;
		if (!p->paused && s->owner && !s->owner->prefill) {
			if (!owner_live(h, ppe, slot)) {
				p->mismatch++;
				p->unknown_loss++;
				retire_locked(p, s);
			} else {
				err = drain_locked(h, ppe, slot, &d);
				if (!err) {
					p->poll_reads++;
					if (owner_live(h, ppe, slot) &&
					    !flush_ct_locked(p, s))
						p->no_acct++;
				} else {
					p->poll_errors++;
				}
			}
		}
		spin_unlock_bh(&p->lock);
		/* Abort on busy, quarantine or timeout instead of burning the budget. */
		if (err)
			break;
	}
requeue:
	if (!READ_ONCE(ctx->stopping))
		schedule_delayed_work(&ctx->poll,
				      msecs_to_jiffies(account_poll_interval_ms));
}
