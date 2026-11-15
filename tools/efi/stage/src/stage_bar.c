/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The identification of the register BAR (stage_bar.h). */
#include <stddef.h>
#include <string.h>
#include "stage_bar.h"
#include "bc250_mmio_bar.h"

/* The AMD register headers (third_party/linux-amdgpu/PROVENANCE.md). The IP base table needs the kernel's
 * attribute name. */
#ifndef __maybe_unused
#define __maybe_unused __attribute__((unused))
#endif
#include "cyan_skillfish_ip_offset.h"
#include "gc_10_1_0_offset.h"

/* The type 0 configuration header (PCI Local Bus Specification 3.0, section 6.1, section 6.2.5.1). */
#define PCI_COMMAND		0x04u
#define PCI_COMMAND_MEMORY	0x0002u
#define PCI_BAR0		0x10u
#define PCI_BAR_IO		0x1u
#define PCI_BAR_TYPE_MASK	0x6u
#define PCI_BAR_TYPE_32		0x0u
#define PCI_BAR_TYPE_64		0x4u
#define PCI_BAR_ADDRESS_MASK	(~0xFu)

/* ---- the resource descriptors -------------------------------------------------------------------- */

static unsigned long long le(const unsigned char *p, unsigned bytes)
{
	unsigned long long v = 0;
	unsigned i;
	for (i = bytes; i > 0; i--) v = (v << 8) | p[i - 1u];
	return v;
}

/* The width of the five numbers, their first offset and the least length field of an address space type. */
static int address_space_layout(unsigned tag, unsigned *width, unsigned *first, unsigned *least)
{
	switch (tag) {
	case STAGE_BAR_TAG_WORD: *width = 2u; *first = 6u; *least = 0x0Du; return 1;
	case STAGE_BAR_TAG_DWORD: *width = 4u; *first = 6u; *least = 0x17u; return 1;
	case STAGE_BAR_TAG_QWORD: *width = 8u; *first = 6u; *least = 0x2Bu; return 1;
	/* The extended type has a revision byte and a reserved byte before the numbers. */
	case STAGE_BAR_TAG_EXTENDED: *width = 8u; *first = 8u; *least = 0x35u; return 1;
	default: return 0;
	}
}

void stage_bar_parse(const unsigned char *data, size_t max_bytes, struct stage_bar_list *list)
{
	size_t at = 0;
	memset(list, 0, sizeof(*list));
	if (data == NULL) return;
	if (max_bytes > STAGE_BAR_LIST_MAX_BYTES) max_bytes = STAGE_BAR_LIST_MAX_BYTES;
	while (at < max_bytes) {
		struct stage_bar_resource r;
		unsigned tag = data[at], width, first, least;
		size_t bytes;
		memset(&r, 0, sizeof(r));
		if (tag & 0x80u) {
			/* A large item: a two-byte length of the data after the three-byte header. */
			if (max_bytes - at < 3u) return;
			bytes = 3u + (size_t)le(data + at + 1u, 2u);
		} else {
			/* A small item: the length of the data is in bits 2:0. */
			bytes = 1u + (tag & 0x7u);
		}
		if (bytes > max_bytes - at) return;
		if (tag == STAGE_BAR_TAG_END) {
			list->complete = 1;
			list->end_offset = (unsigned)at;
			return;
		}
		r.tag = tag;
		r.bytes = (unsigned)bytes;
		if (address_space_layout(tag, &width, &first, &least)) {
			if (bytes - 3u < least) return;
			r.address_space = 1;
			r.type = data[at + 3u];
			r.general = data[at + 4u];
			r.specific = data[at + 5u];
			r.granularity = le(data + at + first, width);
			r.minimum = le(data + at + first + width, width);
			r.maximum = le(data + at + first + 2u * width, width);
			r.translation = le(data + at + first + 3u * width, width);
			r.length = le(data + at + first + 4u * width, width);
		}
		if (list->count < STAGE_BAR_MAX_RESOURCES) list->resource[list->count++] = r;
		list->seen++;
		at += bytes;
	}
}

/* ---- the configuration header ------------------------------------------------------------------ */

static unsigned config32(const unsigned char *config, unsigned offset)
{
	return (unsigned)le(config + offset, 4u);
}

enum stage_bar_config stage_bar_config_address(const unsigned char *config, size_t bytes, unsigned long long *address)
{
	unsigned command, bar4, bar5;
	*address = 0;
	if (config == NULL || bytes < PCI_BAR0 + 4u * (BC250_BAR5_INDEX + 1u)) return STAGE_BAR_CONFIG_EMPTY;
	command = config32(config, PCI_COMMAND) & 0xFFFFu;
	bar4 = config32(config, PCI_BAR0 + 4u * (BC250_BAR5_INDEX - 1u));
	bar5 = config32(config, PCI_BAR0 + 4u * BC250_BAR5_INDEX);
	if (!(command & PCI_COMMAND_MEMORY)) return STAGE_BAR_CONFIG_DECODE_OFF;
	if (!(bar4 & PCI_BAR_IO) && (bar4 & PCI_BAR_TYPE_MASK) == PCI_BAR_TYPE_64) return STAGE_BAR_CONFIG_UPPER_HALF;
	if ((bar5 & PCI_BAR_IO) || (bar5 & PCI_BAR_TYPE_MASK) != PCI_BAR_TYPE_32) return STAGE_BAR_CONFIG_NOT_MEMORY;
	if ((bar5 & PCI_BAR_ADDRESS_MASK) == 0) return STAGE_BAR_CONFIG_EMPTY;
	*address = bar5 & PCI_BAR_ADDRESS_MASK;
	return STAGE_BAR_CONFIG_OK;
}

const char *stage_bar_config_text(enum stage_bar_config result)
{
	static const char *const text[STAGE_BAR_CONFIG_COUNT] = {
		"BAR5 is a 32-bit memory BAR",
		"the GPU function's memory decode is off",
		"BAR5 is not a 32-bit memory BAR",
		"BAR4 is a 64-bit memory BAR, so BAR5 is its upper half",
		"BAR5 holds no address",
	};
	return (unsigned)result < STAGE_BAR_CONFIG_COUNT ? text[result] : "unknown result";
}

/* ---- the access path ----------------------------------------------------------------------------- */

static int memory(const struct stage_bar_resource *r)
{
	return r->address_space && r->type == STAGE_BAR_SPACE_MEMORY;
}

enum stage_bar_match stage_bar_match(const struct stage_bar_list *list, unsigned long long address, unsigned long long length)
{
	enum stage_bar_match match = STAGE_BAR_MATCH_NONE;
	unsigned i;
	for (i = 0; i < list->count; i++) {
		const struct stage_bar_resource *r = &list->resource[i];
		if (!memory(r) || r->minimum != address || r->length == 0) continue;
		if (r->length != length) return STAGE_BAR_MATCH_CONFLICT;
		match = STAGE_BAR_MATCH_EXACT;
	}
	return match;
}

int stage_bar_window_holds(const struct stage_bar_list *list, unsigned long long address, unsigned long long length)
{
	unsigned i;
	if (length == 0 || address + length < address) return 0;
	for (i = 0; i < list->count; i++) {
		const struct stage_bar_resource *r = &list->resource[i];
		if (!memory(r) || r->length == 0 || r->minimum + r->length < r->minimum) continue;
		if (address >= r->minimum && address + length <= r->minimum + r->length) return 1;
	}
	return 0;
}

struct stage_bar_choice stage_bar_choose(const struct stage_bar_findings *f)
{
	struct stage_bar_choice c;
	unsigned i, exact = 0;
	memset(&c, 0, sizeof(c));
	c.path = STAGE_BAR_PATH_NONE;
	if (f->config != STAGE_BAR_CONFIG_OK) {
		c.reason = STAGE_BAR_WHY_CONFIG;
		return c;
	}
	c.address = f->address;
	for (i = 0; i < STAGE_BAR_INDEXES; i++) {
		if (f->index[i] == STAGE_BAR_MATCH_CONFLICT) {
			c.reason = STAGE_BAR_WHY_CONFLICT;
			return c;
		}
		if (f->index[i] == STAGE_BAR_MATCH_EXACT) {
			exact++;
			c.index = i;
		}
	}
	if (exact > 1u) {
		c.reason = STAGE_BAR_WHY_AMBIGUOUS;
		return c;
	}
	if (exact == 1u) {
		c.path = STAGE_BAR_PATH_PCI_IO;
		c.reason = STAGE_BAR_WHY_INDEX;
		return c;
	}
	/* No descriptor of the PCI I/O protocol names the BAR. The root bridge reaches the address without one. Its
	 * own windows choose it when there are several, and a complete list without the address refuses it. */
	if (f->bridges_holding == 1u ||
	    (f->bridges == 1u && f->bridges_holding == 0u && f->bridges_described == 0u)) {
		c.path = STAGE_BAR_PATH_BRIDGE;
		c.reason = STAGE_BAR_WHY_BRIDGE;
		c.bridge = f->bridge;
		return c;
	}
	c.reason = STAGE_BAR_WHY_NO_BRIDGE;
	return c;
}

const char *stage_bar_reason_text(enum stage_bar_reason reason)
{
	static const char *const text[STAGE_BAR_WHY_COUNT] = {
		"one PCI I/O BarIndex describes BAR5",
		"no PCI I/O BarIndex describes BAR5, one root bridge serves its address",
		"the configuration header gives no register BAR",
		"a descriptor at the address of BAR5 has another length",
		"more than one PCI I/O BarIndex describes BAR5",
		"no PCI I/O BarIndex describes BAR5 and no single root bridge serves its address",
	};
	return (unsigned)reason < STAGE_BAR_WHY_COUNT ? text[reason] : "unknown reason";
}

/* ---- the positive control ------------------------------------------------------------------------ */

unsigned stage_bar_control_offset(void)
{
	/* SOC15_REG_OFFSET(GC, 0, mmGB_ADDR_CONFIG) in bytes: the dword offset of the register plus the segment base
	 * of instance 0, from the IP base table that amdgpu reads for this part. */
	return 4u * (GC_BASE.instance[0].segment[mmGB_ADDR_CONFIG_BASE_IDX] + mmGB_ADDR_CONFIG);
}

enum stage_bar_control stage_bar_control_read(int (*read32)(void *context, unsigned offset, unsigned *value),
					      void *context, unsigned *value)
{
	*value = 0;
	if (read32(context, stage_bar_control_offset(), value)) return STAGE_BAR_CONTROL_READ_FAILED;
	return *value == STAGE_BAR_CONTROL_VALUE ? STAGE_BAR_CONTROL_OK : STAGE_BAR_CONTROL_MISMATCH;
}

const char *stage_bar_control_text(enum stage_bar_control result)
{
	static const char *const text[STAGE_BAR_CONTROL_COUNT] = {
		"the control register has its known value",
		"the control register did not read",
		"the control register does not have its known value",
	};
	return (unsigned)result < STAGE_BAR_CONTROL_COUNT ? text[result] : "unknown result";
}
