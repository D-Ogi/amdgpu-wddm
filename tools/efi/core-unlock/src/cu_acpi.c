/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The FADT reset register and the MADT processor count (cu_acpi.h). Offsets are those of the ACPI Specification
 * 6.5 tables, each named once below. Every table is checked against its own length and checksum before a field of
 * it is read. */
#include <stddef.h>
#include <string.h>
#include "cu_acpi.h"

/* RSDP (section 5.2.5.3) */
#define RSDP_V1_BYTES		20u
#define RSDP_V2_BYTES		36u
#define RSDP_REVISION		15u
#define RSDP_RSDT		16u
#define RSDP_LENGTH		20u
#define RSDP_XSDT		24u
/* System description table header (section 5.2.6) */
#define SDT_HEADER_BYTES	36u
#define SDT_LENGTH		4u
#define SDT_MAX_BYTES		(1024u * 1024u)
/* FADT (section 5.2.9) */
#define FADT_FLAGS		112u
#define FADT_FLAG_RESET_REG_SUP	(1u << 10)
#define FADT_RESET_REG		116u	/* a Generic Address Structure: space, bit width, bit offset, access, address */
#define FADT_RESET_VALUE	128u
/* MADT (section 5.2.12) */
#define MADT_ENTRIES		44u
#define MADT_LOCAL_APIC		0u
#define MADT_LOCAL_APIC_FLAGS	4u
#define MADT_X2APIC		9u
#define MADT_X2APIC_FLAGS	8u
#define MADT_ENABLED		1u

static unsigned le32(const unsigned char *p)
{
	return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static unsigned long long le64(const unsigned char *p)
{
	return (unsigned long long)le32(p) | ((unsigned long long)le32(p + 4) << 32);
}

static int checksum_ok(const unsigned char *p, unsigned long bytes)
{
	unsigned char sum = 0;
	while (bytes--) sum = (unsigned char)(sum + *p++);
	return sum == 0;
}

/* The whole table at an address when its signature matches and its checksum is right, else NULL. */
static const unsigned char *table_at(const struct cu_acpi *acpi, unsigned long long address, const char *signature,
				     unsigned *length)
{
	const unsigned char *h;
	unsigned bytes;
	if (address == 0) return NULL;
	h = acpi->map(acpi->context, address, SDT_HEADER_BYTES);
	if (h == NULL || memcmp(h, signature, 4) != 0) return NULL;
	bytes = le32(h + SDT_LENGTH);
	if (bytes < SDT_HEADER_BYTES || bytes > SDT_MAX_BYTES) return NULL;
	h = acpi->map(acpi->context, address, bytes);
	if (h == NULL || !checksum_ok(h, bytes)) return NULL;
	*length = bytes;
	return h;
}

static enum cu_acpi_result find_table(const struct cu_acpi *acpi, const char *signature,
				      const unsigned char **table, unsigned *length)
{
	const unsigned char *rsdp, *root = NULL;
	unsigned root_length = 0, entry_bytes, i;
	*table = NULL;
	*length = 0;
	rsdp = acpi->map(acpi->context, acpi->rsdp, RSDP_V1_BYTES);
	if (rsdp == NULL || memcmp(rsdp, "RSD PTR ", 8) != 0 || !checksum_ok(rsdp, RSDP_V1_BYTES)) return CU_ACPI_NO_RSDP;
	/* The XSDT when the RSDP is of revision 2 or later and names one, else the RSDT. */
	if (rsdp[RSDP_REVISION] >= 2u) {
		const unsigned char *v2 = acpi->map(acpi->context, acpi->rsdp, RSDP_V2_BYTES);
		if (v2 != NULL && le32(v2 + RSDP_LENGTH) >= RSDP_V2_BYTES &&
		    checksum_ok(v2, RSDP_V2_BYTES) && le64(v2 + RSDP_XSDT) != 0)
			root = table_at(acpi, le64(v2 + RSDP_XSDT), "XSDT", &root_length);
	}
	entry_bytes = 8u;
	if (root == NULL) {
		root = table_at(acpi, le32(rsdp + RSDP_RSDT), "RSDT", &root_length);
		entry_bytes = 4u;
	}
	if (root == NULL) return CU_ACPI_NO_RSDP;
	for (i = SDT_HEADER_BYTES; i + entry_bytes <= root_length; i += entry_bytes) {
		unsigned long long address = entry_bytes == 8u ? le64(root + i) : le32(root + i);
		const unsigned char *t = table_at(acpi, address, signature, length);
		if (t != NULL) {
			*table = t;
			return CU_ACPI_OK;
		}
	}
	return CU_ACPI_NO_TABLE;
}

enum cu_acpi_result cu_acpi_reset_register(const struct cu_acpi *acpi, struct cu_reset_register *out)
{
	const unsigned char *fadt, *gas;
	unsigned length;
	enum cu_acpi_result result;
	memset(out, 0, sizeof(*out));
	result = find_table(acpi, "FACP", &fadt, &length);
	if (result != CU_ACPI_OK) return result;
	if (length < FADT_RESET_VALUE + 1u) return CU_ACPI_SHORT;
	if (!(le32(fadt + FADT_FLAGS) & FADT_FLAG_RESET_REG_SUP)) return CU_ACPI_NOT_SUPPORTED;
	gas = fadt + FADT_RESET_REG;
	/* The specification's reset register is 8 bits wide at bit offset 0. Access size 0 (legacy) or 1 (byte). */
	if (gas[1] != 8u || gas[2] != 0u || gas[3] > 1u || le64(gas + 4) == 0) return CU_ACPI_BAD_REGISTER;
	if (gas[0] != CU_ACPI_SPACE_MEMORY && gas[0] != CU_ACPI_SPACE_IO && gas[0] != CU_ACPI_SPACE_PCI)
		return CU_ACPI_BAD_REGISTER;
	out->space = gas[0];
	out->address = le64(gas + 4);
	out->value = fadt[FADT_RESET_VALUE];
	return CU_ACPI_OK;
}

enum cu_acpi_result cu_acpi_madt_processors(const struct cu_acpi *acpi, unsigned *processors)
{
	const unsigned char *madt;
	unsigned length, at, local = 0, x2 = 0;
	enum cu_acpi_result result;
	*processors = 0;
	result = find_table(acpi, "APIC", &madt, &length);
	if (result != CU_ACPI_OK) return result;
	for (at = MADT_ENTRIES; at + 2u <= length;) {
		unsigned type = madt[at], bytes = madt[at + 1u];
		if (bytes < 2u || at + bytes > length) break;
		if (type == MADT_LOCAL_APIC && bytes >= MADT_LOCAL_APIC_FLAGS + 4u &&
		    (le32(madt + at + MADT_LOCAL_APIC_FLAGS) & MADT_ENABLED))
			local++;
		else if (type == MADT_X2APIC && bytes >= MADT_X2APIC_FLAGS + 4u &&
			 (le32(madt + at + MADT_X2APIC_FLAGS) & MADT_ENABLED))
			x2++;
		at += bytes;
	}
	*processors = local ? local : x2;
	return CU_ACPI_OK;
}
