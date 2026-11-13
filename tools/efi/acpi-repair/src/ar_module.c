/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The ACPI repair as a module of the BC-250 stage (docs/design/efi-resident-stage.md, "The ACPI repair"). This file
 * is the UEFI binding: it reads the firmware's tables, asks ar_plan.c what to do, and installs the result through
 * the firmware's EFI_ACPI_TABLE_PROTOCOL. The firmware keeps its own copy of an installed table, so no code and no
 * memory of this module stays after its run.
 *
 * The module writes to RAM only. It calls no variable service and writes no file, no CMOS and no flash.
 */
#include <stddef.h>
#include <string.h>
#include "efi.h"
#include "efl_acpi.h"
#include "stage.h"
#include "ar_plan.h"

#define AR_NAME			"acpi"
#define AR_TABLE_MAX_BYTES	(16u * 1024u)
#define AR_SDT_MAX_INDEX	256u
#define SDT_HEADER_BYTES	36u
#define SDT_OEM_TABLE_ID	16u
#define MADT_LOCAL_APIC		0u
#define MADT_ENABLED		1u

static const EFI_GUID acpi_table_guid = EFI_ACPI_TABLE_PROTOCOL_GUID;
static const EFI_GUID acpi_sdt_guid = EFI_ACPI_SDT_PROTOCOL_GUID;

static struct efl_acpi_table tables[EFL_ACPI_MAX_TABLES];
static struct efl_acpi_madt_cpu madt[EFL_ACPI_MAX_MADT_CPUS];
static struct ar_input input;
static struct ar_plan plan;

/* Fills the input from the firmware's tables. 0 when the root tables do not read. input.complete says that every
 * table of the root table mapped with a right checksum and that every definition block is in the input. */
static int collect(struct stage *stage)
{
	struct efl_acpi acpi;
	unsigned count = 0, dropped = 0, cpus = 0, i, local = 0, dsdts = 0, unread = 0;
	memset(&input, 0, sizeof(input));
	stage_acpi(stage, &acpi);
	if (efl_acpi_tables(&acpi, tables, EFL_ACPI_MAX_TABLES, &count, &dropped) != EFL_ACPI_OK) return 0;
	for (i = 0; i < count; i++) {
		int dsdt = !memcmp(tables[i].signature, "DSDT", 4);
		/* A PSDT is a definition block as an SSDT is (ACPI 6.5, the table signatures). */
		int ssdt = !memcmp(tables[i].signature, "SSDT", 4) || !memcmp(tables[i].signature, "PSDT", 4);
		if (tables[i].bytes == NULL) {
			/* A wrong checksum: the table is not used. Its signature is then not trusted either, so it
			 * counts as a definition block that did not read. */
			unread++;
			continue;
		}
		if (dsdt) {
			input.dsdt.bytes = tables[i].bytes;
			input.dsdt.length = tables[i].length;
			dsdts++;
		} else if (ssdt) {
			if (input.ssdts < AR_MAX_SSDTS) {
				input.ssdt[input.ssdts].bytes = tables[i].bytes;
				input.ssdt[input.ssdts].length = tables[i].length;
				input.ssdts++;
			} else
				unread++;
		}
	}
	input.complete = dropped == 0 && unread == 0 && dsdts == 1u;
	if (!input.complete)
		stage_sayf(stage, AR_NAME, "the table set is not whole: %u tables did not map or fit, %u did not read, %u DSDT",
			   dropped, unread, dsdts);
	if (efl_acpi_madt_list(&acpi, madt, EFL_ACPI_MAX_MADT_CPUS, &count, &dropped) != EFL_ACPI_OK) count = 0;
	/* The Local APIC entries when the table has one, else the x2APIC entries: the rule of the processor count. */
	for (i = 0; i < count; i++)
		if (madt[i].type == MADT_LOCAL_APIC) local = 1;
	for (i = 0; i < count; i++) {
		if ((madt[i].type == MADT_LOCAL_APIC) != (local != 0) || !(madt[i].flags & MADT_ENABLED)) continue;
		if (cpus < AR_MAX_CPUS) {
			input.cpu[cpus].uid = madt[i].uid;
			input.cpu[cpus].apic_id = madt[i].apic_id;
		}
		cpus++;
	}
	input.cpus = dropped ? 0 : cpus;	/* a list that did not fit is not a list to act on */
	return 1;
}

/* Removes the stock CPU table from the root tables. The key of a table comes from the SDT protocol. */
static void remove_stock(struct stage *stage, EFI_ACPI_TABLE_PROTOCOL *installer, const unsigned char *stock_header)
{
	EFI_ACPI_SDT_PROTOCOL *sdt = NULL;
	UINTN index;
	if (EFI_ERROR(stage->bs->LocateProtocol(&acpi_sdt_guid, NULL, (VOID **)&sdt)) || sdt == NULL) {
		stage_say(stage, AR_NAME, "no ACPI SDT protocol: the stock CPU table stays, and it still fails to load");
		return;
	}
	for (index = 0; index < AR_SDT_MAX_INDEX; index++) {
		VOID *table = NULL;
		UINT32 version = 0;
		UINTN key = 0;
		EFI_STATUS status = sdt->GetAcpiTable(index, &table, &version, &key);
		if (EFI_ERROR(status)) break;
		if (table == NULL || memcmp(table, stock_header, SDT_HEADER_BYTES) != 0) continue;
		status = installer->UninstallAcpiTable(installer, key);
		if (EFI_ERROR(status))
			stage_sayf(stage, AR_NAME, "the stock CPU table was not removed, status 0x%llx: it stays",
				   (unsigned long long)status);
		else
			stage_say(stage, AR_NAME, "the stock CPU table is removed from the root tables");
		return;
	}
	stage_say(stage, AR_NAME, "the SDT protocol does not list the stock CPU table: it stays");
}

/* 1 when the root tables now list a sound table with this OEM table ID. */
static int listed(struct stage *stage, const char *oem_table_id)
{
	struct efl_acpi acpi;
	unsigned count = 0, dropped = 0, i;
	stage_acpi(stage, &acpi);
	if (efl_acpi_tables(&acpi, tables, EFL_ACPI_MAX_TABLES, &count, &dropped) != EFL_ACPI_OK) return 0;
	for (i = 0; i < count; i++)
		if (tables[i].bytes != NULL && !strcmp(tables[i].oem_table_id, oem_table_id)) return 1;
	return 0;
}

static int install(struct stage *stage, EFI_ACPI_TABLE_PROTOCOL *installer, unsigned char *table, size_t bytes,
		   const char *what, const char *oem_table_id)
{
	UINTN key = 0;
	EFI_STATUS status = installer->InstallAcpiTable(installer, table, bytes, &key);
	if (EFI_ERROR(status)) {
		stage_sayf(stage, AR_NAME, "%s: the firmware refused the table, status 0x%llx", what, (unsigned long long)status);
		return 0;
	}
	if (!listed(stage, oem_table_id)) {
		stage_sayf(stage, AR_NAME, "%s: installed, but the root tables do not list it", what);
		return 0;
	}
	stage_sayf(stage, AR_NAME, "%s: table of %u bytes installed", what, (unsigned)bytes);
	return 1;
}

static void ar_module_run(struct stage *stage)
{
	EFI_ACPI_TABLE_PROTOCOL *installer = NULL;
	unsigned char *buffer;
	unsigned char stock_header[SDT_HEADER_BYTES];
	size_t bytes;
	int want_idle = stage_option_flag(&stage->options, "acpi.idle", 1);
	int want_stubs = stage_option_flag(&stage->options, "acpi.stubs", 1);
	int apply = stage_option_flag(&stage->options, "acpi.apply", 1);

	if (!stage_board_bc250(stage)) {
		stage_say(stage, AR_NAME, "not a BC-250: nothing done");
		return;
	}
	if (EFI_ERROR(stage->bs->LocateProtocol(&acpi_table_guid, NULL, (VOID **)&installer)) || installer == NULL) {
		stage_say(stage, AR_NAME, "the firmware has no ACPI table protocol: nothing done");
		return;
	}
	if (!collect(stage) || input.dsdt.bytes == NULL) {
		stage_say(stage, AR_NAME, "the ACPI root tables or the DSDT do not read: nothing done");
		return;
	}
	input.want_idle = want_idle;
	input.want_stubs = want_stubs;
	ar_plan_make(&input, &plan);
	stage_sayf(stage, AR_NAME, "idle states: %s", ar_idle_text(plan.idle));
	stage_sayf(stage, AR_NAME, "method stubs: %s", ar_stubs_text(plan.stubs));
	if (plan.idle != AR_IDLE_REPAIR && plan.stubs != AR_STUBS_REPAIR) return;
	if (!apply) stage_say(stage, AR_NAME, "acpi.apply=0: the tables are built and checked, and none is installed or removed");

	buffer = stage_alloc(stage, AR_TABLE_MAX_BYTES);
	if (buffer == NULL) {
		stage_say(stage, AR_NAME, "no memory for a table: nothing done");
		return;
	}
	if (plan.idle == AR_IDLE_REPAIR) {
		stage_sayf(stage, AR_NAME, "stock CPU table: %u scopes, %s", plan.stock_scopes,
			   plan.stock_exact ? "the digest of the table of unit A" : "stock content, another digest");
		stage_sayf(stage, AR_NAME, "processors: %u copied, %u derived, %u left out", plan.copied, plan.derived,
			   plan.left_out);
		/* The stock table is in firmware memory that an install can move and an uninstall can free, so a copy
		 * of its header is kept here. The header holds the length and the checksum, so it names one table. */
		memcpy(stock_header, input.ssdt[plan.stock_ssdt].bytes, SDT_HEADER_BYTES);
		bytes = ar_build_idle(&plan, buffer, AR_TABLE_MAX_BYTES);
		if (bytes == 0 || !ar_check_idle(&plan, buffer, bytes))
			stage_say(stage, AR_NAME, "idle states: the built table failed its own check: not installed");
		else if (!apply)
			stage_sayf(stage, AR_NAME, "idle states: a table of %u bytes passed its check", (unsigned)bytes);
		else if (install(stage, installer, buffer, bytes, "idle states", AR_IDLE_TABLE_ID))
			remove_stock(stage, installer, stock_header);
	}
	if (plan.stubs == AR_STUBS_REPAIR) {
		unsigned s;
		for (s = 0; s < AR_STUBS; s++)
			if (plan.stub_mask & (1u << s))
				stage_sayf(stage, AR_NAME, "method %s is called and not defined: an empty method", ar_stub_name(s));
		bytes = ar_build_stubs(&plan, buffer, AR_TABLE_MAX_BYTES);
		if (bytes == 0 || !ar_check_stubs(&plan, buffer, bytes))
			stage_say(stage, AR_NAME, "method stubs: the built table failed its own check: not installed");
		else if (!apply)
			stage_sayf(stage, AR_NAME, "method stubs: a table of %u bytes passed its check", (unsigned)bytes);
		else
			(void)install(stage, installer, buffer, bytes, "method stubs", AR_STUB_TABLE_ID);
	}
	stage_free(stage, buffer);
}

/* probe=1 with acpi=0: the two protocols that the repair needs, read and not changed. */
static void ar_module_probe(struct stage *stage)
{
	VOID *found = NULL;
	stage_sayf(stage, AR_NAME, "probe: ACPI table protocol %s",
		   EFI_ERROR(stage->bs->LocateProtocol(&acpi_table_guid, NULL, &found)) || found == NULL ? "absent" : "present");
	found = NULL;
	stage_sayf(stage, AR_NAME, "probe: ACPI SDT protocol %s",
		   EFI_ERROR(stage->bs->LocateProtocol(&acpi_sdt_guid, NULL, &found)) || found == NULL ? "absent" : "present");
}

const struct stage_module ar_module = { AR_NAME, "acpi", ar_module_run, ar_module_probe, NULL, NULL, NULL };
