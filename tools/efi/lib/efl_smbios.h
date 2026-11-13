/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The SMBIOS table of the firmware, in the form Windows hands the driver ('RSMB' of GetSystemFirmwareTable: an
 * eight-byte header with the version and the length, then the structures). The driver's own identity check,
 * BoardIdentitySelect in driver/kmd/board_identity.c, then reads it unchanged. Layouts: SMBIOS Reference
 * Specification 3.7, section 5.2 (the two entry points) and section 6.1 (the structure form).
 */
#ifndef EFL_SMBIOS_H
#define EFL_SMBIOS_H
#include <stddef.h>
#include "efl_acpi.h"

#define EFL_RSMB_HEADER_BYTES	8u

struct efl_smbios_table {
	unsigned long long address;
	unsigned long max_bytes;	/* the 2.1 entry point's exact length, or the 3.0 entry point's maximum */
	unsigned major, minor;
};

/* The table that an entry point names. v3 selects the 64-bit entry point ("_SM3_"), else the 32-bit one ("_SM_").
 * 0 for a wrong anchor, length or checksum. */
int efl_smbios_entry(efl_map_fn map, void *context, unsigned long long entry, int v3, struct efl_smbios_table *out);

/* The bytes from the first structure to the end of the end-of-table structure (type 127), or 0 when the table does
 * not end inside max_bytes with a well-formed type 127. */
size_t efl_smbios_extent(const unsigned char *table, size_t max_bytes);

/* The RSMB form into out: the header, then the extent bytes of the table. Returns the bytes written, 0 when out is
 * too small. */
size_t efl_smbios_rsmb(const unsigned char *table, size_t extent, unsigned major, unsigned minor,
		      unsigned char *out, size_t capacity);

#endif
