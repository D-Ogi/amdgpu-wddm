/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The SMBIOS entry points and the RSMB form (cu_smbios.h). */
#include <stddef.h>
#include <string.h>
#include "cu_smbios.h"

/* 32-bit entry point (section 5.2.1) */
#define EP2_BYTES_MIN		0x1Fu
#define EP2_LENGTH		5u
#define EP2_MAJOR		6u
#define EP2_MINOR		7u
#define EP2_TABLE_LENGTH	0x16u
#define EP2_TABLE_ADDRESS	0x18u
/* 64-bit entry point (section 5.2.2) */
#define EP3_BYTES_MIN		0x18u
#define EP3_LENGTH		6u
#define EP3_MAJOR		7u
#define EP3_MINOR		8u
#define EP3_TABLE_MAX		0x0Cu
#define EP3_TABLE_ADDRESS	0x10u
/* Structure header (section 6.1.2): type, formatted length, handle */
#define SMBIOS_HEADER_BYTES	4u
#define SMBIOS_END_OF_TABLE	127u

static unsigned le16(const unsigned char *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static unsigned le32(const unsigned char *p) { return le16(p) | (le16(p + 2) << 16); }

static int checksum_ok(const unsigned char *p, unsigned bytes)
{
	unsigned char sum = 0;
	while (bytes--) sum = (unsigned char)(sum + *p++);
	return sum == 0;
}

int cu_smbios_entry(cu_map_fn map, void *context, unsigned long long entry, int v3, struct cu_smbios_table *out)
{
	const unsigned char *e;
	unsigned bytes;
	memset(out, 0, sizeof(*out));
	if (entry == 0) return 0;
	if (v3) {
		e = map(context, entry, EP3_BYTES_MIN);
		if (e == NULL || memcmp(e, "_SM3_", 5) != 0) return 0;
		bytes = e[EP3_LENGTH];
		if (bytes < EP3_BYTES_MIN || (e = map(context, entry, bytes)) == NULL || !checksum_ok(e, bytes)) return 0;
		out->address = (unsigned long long)le32(e + EP3_TABLE_ADDRESS) |
			       ((unsigned long long)le32(e + EP3_TABLE_ADDRESS + 4) << 32);
		out->max_bytes = le32(e + EP3_TABLE_MAX);
		out->major = e[EP3_MAJOR];
		out->minor = e[EP3_MINOR];
	} else {
		e = map(context, entry, EP2_BYTES_MIN);
		if (e == NULL || memcmp(e, "_SM_", 4) != 0) return 0;
		bytes = e[EP2_LENGTH];
		if (bytes < EP2_BYTES_MIN || (e = map(context, entry, bytes)) == NULL || !checksum_ok(e, bytes)) return 0;
		out->address = le32(e + EP2_TABLE_ADDRESS);
		out->max_bytes = le16(e + EP2_TABLE_LENGTH);
		out->major = e[EP2_MAJOR];
		out->minor = e[EP2_MINOR];
	}
	return out->address != 0 && out->max_bytes != 0;
}

size_t cu_smbios_extent(const unsigned char *table, size_t max_bytes)
{
	size_t at = 0;
	if (table == NULL) return 0;
	while (at + SMBIOS_HEADER_BYTES <= max_bytes) {
		size_t end = at + table[at + 1];
		unsigned type = table[at];
		if (table[at + 1] < SMBIOS_HEADER_BYTES || end > max_bytes) return 0;
		/* The string set ends with two zero bytes, also when the structure has no string. */
		while (end + 1 < max_bytes && (table[end] || table[end + 1])) end++;
		if (end + 1 >= max_bytes) return 0;
		at = end + 2;
		if (type == SMBIOS_END_OF_TABLE) return at;
	}
	return 0;
}

size_t cu_smbios_rsmb(const unsigned char *table, size_t extent, unsigned major, unsigned minor,
		      unsigned char *out, size_t capacity)
{
	if (table == NULL || out == NULL || extent == 0 || extent > 0xFFFFFFFFu ||
	    capacity < CU_RSMB_HEADER_BYTES + extent)
		return 0;
	out[0] = 0;			/* Used20CallingMethod */
	out[1] = (unsigned char)major;
	out[2] = (unsigned char)minor;
	out[3] = 0;			/* DmiRevision */
	out[4] = (unsigned char)extent;
	out[5] = (unsigned char)(extent >> 8);
	out[6] = (unsigned char)(extent >> 16);
	out[7] = (unsigned char)(extent >> 24);
	memcpy(out + CU_RSMB_HEADER_BYTES, table, extent);
	return CU_RSMB_HEADER_BYTES + extent;
}
