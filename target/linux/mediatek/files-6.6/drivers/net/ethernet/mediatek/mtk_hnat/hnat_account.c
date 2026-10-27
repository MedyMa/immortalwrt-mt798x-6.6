// SPDX-License-Identifier: GPL-2.0
/* Owner-scoped read-clear accounting with a bounded MIB poller. */
#include <linux/module.h>
#include <linux/atomic.h>
#include <linux/slab.h>
#include <linux/rcupdate.h>
#include <linux/mutex.h>
#include <linux/iopoll.h>
#include <linux/seq_file.h>
#include <linux/workqueue.h>
#include <linux/ktime.h>
#include <linux/jiffies.h>
#include <linux/bitmap.h>
#include <net/net_namespace.h>
#include <net/netns/generic.h>
#include <net/netfilter/nf_conntrack.h>
#include <net/netfilter/nf_conntrack_acct.h>
#include <net/netfilter/nf_conntrack_zones.h>
#ifdef CONFIG_NF_HNAT_PACKET_ACCOUNTING
#include <net/netfilter/nf_hnat_packet_accounting.h>
#endif
#include "hnat.h"
#include "hnat_account_ledger.h"

#ifdef CONFIG_NF_HNAT_PACKET_ACCOUNTING
static bool experimental_accounting = true;
static unsigned int account_poll_interval_ms = 100;
#else
/* Targets without the matching kernel helpers retain the legacy path. */
static bool experimental_accounting;
static unsigned int account_poll_interval_ms;
#endif
static DEFINE_MUTEX(account_admin_mutex);
void hnat_account_admin_lock(void)
{
	if (experimental_accounting)
		mutex_lock(&account_admin_mutex);
}
void hnat_account_admin_unlock(void)
{
	if (experimental_accounting)
		mutex_unlock(&account_admin_mutex);
}
module_param(experimental_accounting, bool, 0444);
MODULE_PARM_DESC(experimental_accounting, "HNAT owner accounting (default on with packet accounting built)");
static unsigned int account_poll_budget = 64;
module_param(account_poll_interval_ms, uint, 0444);
module_param(account_poll_budget, uint, 0444);
MODULE_PARM_DESC(account_poll_interval_ms, "MIB polling interval, 0=off or 100..60000 ms");
MODULE_PARM_DESC(account_poll_budget, "Maximum slots examined per poll across all PPEs, 1..64");
#define HNAT_ACCOUNT_POLL_TIME_NS 2000000ULL

static unsigned int account_net_id;
struct hnat_account_net { bool exiting; };

struct hnat_owner {
	struct nf_conn *ct;
	struct net *net; /* borrowed from ct; ct reference pins its lifetime */
	struct nf_conntrack_zone zone;
	struct rcu_head rcu;
	u64 generation;
	struct foe_entry identity;
	unsigned int dir;
	unsigned long seen;
	bool prefill;
};
struct hnat_slot {
	struct hnat_owner *owner;
	struct hnat_accounting pending;
	struct hnat_accounting pending_ct;
	u64 generation;
};
struct hnat_account_ppe {
	struct mtk_hnat *h;
	spinlock_t lock; /* entry_lock -> this lock; never acquire entry_lock here */
	struct hnat_slot *slots;
	unsigned long *poll_active; /* bound, owned epochs only; protected by lock */
	u32 poll_cursor;
	u32 cursor;
	bool paused;
	bool quarantined;
	u64 reads, read_error, unknown_loss, untracked, mismatch;
	u64 retired, no_acct, discarded_bytes, discarded_packets;
	u64 retire_rule_bytes, retire_rule_packets;
	u64 ct_written_bytes, ct_written_packets;
	u64 poll_scanned, poll_reads, poll_errors;
};
struct hnat_account_ctx {
	struct mtk_hnat *h;
	u32 ppe_num; /* allocation bounds, immutable until cleanup */
	u32 slot_count;
	struct hnat_account_ppe ppe[MAX_PPE_NUM];
	atomic64_t poll_lock_skips[MAX_PPE_NUM];
	struct delayed_work gc;
	struct delayed_work poll;
	u32 poll_ppe;
	bool stopping;
};

bool hnat_account_enabled(struct mtk_hnat *h)
{
	return h && h->account;
}

static bool account_valid(struct mtk_hnat *h, u32 ppe, u32 slot)
{
	return h && h->data && h->data->per_flow_accounting && h->account &&
		ppe < MAX_PPE_NUM && ppe < CFG_PPE_NUM && ppe < h->ppe_num &&
		ppe < h->account->ppe_num &&
		slot < h->foe_etry_num && slot < h->account->slot_count && h->ppe_base[ppe] &&
		h->acct[ppe] && h->foe_table_cpu[ppe] && h->account->ppe[ppe].slots;
}

/* Ignore info1 timestamps/state, which hardware changes independently. */
static bool account_identity(const struct foe_entry *a, const struct foe_entry *b)
{
	struct foe_entry left = *a, right = *b;

	if (IS_IPV4_HNAPT(a) || IS_IPV4_HNAT(a)) {
		left.ipv4_hnapt.iblk2.dscp = right.ipv4_hnapt.iblk2.dscp = 0;
		left.ipv4_hnapt.m_timestamp = right.ipv4_hnapt.m_timestamp = 0;
	} else if (IS_IPV6_5T_ROUTE(a) || IS_IPV6_HNAPT(a) || IS_IPV6_HNAT(a)) {
		left.ipv6_5t_route.iblk2.dscp = right.ipv6_5t_route.iblk2.dscp = 0;
	}
	a = &left;
	b = &right;
	return a->bfib1.pkt_type == b->bfib1.pkt_type &&
		a->bfib1.udp == b->bfib1.udp &&
		!memcmp((const u8 *)a + sizeof(a->bfib1),
			(const u8 *)b + sizeof(b->bfib1),
			sizeof(*a) - sizeof(a->bfib1));
}

static void owner_free(struct rcu_head *rcu)
{
	struct hnat_owner *owner = container_of(rcu, struct hnat_owner, rcu);
	nf_ct_put(owner->ct);
	kfree(owner);
}

/* CT and rule ledgers are separate: polling can update CT without consuming
 * the increment still owed to a real keepalive's rule traversal.
 */
static bool flush_ct_locked(struct hnat_account_ppe *p, struct hnat_slot *s)
{
	struct nf_conn_acct *acct = s->owner ? nf_conn_acct_find(s->owner->ct) : NULL;
	struct hnat_accounting d;

	if (!acct || !READ_ONCE(p->h->nf_stat_en))
		return false;
	d = hnat_ledger_take(&s->pending_ct);
	atomic64_add(d.bytes, &acct->counter[s->owner->dir].bytes);
	atomic64_add(d.packets, &acct->counter[s->owner->dir].packets);
	p->ct_written_bytes += d.bytes;
	p->ct_written_packets += d.packets;
	return true;
}

/* Never execute conntrack destruction recursively under accounting/entry locks. */
static void retire_locked(struct hnat_account_ppe *p, struct hnat_slot *s)
{
	struct hnat_accounting d = hnat_ledger_take(&s->pending);

	__clear_bit(s - p->slots, p->poll_active);
	/* These bytes have already passed identity validation. Retire them to
	 * their pinned old CT, but never synthesize shared diff / a rule path.
	 */
	if (!flush_ct_locked(p, s)) {
		struct hnat_accounting unaccounted = hnat_ledger_take(&s->pending_ct);
		p->discarded_bytes += unaccounted.bytes;
		p->discarded_packets += unaccounted.packets;
	}
	p->retire_rule_bytes += d.bytes;
	p->retire_rule_packets += d.packets;
	if (s->owner) {
		call_rcu(&s->owner->rcu, owner_free);
		s->owner = NULL;
		p->retired++;
	}
}

static bool owner_live(struct mtk_hnat *h, u32 ppe, u32 slot)
{
	struct hnat_owner *o = h->account->ppe[ppe].slots[slot].owner;
	struct foe_entry *entry = &h->foe_table_cpu[ppe][slot];

	return o && !nf_ct_is_dying(o->ct) &&
		o->generation == h->account->ppe[ppe].slots[slot].generation &&
		(o->prefill ? hnat_flow_entry_unbind_match(&o->identity, entry) :
		 entry->bfib1.state == BIND && account_identity(&o->identity, entry));
}

/* Caller holds per-PPE lock across the complete SER transaction and ledger. */
static int drain_locked(struct mtk_hnat *h, u32 ppe, u32 slot,
			struct hnat_accounting *delta)
{
	struct hnat_account_ppe *p = &h->account->ppe[ppe];
	struct hnat_slot *s = &p->slots[slot];
	void __iomem *base = h->ppe_base[ppe];
	u32 val, r0, r1, r2, r3;
	int err;

	delta->bytes = delta->packets = 0;
	if (p->paused)
		return -EBUSY;
	/* A timed-out transaction may still be completing. Never overwrite it.
 * Once idle, throw away its result and report the unknown loss explicitly.
 */
	val = readl(base + PPE_MIB_SER_CR);
	if (val & BIT_MIB_BUSY)
		return -EBUSY;
	if (p->quarantined) {
		p->quarantined = false;
		return -EAGAIN;
	}
	if (s->owner && !owner_live(h, ppe, slot)) {
		p->mismatch++;
		retire_locked(p, s);
	}
	writel(slot | BIT(16), base + PPE_MIB_SER_CR);
	err = readx_poll_timeout_atomic(readl, base + PPE_MIB_SER_CR,
				       val, !(val & BIT_MIB_BUSY), 20, 1000);
	if (err) {
		p->read_error++;
		p->unknown_loss++;
		p->quarantined = true;
		return err;
	}
	r0 = readl(base + PPE_MIB_SER_R0);
	r1 = readl(base + PPE_MIB_SER_R1);
	r2 = readl(base + PPE_MIB_SER_R2);
	if (h->data->version == MTK_HNAT_V3) {
		r3 = readl(base + PPE_MIB_SER_R3);
		delta->bytes = r0 + ((u64)r1 << 32);
		delta->packets = r2 + ((u64)r3 << 32);
	} else {
		delta->bytes = r0 + ((u64)(r1 & 0xffff) << 32);
		delta->packets = (r1 >> 16) + ((u64)(r2 & 0xffffff) << 16);
	}
	if (s->owner && !owner_live(h, ppe, slot)) {
		p->mismatch++;
		retire_locked(p, s);
	}
	hnat_ledger_add(&h->acct[ppe][slot], &s->pending, delta,
			s->owner && !s->owner->prefill);
	if (s->owner && !s->owner->prefill) {
		s->pending_ct.bytes += delta->bytes;
		s->pending_ct.packets += delta->packets;
		/* Every read clears the hardware MIB, including keepalive/debug and
		 * retirement readers. Publish its verified CT delta here exactly once;
		 * the independent rule ledger remains pending for a real carrier skb.
		 */
		if (!flush_ct_locked(p, s))
			p->no_acct++;
	}
	if (!s->owner || s->owner->prefill) {
		p->discarded_bytes += delta->bytes;
		p->discarded_packets += delta->packets;
	}
	p->reads++;
	return 0;
}

int hnat_account_drain(struct mtk_hnat *h, u32 ppe, u32 slot,
		       struct hnat_accounting *total, struct hnat_accounting *delta)
{
	struct hnat_accounting d;
	struct hnat_account_ppe *p;
	int err;
	if (!account_valid(h, ppe, slot))
		return -EINVAL;
	p = &h->account->ppe[ppe];
	spin_lock_bh(&p->lock);
	err = drain_locked(h, ppe, slot, &d);
	if (!err && total)
		*total = h->acct[ppe][slot];
	if (delta)
		*delta = d;
	spin_unlock_bh(&p->lock);
	return err;
}

bool hnat_account_entry_snapshot(struct mtk_hnat *h, u32 ppe, u32 slot,
				 struct foe_entry *entry, u64 *generation)
{
	struct hnat_account_ppe *p;
	if (!account_valid(h, ppe, slot))
		return false;
	p = &h->account->ppe[ppe];
	spin_lock_bh(&p->lock);
	*entry = h->foe_table_cpu[ppe][slot];
	*generation = p->slots[slot].generation;
	if (p->paused)
		entry->bfib1.state = BIND; /* caller skips publication */
	spin_unlock_bh(&p->lock);
	return true;
}

bool hnat_account_generation_snapshot(struct mtk_hnat *h, u32 ppe, u32 slot,
				      u64 *generation)
{
	struct hnat_account_ppe *p;

	if (!generation || !account_valid(h, ppe, slot))
		return false;
	p = &h->account->ppe[ppe];
	spin_lock_bh(&p->lock);
	if (p->paused) {
		spin_unlock_bh(&p->lock);
		return false;
	}
	*generation = p->slots[slot].generation;
	spin_unlock_bh(&p->lock);
	return true;
}

/* Conditional deletion observes the descriptor and token under the same PPE lock.
 * Unlike the publication snapshot, this preserves the actual state when paused. */
bool hnat_account_clear_snapshot(struct mtk_hnat *h, u32 ppe, u32 slot,
				 struct foe_entry *entry, u64 *generation)
{
	struct hnat_account_ppe *p;
	if (!entry || !generation || !account_valid(h, ppe, slot))
		return false;
	p = &h->account->ppe[ppe];
	spin_lock_bh(&p->lock);
	*entry = h->foe_table_cpu[ppe][slot];
	*generation = p->slots[slot].generation;
	spin_unlock_bh(&p->lock);
	return true;
}

/* Own the accounting epoch. A WHNAT prefill records its owner while leaving
 * the hardware descriptor UNBIND; confirm drains and revalidates immediately
 * before publishing. Return true means accounting handled the operation (so
 * callers must not fall through to an unguarded memcpy). accepted distinguishes
 * an accepted prefill/publish from a handled rejection.
 */
static bool account_commit(struct mtk_hnat *h, u32 ppe, u32 slot,
			 struct sk_buff *skb, const struct foe_entry *entry,
			 bool prefill, bool confirm, u64 expected_generation,
			 bool *accepted, struct nf_conn *saved_ct,
			 enum ip_conntrack_info saved_info, bool require_owner)
{
	struct hnat_account_ppe *p;
	struct hnat_slot *s;
	struct hnat_owner *o = NULL;
	struct hnat_owner *confirm_owner;
	struct hnat_accounting d;
	enum ip_conntrack_info info = saved_info;
	struct nf_conn *ct = skb ? nf_ct_get(skb, &info) : saved_ct;
	struct foe_entry *foe;
	int err;

	if (accepted)
		*accepted = false;
	if (!account_valid(h, ppe, slot))
		return false;
	p = &h->account->ppe[ppe];
	s = &p->slots[slot];
	foe = &h->foe_table_cpu[ppe][slot];
	if (!confirm && ct && !nf_ct_is_template(ct) && !nf_ct_is_dying(ct) &&
	    !READ_ONCE(((struct hnat_account_net *)net_generic(nf_ct_net(ct), account_net_id))->exiting) &&
	    (IS_IPV4_HNAPT(entry) || IS_IPV4_HNAT(entry) ||
	     IS_IPV6_5T_ROUTE(entry) || IS_IPV6_HNAPT(entry) || IS_IPV6_HNAT(entry))) {
		o = kzalloc(sizeof(*o), GFP_ATOMIC);
		if (o) {
			nf_conntrack_get(&ct->ct_general);
			o->ct = ct;
			o->net = nf_ct_net(ct);
			o->zone = *nf_ct_zone(ct);
			o->dir = CTINFO2DIR(info);
			o->prefill = prefill;
			o->seen = jiffies;
		}
	}
	spin_lock_bh(&p->lock);
	if (o && READ_ONCE(((struct hnat_account_net *)net_generic(o->net, account_net_id))->exiting)) {
		call_rcu(&o->rcu, owner_free);
		o = NULL;
	}
	if (p->paused) {
		if (o)
			call_rcu(&o->rcu, owner_free);
		spin_unlock_bh(&p->lock);
		return true; /* handled: block publication during reset/stop */
	}
	if (confirm && s->generation != expected_generation) {
		p->mismatch++;
		spin_unlock_bh(&p->lock);
		return true; /* handled: stale software epoch must not publish */
	}
	if (confirm && require_owner && (!ct || !s->owner)) {
		/* A TOPS encap flow must retain its verified INNER CT owner. An
		 * allocation failure is a software fallback, not an unaccounted
		 * hardware tunnel. Preserve the ordinary owner's optional policy.
		 */
		p->untracked++;
		spin_unlock_bh(&p->lock);
		return true;
	}
	if (confirm && ct && (nf_ct_is_template(ct) || nf_ct_is_dying(ct) ||
	    READ_ONCE(((struct hnat_account_net *)net_generic(nf_ct_net(ct), account_net_id))->exiting))) {
		p->mismatch++;
		spin_unlock_bh(&p->lock);
		return true;
	}
	if (confirm) {
		confirm_owner = s->owner;
		if ((confirm_owner &&
		     (!owner_live(h, ppe, slot) || !confirm_owner->prefill ||
		      (ct && (nf_ct_is_template(ct) || nf_ct_is_dying(ct) ||
			      ct != confirm_owner->ct ||
			      CTINFO2DIR(info) != confirm_owner->dir ||
			      nf_ct_net(ct) != confirm_owner->net ||
			      nf_ct_zone(ct)->id != confirm_owner->zone.id ||
			      nf_ct_zone(ct)->dir != confirm_owner->zone.dir ||
			      nf_ct_zone(ct)->flags != confirm_owner->zone.flags)))) ||
		    !hnat_flow_entry_unbind_match(entry, foe)) {
			p->mismatch++;
			if (confirm_owner)
				retire_locked(p, s);
			p->untracked++;
			spin_unlock_bh(&p->lock);
			return true; /* handled: do not fall through to direct FOE write */
		}
		err = drain_locked(h, ppe, slot, &d);
		if (err || (ct && nf_ct_is_dying(ct)) ||
		    s->generation != expected_generation ||
		    s->owner != confirm_owner ||
		    (confirm_owner &&
		     (!owner_live(h, ppe, slot) || !confirm_owner->prefill)) ||
		    !hnat_flow_entry_unbind_match(entry, foe)) {
			p->mismatch++;
			if (s->owner)
				retire_locked(p, s);
			p->untracked++;
			spin_unlock_bh(&p->lock);
			return true; /* drain may retire/change the owner; refuse publication */
		}
		if (confirm_owner) {
			confirm_owner->prefill = false;
			confirm_owner->seen = jiffies;
		} else {
			p->untracked++;
		}
	} else {
		retire_locked(p, s);
		/* Clear old hardware residue BEFORE installing a new owner. If the
 * read cannot complete, this entire epoch stays untracked, never guessed.
 */
		err = drain_locked(h, ppe, slot, &d);
		memset(&h->acct[ppe][slot], 0, sizeof(h->acct[ppe][slot]));
		s->generation++;
		if (err && o) {
			call_rcu(&o->rcu, owner_free);
			o = NULL;
		}
		if ((prefill || entry->bfib1.state == BIND) &&
		    !hnat_flow_entry_unbind_match(entry, foe)) {
			if (o)
				call_rcu(&o->rcu, owner_free);
			p->mismatch++;
			p->untracked++;
			spin_unlock_bh(&p->lock);
			return true; /* handled: live slot changed before commit */
		}
		s->owner = o;
		if (o)
			o->generation = s->generation;
		else
			p->untracked++;
	}
	if (!prefill) {
		/* Recheck immediately before the descriptor writes. PPE is not
		 * serialized by entry_lock or the accounting lock.
		 */
		if ((confirm && ct && nf_ct_is_dying(ct)) ||
		    !hnat_flow_entry_unbind_match(entry, foe)) {
			p->mismatch++;
			if (s->owner)
				retire_locked(p, s);
			p->untracked++;
			spin_unlock_bh(&p->lock);
			return true;
		}
		memcpy((u8 *)foe + sizeof(foe->bfib1),
		       (const u8 *)entry + sizeof(entry->bfib1),
		       sizeof(*entry) - sizeof(entry->bfib1));
		wmb();
		memcpy(&foe->bfib1, &entry->bfib1, sizeof(entry->bfib1));
		dma_wmb();
	}
	if (s->owner) {
		s->owner->identity = *foe;
		if (!s->owner->prefill)
			__set_bit(slot, p->poll_active);
	}
	if (accepted)
		*accepted = true;
	spin_unlock_bh(&p->lock);
	return true;
}

bool hnat_account_commit(struct mtk_hnat *h, u32 ppe, u32 slot,
			 struct sk_buff *skb, const struct foe_entry *entry,
			 bool prefill, bool confirm, u64 generation, bool *accepted)
{
	return account_commit(h, ppe, slot, skb, entry, prefill, confirm,
			      generation, accepted, NULL, 0, false);
}

bool hnat_account_tunnel_commit(struct mtk_hnat *h, u32 ppe, u32 slot,
			       struct nf_conn *ct, u8 ctinfo,
			       const struct foe_entry *entry, u64 generation,
			       bool *accepted)
{
	/* The flow shadow pins the verified INNER CT. Never replace the outer
	 * skb's conntrack association or account its bytes a second time.
	 */
	return account_commit(h, ppe, slot, NULL, entry, false, true,
			      generation, accepted, ct, ctinfo, true);
}

bool hnat_account_publish_delta(struct mtk_hnat *h, u32 ppe, u32 slot,
				struct sk_buff *skb)
{
#ifdef CONFIG_NF_HNAT_PACKET_ACCOUNTING
	struct hnat_account_ppe *p;
	struct hnat_slot *s;
	struct nf_conn *ct;
	struct nf_conn_acct *acct;
	struct hnat_accounting d;
	const struct nf_conntrack_zone *zone;
	enum ip_conntrack_info ctinfo;
	unsigned int dir;
	bool published = false;

	if (!skb)
		return false;
	ct = nf_ct_get(skb, &ctinfo);
	dir = CTINFO2DIR(ctinfo);
	if (!ct || dir >= IP_CT_DIR_MAX)
		goto zero;
	acct = nf_conn_acct_find(ct);
	if (!account_valid(h, ppe, slot))
		goto zero;
	/* Reserve before reading/taking the ledger. Terminal/copied/published skbs
	 * cannot re-enter; allocation failure keeps pending for a future carrier.
	 */
	if (!nf_hnat_packet_accounting_prepare(skb))
		goto zero;
	p = &h->account->ppe[ppe];
	s = &p->slots[slot];
	spin_lock_bh(&p->lock);
	if (p->paused || !owner_live(h, ppe, slot))
		goto out;
	zone = nf_ct_zone(ct);
	if (s->owner->prefill || ct != s->owner->ct || dir != s->owner->dir ||
	    nf_ct_net(ct) != s->owner->net || zone->id != s->owner->zone.id ||
	    zone->dir != s->owner->zone.dir || zone->flags != s->owner->zone.flags) {
		p->mismatch++;
		goto out;
	}
	if (drain_locked(h, ppe, slot, &d))
		goto out;
	/* Hardware may have changed identity during SER; drain can retire it. */
	if (!owner_live(h, ppe, slot) || s->owner->ct != ct ||
	    s->owner->dir != dir || s->owner->prefill)
		goto out;
	if (!acct) {
		p->no_acct++;
		goto out; /* preserve pending until extension is present or retirement */
	}
	/* Publish first: a failed identity/extension check must not consume pending.
	 * The current skb executes serially; only the PPE ledger is shared.
	 */
	d = s->pending;
	if (!nf_hnat_packet_accounting_publish(skb, d.bytes, d.packets))
		goto out;
	hnat_ledger_take(&s->pending);
	flush_ct_locked(p, s);
	s->owner->seen = jiffies;
	published = true;
out:
	spin_unlock_bh(&p->lock);
zero:
	nf_hnat_packet_accounting_finish_zero(skb);
	return published;
#else
	return false;
#endif
}

void hnat_account_invalidate(struct mtk_hnat *h, u32 ppe, u32 slot)
{
	struct hnat_account_ppe *p;
	if (!account_valid(h, ppe, slot))
		return;
	p = &h->account->ppe[ppe];
	spin_lock_bh(&p->lock);
	retire_locked(p, &p->slots[slot]);
	p->slots[slot].generation++;
	/* No retirement MIB read: hardware tail remains explicitly uncertain. */
	p->unknown_loss++;
	spin_unlock_bh(&p->lock);
}

bool hnat_account_tunnel_invalidate(struct mtk_hnat *h, u32 ppe, u32 slot)
{
	struct hnat_account_ppe *p;
	struct hnat_accounting delta;
	struct foe_entry before;
	struct foe_entry *foe;
	u32 info1;

	if (!account_valid(h, ppe, slot))
		return false;
	lockdep_assert_held(&h->entry_lock);
	p = &h->account->ppe[ppe];
	foe = &h->foe_table_cpu[ppe][slot];
	spin_lock_bh(&p->lock);
	memcpy(&before, foe, sizeof(before));
	/* drain_locked attributes only a live owner's verified identity.  The
	 * return value never authorizes publishing data to a different owner.
	 * A timeout remains quarantined and counted by the existing ledger.
	 */
	drain_locked(h, ppe, slot, &delta);
	/* Hardware is not frozen by software locks.  Do not invalidate a slot
	 * that was aged/relearned while the MIB transaction was in progress.
	 */
	if (foe->bfib1.state != BIND || !account_identity(&before, foe)) {
		p->mismatch++;
		spin_unlock_bh(&p->lock);
		return true; /* handled: never fall through to an unguarded clear */
	}
	info1 = READ_ONCE(foe->ipv4_hnapt.info_blk1);
	WRITE_ONCE(foe->ipv4_hnapt.info_blk1, info1 & ~GENMASK(29, 28));
	dma_wmb();
	retire_locked(p, &p->slots[slot]);
	p->slots[slot].generation++;
	/* PPE may finish an in-flight packet after the last MIB read.  Keep that
	 * hardware tail explicitly uncertain instead of claiming lossless totals.
	 */
	p->unknown_loss++;
	spin_unlock_bh(&p->lock);
	return true;
}

static bool account_clear(struct mtk_hnat *h, u32 ppe, u32 slot, bool erase,
			  bool conditional, u64 generation, bool stopping)
{
	struct hnat_account_ppe *p;
	if (!account_valid(h, ppe, slot))
		return false;
	p = &h->account->ppe[ppe];
	spin_lock_bh(&p->lock);
	/* A stale request is handled: never fall back to an unguarded clear. */
	if (conditional && p->slots[slot].generation != generation) {
		spin_unlock_bh(&p->lock);
		return true;
	}
	if (stopping && h->foe_table_cpu[ppe][slot].bfib1.state != BIND) {
		spin_unlock_bh(&p->lock);
		return true;
	}
	retire_locked(p, &p->slots[slot]);
	p->slots[slot].generation++;
	p->unknown_loss++;
	if (erase)
		memset(&h->foe_table_cpu[ppe][slot], 0, sizeof(struct foe_entry));
	else {
		struct foe_entry *entry = &h->foe_table_cpu[ppe][slot];
		entry->bfib1.state = INVALID;
		if (h->fe_base)
			entry->bfib1.time_stamp = readl(h->fe_base + 0x0010) & 0xff;
		/* Hook stop preserves the pingpong interface field. */
		if (!stopping) {
			if (IS_IPV4_GRP(entry))
				entry->ipv4_hnapt.act_dp &= ~UDF_PINGPONG_IFIDX;
			else
				entry->ipv6_5t_route.act_dp &= ~UDF_PINGPONG_IFIDX;
		}
	}
	dma_wmb();
	spin_unlock_bh(&p->lock);
	return true;
}

/* Caller has paused the PPE; only INVALID + timestamp change on hook stop. */
bool hnat_account_stop_entry(struct mtk_hnat *h, u32 ppe, u32 slot)
{
	return account_clear(h, ppe, slot, false, false, 0, true);
}

bool hnat_account_clear(struct mtk_hnat *h, u32 ppe, u32 slot, bool erase)
{
	return account_clear(h, ppe, slot, erase, false, 0, false);
}

bool hnat_account_clear_if_generation(struct mtk_hnat *h, u32 ppe, u32 slot,
				      bool erase, u64 generation)
{
	return account_clear(h, ppe, slot, erase, true, generation, false);
}

bool hnat_account_clear_entry(struct mtk_hnat *h, struct foe_entry *entry)
{
	u32 ppe;
	unsigned long addr = (unsigned long)entry;
	if (!h || !h->account || !entry)
		return false;
	for (ppe = 0; ppe < h->ppe_num && ppe < MAX_PPE_NUM &&
	     ppe < CFG_PPE_NUM && ppe < h->account->ppe_num; ppe++) {
		unsigned long base = (unsigned long)h->foe_table_cpu[ppe];
		unsigned long offset = addr - base;
		if (base && addr >= base && offset < h->foe_etry_num * sizeof(*entry) &&
		    !(offset % sizeof(*entry)))
			return hnat_account_clear(h, ppe, offset / sizeof(*entry), false);
	}
	return false;
}

void hnat_account_pause(struct mtk_hnat *h, u32 ppe)
{
	struct hnat_account_ppe *p;
	u32 i;
	if (!h || !h->account || ppe >= MAX_PPE_NUM || ppe >= CFG_PPE_NUM ||
	    ppe >= h->ppe_num || ppe >= h->account->ppe_num)
		return;
	p = &h->account->ppe[ppe];
	spin_lock_bh(&p->lock);
	p->paused = true;
	spin_unlock_bh(&p->lock);
	for (i = 0; i < min(h->foe_etry_num, h->account->slot_count); i++) {
		spin_lock_bh(&p->lock);
		if (p->slots[i].owner)
			p->unknown_loss++;
		retire_locked(p, &p->slots[i]);
		p->slots[i].generation++;
		spin_unlock_bh(&p->lock);
	}
}

bool hnat_account_reset_table(struct mtk_hnat *h, u32 ppe)
{
	u32 slot;

	if (!hnat_account_enabled(h))
		return false;
	/* Invalid experimental input is handled without unsafe legacy fallback. */
	if (!account_valid(h, ppe, 0))
		return true;
	hnat_account_pause(h, ppe);
	/* Commit observes paused under this same PPE lock. Single-entry clears,
	 * timer invalidations and snapshot readers serialize each descriptor.
	 * No lock is held while traversing the entire table and no MIB is polled.
	 */
	for (slot = 0; slot < min(h->foe_etry_num, h->account->slot_count); slot++)
		hnat_account_clear(h, ppe, slot, true);
	return true;
}

void hnat_account_resume(struct mtk_hnat *h, u32 ppe)
{
	struct hnat_account_ppe *p;
	if (!h || !h->account || ppe >= MAX_PPE_NUM || ppe >= CFG_PPE_NUM ||
	    ppe >= h->ppe_num || ppe >= h->account->ppe_num)
		return;
	p = &h->account->ppe[ppe];
	spin_lock_bh(&p->lock);
	if (!p->paused) {
		spin_unlock_bh(&p->lock);
		return;
	}
	/* Do not clear quarantine: reset is not proof the SER command completed. */
	if (h->acct[ppe])
		memset(h->acct[ppe], 0, min(h->foe_etry_num, h->account->slot_count) * sizeof(*h->acct[ppe]));
	p->paused = false;
	spin_unlock_bh(&p->lock);
}

static void account_gc(struct work_struct *work)
{
	struct hnat_account_ctx *ctx = container_of(to_delayed_work(work),
						struct hnat_account_ctx, gc);
	struct mtk_hnat *h = ctx->h;
	u32 ppe, n;
	if (READ_ONCE(ctx->stopping))
		return;
	/* Budget of 64 lifecycle checks per PPE per second; never MMIO/MIB. */
	for (ppe = 0; ppe < min_t(u32, h->ppe_num,
		min_t(u32, h->account->ppe_num, min_t(u32, MAX_PPE_NUM, CFG_PPE_NUM))); ppe++) {
		struct hnat_account_ppe *p = &ctx->ppe[ppe];
		for (n = 0; n < min_t(u32, 64, min(h->foe_etry_num, ctx->slot_count)); n++) {
			struct hnat_owner *o;
			u32 slot;

			/* Defer GC on contention; leave its cursor for the next pass. */
			if (READ_ONCE(ctx->stopping) || !spin_trylock_bh(&p->lock))
				break;
			if (p->paused) {
				spin_unlock_bh(&p->lock);
				break;
			}
			slot = p->cursor++ % min(h->foe_etry_num, ctx->slot_count);
			o = p->slots[slot].owner;
			if (o && !owner_live(h, ppe, slot)) {
				p->unknown_loss++;
				retire_locked(p, &p->slots[slot]);
			}
			spin_unlock_bh(&p->lock);
		}
	}
	if (!READ_ONCE(ctx->stopping))
		schedule_delayed_work(&ctx->gc, HZ);
}

static void account_poll(struct work_struct *work)
{
	struct hnat_account_ctx *ctx = container_of(to_delayed_work(work),
						struct hnat_account_ctx, poll);
	struct mtk_hnat *h = ctx->h;
	u32 ppe_count, slot_count, n, budget, exhausted_count = 0;
	u32 start_cursor[MAX_PPE_NUM] = { 0 };
	bool started_ppe[MAX_PPE_NUM] = { false };
	bool wrapped[MAX_PPE_NUM] = { false };
	bool exhausted[MAX_PPE_NUM] = { false };
	u64 started = ktime_get_ns();

	if (READ_ONCE(ctx->stopping) || !account_poll_interval_ms)
		return;
	ppe_count = min_t(u32, h->ppe_num,
		min_t(u32, ctx->ppe_num, min_t(u32, MAX_PPE_NUM, CFG_PPE_NUM)));
	slot_count = min(h->foe_etry_num, ctx->slot_count);
	if (!ppe_count || !slot_count || !READ_ONCE(h->nf_stat_en))
		goto requeue;
	budget = min_t(u64, account_poll_budget, (u64)ppe_count * slot_count);
	for (n = 0; n < budget && exhausted_count < ppe_count;) {
		struct hnat_account_ppe *p;
		struct hnat_slot *s;
		struct hnat_accounting d;
		u32 ppe = ctx->poll_ppe % ppe_count;
		u32 slot;
		int err = 0;

		/* A service-time budget, not a guarantee about scheduler/lock waits.
		 * One already-started SER transaction can exceed the deadline.
		 */
		if (READ_ONCE(ctx->stopping) ||
		    ktime_get_ns() - started >= HNAT_ACCOUNT_POLL_TIME_NS)
			break;
		ctx->poll_ppe = (ppe + 1) % ppe_count;
		if (exhausted[ppe])
			continue;
		if (!account_valid(h, ppe, 0)) {
			exhausted[ppe] = true;
			exhausted_count++;
			continue;
		}
		p = &ctx->ppe[ppe];
		/* Background sampling must not wait for a forwarding-path reader. */
		if (!spin_trylock_bh(&p->lock)) {
			atomic64_inc(&ctx->poll_lock_skips[ppe]);
			exhausted[ppe] = true;
			exhausted_count++;
			continue;
		}
		/* Check again after acquisition; never start a late SER transaction. */
		if (READ_ONCE(ctx->stopping) ||
		    ktime_get_ns() - started >= HNAT_ACCOUNT_POLL_TIME_NS) {
			spin_unlock_bh(&p->lock);
			break;
		}
		if (!started_ppe[ppe]) {
			start_cursor[ppe] = min(p->poll_cursor, slot_count);
			started_ppe[ppe] = true;
		}
		slot = find_next_bit(p->poll_active, slot_count,
				     min(p->poll_cursor, slot_count));
		if (slot == slot_count && !wrapped[ppe]) {
			wrapped[ppe] = true;
			slot = find_first_bit(p->poll_active, slot_count);
		}
		/* At most one circular pass per PPE in this invocation, even when
		 * the first owner is retired concurrently. Sparse tables therefore
		 * do not wait for a scan through all 8192 slots or reread one owner.
		 */
		if (p->paused || slot == slot_count ||
		    (wrapped[ppe] && slot >= start_cursor[ppe])) {
			exhausted[ppe] = true;
			exhausted_count++;
			spin_unlock_bh(&p->lock);
			continue;
		}
		p->poll_cursor = slot + 1;
		s = &p->slots[slot];
		n++;
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

static void account_net_exit(struct net *net)
{
	struct mtk_hnat *h = hnat_priv;
	u32 ppe, slot;
	WRITE_ONCE(((struct hnat_account_net *)net_generic(net, account_net_id))->exiting, true);
	if (!h || !h->account)
		return;
	for (ppe = 0; ppe < h->account->ppe_num; ppe++) {
		struct hnat_account_ppe *p = &h->account->ppe[ppe];
		for (slot = 0; slot < h->account->slot_count; slot++) {
			struct hnat_owner *o;
			spin_lock_bh(&p->lock);
			o = p->slots[slot].owner;
			if (o && o->net == net) {
				p->unknown_loss++;
				retire_locked(p, &p->slots[slot]);
			}
			spin_unlock_bh(&p->lock);
		}
	}
}
static struct pernet_operations account_net_ops = {
	.exit = account_net_exit,
	.id = &account_net_id,
	.size = sizeof(struct hnat_account_net),
};

int hnat_account_init(struct mtk_hnat *h)
{
	struct hnat_account_ctx *ctx;
	u32 ppe;
	int err;
	if (!experimental_accounting)
		return 0;
#ifndef CONFIG_NF_HNAT_PACKET_ACCOUNTING
	/* Owner accounting cannot run while rule readers retain shared-CT deltas. */
	return -EOPNOTSUPP;
#endif
	if ((account_poll_interval_ms &&
	     (account_poll_interval_ms < 100 || account_poll_interval_ms > 60000)) ||
	    !account_poll_budget || account_poll_budget > 64)
		return -EINVAL;
	if (!h || !h->data || !h->data->per_flow_accounting ||
	    !h->ppe_num || h->ppe_num > MAX_PPE_NUM || h->ppe_num > CFG_PPE_NUM || !h->foe_etry_num)
		return -EINVAL;
	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->h = h;
	ctx->ppe_num = h->ppe_num;
	ctx->slot_count = h->foe_etry_num;
	for (ppe = 0; ppe < ctx->ppe_num; ppe++) {
		ctx->ppe[ppe].h = h;
		spin_lock_init(&ctx->ppe[ppe].lock);
		atomic64_set(&ctx->poll_lock_skips[ppe], 0);
		ctx->ppe[ppe].slots = kvcalloc(h->foe_etry_num,
					     sizeof(struct hnat_slot), GFP_KERNEL);
		if (!ctx->ppe[ppe].slots) {
			err = -ENOMEM;
			goto fail;
		}
		ctx->ppe[ppe].poll_active = bitmap_zalloc(h->foe_etry_num, GFP_KERNEL);
		if (!ctx->ppe[ppe].poll_active) {
			err = -ENOMEM;
			goto fail;
		}
	}
	INIT_DELAYED_WORK(&ctx->gc, account_gc);
	INIT_DELAYED_WORK(&ctx->poll, account_poll);
	h->account = ctx;
	err = register_pernet_subsys(&account_net_ops);
	if (err) {
		h->account = NULL;
		goto fail;
	}
#ifdef CONFIG_NF_HNAT_PACKET_ACCOUNTING
	nf_hnat_packet_accounting_enable();
#endif
	schedule_delayed_work(&ctx->gc, HZ);
	if (account_poll_interval_ms)
		schedule_delayed_work(&ctx->poll,
				      msecs_to_jiffies(account_poll_interval_ms));
	return 0;
fail:
	for (ppe = 0; ppe < ctx->ppe_num; ppe++) {
		bitmap_free(ctx->ppe[ppe].poll_active);
		kvfree(ctx->ppe[ppe].slots);
	}
	kfree(ctx);
	return err;
}

/* Called after hooks/debugfs have been removed, before table storage is freed. */
void hnat_account_cleanup(struct mtk_hnat *h)
{
	struct hnat_account_ctx *ctx;
	u32 ppe;
	if (!h || !h->account)
		return;
	ctx = h->account;
	WRITE_ONCE(ctx->stopping, true);
	cancel_delayed_work_sync(&ctx->poll);
	cancel_delayed_work_sync(&ctx->gc);
	unregister_pernet_subsys(&account_net_ops);
	for (ppe = 0; ppe < ctx->ppe_num; ppe++) {
		struct hnat_account_ppe *p = &ctx->ppe[ppe];
		u32 slot;
		spin_lock_bh(&p->lock);
		p->paused = true;
		spin_unlock_bh(&p->lock);
		for (slot = 0; slot < ctx->slot_count; slot++) {
			spin_lock_bh(&p->lock);
			retire_locked(p, &p->slots[slot]);
			spin_unlock_bh(&p->lock);
		}
	}
	rcu_barrier(); /* callbacks contain module text; unload must wait */
	h->account = NULL;
	for (ppe = 0; ppe < ctx->ppe_num; ppe++) {
		bitmap_free(ctx->ppe[ppe].poll_active);
		kvfree(ctx->ppe[ppe].slots);
	}
	kfree(ctx);
#ifdef CONFIG_NF_HNAT_PACKET_ACCOUNTING
	nf_hnat_packet_accounting_disable();
#endif
}

int hnat_account_show(struct seq_file *m, void *unused)
{
	struct mtk_hnat *h = m->private;
	u32 ppe;
	seq_printf(m, "experimental_enabled=%u\n", hnat_account_enabled(h));
	if (!hnat_account_enabled(h))
		return 0;
	seq_printf(m, "poll_interval_ms=%u poll_slot_budget=%u poll_service_budget_ns=%llu\n",
		   account_poll_interval_ms, account_poll_budget,
		   HNAT_ACCOUNT_POLL_TIME_NS);
	seq_puts(m, "coverage=software_epochs; hardware_same_tuple_reuse_and_final_tail_unproven\n");
	seq_puts(m, "rule_diff=packet_snapshot; early_raw_zero; keepalive_rule_attribution_estimate\n");
	for (ppe = 0; ppe < min_t(u32, h->ppe_num,
		min_t(u32, h->account->ppe_num, min_t(u32, MAX_PPE_NUM, CFG_PPE_NUM))); ppe++) {
		struct hnat_account_ppe *p = &h->account->ppe[ppe];
		struct hnat_account_ppe snap;
		u32 active;
		spin_lock_bh(&p->lock);
		snap = *p;
		active = bitmap_weight(p->poll_active, h->account->slot_count);
		spin_unlock_bh(&p->lock);
		seq_printf(m, "ppe=%u paused=%u quarantine=%u reads=%llu read_error=%llu unknown_loss_events=%llu untracked=%llu mismatch=%llu retired=%llu no_acct=%llu discarded_bytes=%llu discarded_packets=%llu retire_rule_unpublished_bytes=%llu retire_rule_unpublished_packets=%llu\n",
			   ppe, snap.paused, snap.quarantined, snap.reads,
			   snap.read_error, snap.unknown_loss, snap.untracked,
			   snap.mismatch, snap.retired, snap.no_acct,
			   snap.discarded_bytes, snap.discarded_packets,
			   snap.retire_rule_bytes, snap.retire_rule_packets);
		seq_printf(m, "ppe=%u poll_scanned=%llu poll_reads=%llu poll_errors=%llu poll_lock_skips=%llu ct_written_bytes=%llu ct_written_packets=%llu\n",
			   ppe, snap.poll_scanned, snap.poll_reads, snap.poll_errors,
			   (unsigned long long)atomic64_read(&h->account->poll_lock_skips[ppe]),
			   snap.ct_written_bytes, snap.ct_written_packets);
		seq_printf(m, "ppe=%u poll_active_owned_slots=%u poll_selection=owned_bitmap\n",
			   ppe, active);
	}
	return 0;
}
