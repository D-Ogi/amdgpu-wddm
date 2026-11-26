/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * IOMMU module for the BC-250 stage. The stock BIOS leaves the AMD IOMMU (PCI 1022:13E1
 * at SMN 0x13F00000) with no MMIO base and no IVRS ACPI table. This module sets the base,
 * installs a minimal IVRS, and lets Windows handle the rest.
 *
 * Lab quality: one IVHD block with an all-devices range (no per-device isolation), no
 * command buffer or event log setup (Windows builds its own), passthrough until the OS
 * enables translation.
 *
 * Nie szukaj dzisiaj tego, co znajdziesz jutro - don't look for today what you'll find tomorrow.
 * (But look for the IOMMU today, because the BIOS never will.)
 */
#include <stddef.h>
#include <string.h>
#include "efi.h"
#include "stage.h"
#include "iommu_module.h"
#include "stage_reg.h"

static const EFI_GUID acpi_table_guid = EFI_ACPI_TABLE_PROTOCOL_GUID;

/* --- SMN access through BAR5 PCIE_INDEX2/DATA2 ------------------------------------------------ */

/* BAR5 through the register BAR of the stage (stage_reg.h). On unit A the firmware serves it as PCI I/O BarIndex 3;
 * BarIndex 5 answers EFI_UNSUPPORTED because edk2 counts a 64-bit BAR as one index. */
static int base_set;

static int smn_read32(struct stage *stage, unsigned addr, unsigned *value)
{
	if (stage_reg_write32(stage, IOMMU_BAR5_INDEX2, addr)) return -1;
	return stage_reg_read32(stage, IOMMU_BAR5_DATA2, value);
}

static int smn_write32(struct stage *stage, unsigned addr, unsigned value)
{
	if (stage_reg_write32(stage, IOMMU_BAR5_INDEX2, addr)) return -1;
	return stage_reg_write32(stage, IOMMU_BAR5_DATA2, value);
}

/* 1 when the ACPI tables already hold an IVRS. */
static int ivrs_present(struct stage *stage)
{
	struct efl_acpi acpi;
	struct efl_acpi_table tables[EFL_ACPI_MAX_TABLES];
	unsigned count = 0, dropped = 0, i;
	stage_acpi(stage, &acpi);
	if (efl_acpi_tables(&acpi, tables, EFL_ACPI_MAX_TABLES, &count, &dropped) != EFL_ACPI_OK) return 0;
	for (i = 0; i < count; i++)
		if (memcmp(tables[i].signature, "IVRS", 4) == 0) return 1;
	return 0;
}

/* --- IVRS table construction --------------------------------------------------------------------- */

/* AMD IOMMU spec, IVRS table (section 5.2). Minimal: one IVHD type 0x10 with an all-devices range. */

#pragma pack(push, 1)
struct acpi_hdr {
	char     sig[4];
	UINT32   length;
	UINT8    revision;
	UINT8    checksum;
	char     oem_id[6];
	char     oem_table_id[8];
	UINT32   oem_rev;
	char     creator_id[4];
	UINT32   creator_rev;
};

struct ivrs_hdr {
	struct acpi_hdr acpi;
	UINT32   iv_info;
	UINT8    reserved[8];
};

struct ivhd_10 {
	UINT8    type;        /* 0x10 */
	UINT8    flags;
	UINT16   length;
	UINT16   device_id;   /* BDF of the IOMMU function */
	UINT16   cap_offset;
	UINT64   iommu_base;
	UINT16   pci_segment;
	UINT16   iommu_info;
	UINT32   iommu_feature;
};

struct ivhd_all_entry {
	UINT8  type;       /* 0x01 = all devices */
	UINT16 device_id;  /* 0x0000 */
	UINT8  dte;        /* 0x00 = no special bits */
};
#pragma pack(pop)

#define IVRS_TOTAL (sizeof(struct ivrs_hdr) + sizeof(struct ivhd_10) + sizeof(struct ivhd_all_entry))

static UINT8 acpi_checksum(const void *data, unsigned len)
{
	const UINT8 *p = (const UINT8 *)data;
	UINT8 s = 0;
	unsigned i;
	for (i = 0; i < len; i++) s += p[i];
	return (UINT8)(0u - s);
}

static int build_ivrs(unsigned char *buf, unsigned capacity)
{
	struct ivrs_hdr *ivrs;
	struct ivhd_10 *ivhd;
	struct ivhd_all_entry *entry;

	if (capacity < IVRS_TOTAL) return 0;
	memset(buf, 0, IVRS_TOTAL);

	ivrs = (struct ivrs_hdr *)buf;
	memcpy(ivrs->acpi.sig, "IVRS", 4);
	ivrs->acpi.length   = (UINT32)IVRS_TOTAL;
	ivrs->acpi.revision = 2;
	memcpy(ivrs->acpi.oem_id,       "BC250 ", 6);
	memcpy(ivrs->acpi.oem_table_id, "IOMMU   ", 8);
	ivrs->acpi.oem_rev    = 1;
	memcpy(ivrs->acpi.creator_id, "BC25", 4);
	ivrs->acpi.creator_rev = 1;
	ivrs->iv_info = 0x00203040u;

	ivhd = (struct ivhd_10 *)(buf + sizeof(struct ivrs_hdr));
	ivhd->type        = 0x10;
	ivhd->flags       = 0;
	ivhd->length      = (UINT16)(sizeof(struct ivhd_10) + sizeof(struct ivhd_all_entry));
	ivhd->device_id   = IOMMU_BDF;
	ivhd->cap_offset  = (UINT16)IOMMU_CAP_OFFSET;
	ivhd->iommu_base  = (UINT64)IOMMU_MMIO_BASE;
	ivhd->pci_segment = 0;

	entry = (struct ivhd_all_entry *)((unsigned char *)ivhd + sizeof(struct ivhd_10));
	entry->type      = 0x01;
	entry->device_id = 0x0000;
	entry->dte       = 0x00;

	ivrs->acpi.checksum = acpi_checksum(buf, (unsigned)IVRS_TOTAL);
	return (int)IVRS_TOTAL;
}

/* --- module entry points ------------------------------------------------------------------------- */

static void iommu_run(struct stage *stage)
{
	unsigned vid_did = 0, base_lo = 0;
	EFI_ACPI_TABLE_PROTOCOL *installer = NULL;
	unsigned char ivrs_buf[256];
	int ivrs_len;
	UINTN key = 0;

	if (!stage_board_bc250(stage)) {
		stage_say(stage, IOMMU_NAME, "not a BC-250");
		return;
	}

	if (stage->gpu == NULL) {
		stage_say(stage, IOMMU_NAME, "no GPU PciIo handle");
		return;
	}

	/* The core unlock identifies the BAR first when it runs; with the unlock off, the IOMMU does it. */
	if (!stage->reg_identified && !stage_reg_identify(stage, IOMMU_NAME)) {
		stage_say(stage, IOMMU_NAME, "the register BAR is not identified, no IOMMU setup");
		return;
	}

	if (ivrs_present(stage)) {
		stage_say(stage, IOMMU_NAME, "the firmware has an IVRS, no change");
		return;
	}

	/* verify hardware presence via SMN */
	if (smn_read32(stage, IOMMU_SMN_BASE, &vid_did) || vid_did != IOMMU_VID_DID) {
		stage_sayf(stage, IOMMU_NAME, "IOMMU not found at SMN 0x%08x (read 0x%08x)",
			   IOMMU_SMN_BASE, vid_did);
		return;
	}

	/* check current state */
	if (smn_read32(stage, IOMMU_SMN_CAP_LO, &base_lo)) {
		stage_say(stage, IOMMU_NAME, "MMIO base read failed");
		return;
	}
	if (base_lo & 1u) {
		stage_sayf(stage, IOMMU_NAME, "IOMMU already configured (base_lo 0x%08x)", base_lo);
		return;
	}

	/* set MMIO base */
	base_set = 1;
	if (smn_write32(stage, IOMMU_SMN_CAP_LO, IOMMU_MMIO_BASE | 1u) ||
	    smn_write32(stage, IOMMU_SMN_CAP_HI, 0)) {
		stage_say(stage, IOMMU_NAME, "MMIO base write failed");
		return;
	}

	/* verify */
	base_lo = 0;
	smn_read32(stage, IOMMU_SMN_CAP_LO, &base_lo);
	if (base_lo != (IOMMU_MMIO_BASE | 1u)) {
		stage_sayf(stage, IOMMU_NAME, "MMIO base readback 0x%08x, expected 0x%08x",
			   base_lo, IOMMU_MMIO_BASE | 1u);
		return;
	}

	stage_sayf(stage, IOMMU_NAME, "MMIO base set to 0x%08x", IOMMU_MMIO_BASE);

	/* build and install IVRS */
	ivrs_len = build_ivrs(ivrs_buf, sizeof(ivrs_buf));
	if (ivrs_len <= 0) {
		stage_say(stage, IOMMU_NAME, "IVRS build failed");
		return;
	}

	if (EFI_ERROR(stage->bs->LocateProtocol((EFI_GUID *)&acpi_table_guid, NULL, (VOID **)&installer)) ||
	    installer == NULL) {
		stage_say(stage, IOMMU_NAME, "no ACPI table protocol");
		return;
	}

	if (EFI_ERROR(installer->InstallAcpiTable(installer, ivrs_buf, (UINTN)ivrs_len, &key))) {
		stage_say(stage, IOMMU_NAME, "IVRS install failed");
		return;
	}

	stage_sayf(stage, IOMMU_NAME, "IVRS installed (%u bytes, key %llu)", (unsigned)ivrs_len,
		   (unsigned long long)key);
}

/* Reads no register: an SMN read needs a write of PCIE_INDEX2, and a probe changes nothing. */
static void iommu_probe(struct stage *stage)
{
	EFI_ACPI_TABLE_PROTOCOL *installer = NULL;
	stage_sayf(stage, IOMMU_NAME, "probe: IVRS %s, ACPI table protocol %s",
		   ivrs_present(stage) ? "present" : "absent",
		   EFI_ERROR(stage->bs->LocateProtocol((EFI_GUID *)&acpi_table_guid, NULL, (VOID **)&installer)) ||
		   installer == NULL ? "absent" : "present");
}

static void iommu_unwind(struct stage *stage)
{
	/* Clear the MMIO base that this run set, so that the next boot without the stage has no leftover. */
	if (!base_set) return;
	smn_write32(stage, IOMMU_SMN_CAP_LO, 0);
	smn_write32(stage, IOMMU_SMN_CAP_HI, 0);
	base_set = 0;
	stage_say(stage, IOMMU_NAME, "MMIO base cleared");
}

const struct stage_module iommu_module = {
	.name       = IOMMU_NAME,
	.off_switch = "iommu",
	.default_on = 1,
	.run        = iommu_run,
	.probe      = iommu_probe,
	.gate       = NULL,
	.loaded     = NULL,
	.unwind     = iommu_unwind,
};
