/* SPDX-License-Identifier: GPL-2.0 */
/* Pure arithmetic shared by the driver and host regression harness. */
#ifndef _HNAT_ACCOUNT_LEDGER_H
#define _HNAT_ACCOUNT_LEDGER_H
static inline void hnat_ledger_add(struct hnat_accounting *total,
				   struct hnat_accounting *pending,
				   const struct hnat_accounting *delta,
				   bool tracked)
{
	total->bytes += delta->bytes;
	total->packets += delta->packets;
	if (tracked) {
		pending->bytes += delta->bytes;
		pending->packets += delta->packets;
	}
}
static inline struct hnat_accounting
hnat_ledger_take(struct hnat_accounting *pending)
{
	struct hnat_accounting delta = *pending;
	pending->bytes = 0;
	pending->packets = 0;
	return delta;
}
#endif
