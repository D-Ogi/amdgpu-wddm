/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * BC-250 overclock tiers (include/bc250_oc.h, docs/design/dpm.md). Pure: the miniport (driver/kmd/oc_tier.c) reads
 * and persists, this file decides. Lepiej dmuchać na zimne - better to blow on it cold: the default answer of every
 * doubtful case below is the tier none.
 */
#include <string.h>
#include "bc250_oc.h"

void bc250_oc_decide(const struct bc250_oc_request *r, struct bc250_oc_decision *d)
{
	memset(d, 0, sizeof(*d));
	d->tier = BC250_OC_TIER_NONE;
	if (!r->tier_present || r->tier == BC250_OC_TIER_NONE) {
		/* A binding with no tier describes nothing: it goes. */
		d->reason = BC250_OC_REASON_NONE;
		d->clear = r->board_present;
		return;
	}
	if (r->tier >= BC250_OC_TIER_COUNT) {
		/* Only the application writes OcTier, so this is somebody's hand edit: kept for them to read, never run. */
		d->reason = BC250_OC_REASON_INVALID;
		return;
	}
	if (!r->supported || r->current_board == 0u) {
		/* No positive identification on this start. That can be a foreign device or a failed firmware table
		 * read, and neither is evidence that the board changed, so the choice stays stored and does not run. */
		d->reason = BC250_OC_REASON_UNSUPPORTED;
		return;
	}
	if (!r->board_present || r->board == 0u) {
		d->reason = BC250_OC_REASON_UNBOUND;
		d->clear = 1;
		return;
	}
	if (r->board != r->current_board) {
		/* The person answered for the cooling of another board (or of this one before a firmware update). */
		d->reason = BC250_OC_REASON_BOARD_CHANGED;
		d->clear = 1;
		return;
	}
	d->tier = r->tier;
	d->reason = BC250_OC_REASON_OK;
}

unsigned int bc250_oc_board_fingerprint(const unsigned char machine_id[16], unsigned int bios_id)
{
	unsigned int i, h = 2166136261u;
	for (i = 0; i < 16u; i++) {
		h ^= machine_id[i];
		h *= 16777619u;
	}
	for (i = 0; i < 4u; i++) {
		h ^= (bios_id >> (8u * i)) & 0xFFu;
		h *= 16777619u;
	}
	return h ? h : 1u;
}
