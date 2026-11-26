/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * Host tests of the read-only report: the AML name scan, the ACPI lists, the register guard and the text. The stock
 * tests read the ACPI tables of unit A that the repository stores in
 * evidence/linux/2026-09-21-E01-recon-binaries/acpi; build.ps1 passes that directory. Every other value here is
 * test data, not a fact about a board.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "efl_acpi.h"
#include "efl_aml.h"
#include "cu_msr.h"
#include "cu_report.h"
#include "efl_text.h"
#include "cu_options.h"

static unsigned checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---- a memory arena for tables ----------------------------------------------------------------------- */

#define ARENA_BASE 0x100000ull
#define ARENA_BYTES 0x10000u
static unsigned char arena[ARENA_BYTES];

static const unsigned char *arena_map(void *context, unsigned long long address, unsigned long bytes)
{
	(void)context;
	if (address < ARENA_BASE || address + bytes > ARENA_BASE + ARENA_BYTES) return NULL;
	return arena + (address - ARENA_BASE);
}

static void put32(unsigned char *p, unsigned v)
{
	p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
static void put64(unsigned char *p, unsigned long long v) { put32(p, (unsigned)v); put32(p + 4, (unsigned)(v >> 32)); }

static void fix_checksum(unsigned char *p, unsigned bytes, unsigned at)
{
	unsigned char sum = 0;
	unsigned i;
	p[at] = 0;
	for (i = 0; i < bytes; i++) sum = (unsigned char)(sum + p[i]);
	p[at] = (unsigned char)(0x100u - sum);
}

static unsigned char *load(const char *dir, const char *name, size_t *bytes)
{
	char path[1024];
	FILE *f;
	unsigned char *data;
	long n;
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	f = fopen(path, "rb");
	if (f == NULL) return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	data = malloc((size_t)n);
	if (data == NULL || fread(data, 1, (size_t)n, f) != (size_t)n) {
		fclose(f);
		free(data);
		return NULL;
	}
	fclose(f);
	*bytes = (size_t)n;
	return data;
}

/* ---- AML built by hand ----------------------------------------------------------------------------------- */

struct aml { unsigned char b[1024]; size_t n; };

static void emit(struct aml *a, const void *p, size_t n) { memcpy(a->b + a->n, p, n); a->n += n; }
static void emit1(struct aml *a, unsigned char c) { a->b[a->n++] = c; }

/* Opens a construct with a two-byte PkgLength; close_pkg() fills it in. Returns the PkgLength position. */
static size_t open_pkg(struct aml *a) { size_t at = a->n; a->n += 2; return at; }
static void close_pkg(struct aml *a, size_t at)
{
	size_t length = a->n - at;
	a->b[at] = (unsigned char)(0x40u | (length & 0xFu));
	a->b[at + 1] = (unsigned char)(length >> 4);
}

static void header(struct aml *a, const char *signature)
{
	memset(a->b, 0, 36);
	memcpy(a->b, signature, 4);
	a->b[8] = 2;
	memcpy(a->b + 10, "TEST  ", 6);
	memcpy(a->b + 16, "SYNTH   ", 8);
	a->n = 36;
}

static void finish(struct aml *a)
{
	put32(a->b + 4, (unsigned)a->n);
	fix_checksum(a->b, (unsigned)a->n, 9);
}

static void test_aml_synthetic(void)
{
	static struct efl_aml_scan s;
	struct aml a;
	size_t scope, device, method, processor, buffer, cpu1;
	header(&a, "SSDT");
	/* Scope (\_SB) { Device (CPU0) { Name (_HID, "ACPI0007") Name (_CST, Zero) Method (_PSS) { Name (_CPC, 0) } } } */
	emit1(&a, 0x10); scope = open_pkg(&a); emit(&a, "\\_SB_", 5);
	emit(&a, "\x5B\x82", 2); device = open_pkg(&a); emit(&a, "CPU0", 4);
	emit1(&a, 0x08); emit(&a, "_HID", 4); emit1(&a, 0x0D); emit(&a, "ACPI0007", 9);
	emit1(&a, 0x08); emit(&a, "_CST", 4); emit1(&a, 0x00);
	emit1(&a, 0x14); method = open_pkg(&a); emit(&a, "_PSS", 4); emit1(&a, 0x00);
	emit1(&a, 0x08); emit(&a, "_CPC", 4); emit1(&a, 0x00);	/* inside a method body: not a definition */
	close_pkg(&a, method);
	close_pkg(&a, device);
	/* Device (CPU1) { Name (_HID, "ACPI0007") }: a processor device without power names */
	emit(&a, "\x5B\x82", 2); cpu1 = open_pkg(&a); emit(&a, "CPU1", 4);
	emit1(&a, 0x08); emit(&a, "_HID", 4); emit1(&a, 0x0D); emit(&a, "ACPI0007", 9);
	close_pkg(&a, cpu1);
	close_pkg(&a, scope);
	/* Scope (_PR) { Processor (P000, 1, 0x810, 6) {} } */
	emit1(&a, 0x10); scope = open_pkg(&a); emit(&a, "_PR_", 4);
	emit(&a, "\x5B\x83", 2); processor = open_pkg(&a); emit(&a, "P000", 4);
	emit(&a, "\x01\x10\x08\x00\x00\x06", 6);
	close_pkg(&a, processor);
	close_pkg(&a, scope);
	/* Name (XBUF, Buffer () { "_PSS" }): bytes of a name inside a buffer do not count */
	emit1(&a, 0x08); emit(&a, "XBUF", 4);
	emit1(&a, 0x11); buffer = open_pkg(&a); emit(&a, "\x0A\x04", 2); emit(&a, "_PSS", 4);
	close_pkg(&a, buffer);
	/* Scope (\_PR.C000) { Name (_CSD, Zero) } after the processor: a dual name path */
	emit1(&a, 0x10); scope = open_pkg(&a); emit(&a, "\\\x2E_PR_C000", 10);
	emit1(&a, 0x08); emit(&a, "_CSD", 4); emit1(&a, 0x00);
	close_pkg(&a, scope);
	/* A thermal zone: ThermalZone (TZ01) { Method (_TMP) { Return (Zero) } } */
	emit(&a, "\x5B\x85", 2); scope = open_pkg(&a); emit(&a, "TZ01", 4);
	emit1(&a, 0x14); method = open_pkg(&a); emit(&a, "_TMP", 4); emit1(&a, 0x00); emit(&a, "\xA4\x00", 2);
	close_pkg(&a, method);
	close_pkg(&a, scope);
	finish(&a);

	CHECK(efl_aml_scan(a.b, a.n, &s) == 1);
	CHECK(s.processor_objects == 1u && s.thermal_zones == 1u && s.malformed == 0u && s.dropped == 0u);
	CHECK(s.objects == 4u);
	CHECK(!strcmp(s.object[0].path, "\\_SB_.CPU0") && s.object[0].kind == EFL_AML_DEVICE_CPU);
	CHECK(s.object[0].names == ((1u << EFL_AML_CST) | (1u << EFL_AML_PSS)));
	CHECK(!strcmp(s.object[1].path, "\\_SB_.CPU1") && s.object[1].kind == EFL_AML_DEVICE_CPU && s.object[1].names == 0);
	CHECK(!strcmp(s.object[2].path, "\\_PR_.P000") && s.object[2].kind == EFL_AML_PROCESSOR && s.object[2].names == 0);
	CHECK(!strcmp(s.object[3].path, "\\_PR_.C000") && s.object[3].kind == EFL_AML_SCOPE);
	CHECK(s.object[3].names == (1u << EFL_AML_CSD));
	CHECK(s.counts[EFL_AML_TMP] == 1u && s.counts[EFL_AML_FAN_EISAID] == 0u && s.counts[EFL_AML_FAN_STRING] == 0u);

	/* A scope whose length runs past the table is not opened, and the scan goes on. */
	header(&a, "DSDT");
	emit1(&a, 0x10); emit(&a, "\x7F", 1); emit(&a, "_SB_", 4);
	emit1(&a, 0x08); emit(&a, "_CST", 4); emit1(&a, 0x00);
	finish(&a);
	CHECK(efl_aml_scan(a.b, a.n, &s) == 1);
	CHECK(s.malformed == 1u && s.objects == 1u && !strcmp(s.object[0].path, "\\"));

	/* Not an AML table, or a length beyond the bytes. */
	header(&a, "APIC");
	finish(&a);
	CHECK(efl_aml_scan(a.b, a.n, &s) == 0);
	header(&a, "SSDT");
	finish(&a);
	CHECK(efl_aml_scan(a.b, a.n - 1u, &s) == 0);
	CHECK(efl_aml_scan(NULL, 0, &s) == 0);

	/* The EISA ID encoding: PNP0A03 is the well-known 0x030AD041 of a PCI host bridge. */
	CHECK(efl_aml_eisaid("PNP0A03") == 0x030AD041u);
	CHECK(efl_aml_eisaid("PNP0C0B") == 0x0B0CD041u);
	CHECK(efl_aml_eisaid("pnp0C0B") == 0u && efl_aml_eisaid("PNP0C0") == 0u && efl_aml_eisaid("PNP0C0BX") == 0u);
	CHECK(efl_aml_eisaid(NULL) == 0u);
}

/* ---- the stock tables of unit A --------------------------------------------------------------------------- */

#define RSDP_AT (ARENA_BASE + 0x0000u)
#define XSDT_AT (ARENA_BASE + 0x0100u)
#define FACP_AT (ARENA_BASE + 0x0200u)
#define APIC_AT (ARENA_BASE + 0x0400u)
#define SSDT1_AT (ARENA_BASE + 0x0600u)
#define SSDT2_AT (ARENA_BASE + 0x0E00u)
#define DSDT_AT (ARENA_BASE + 0x3000u)

static int place(const char *dir, const char *name, unsigned long long at, size_t max)
{
	size_t bytes = 0;
	unsigned char *data = load(dir, name, &bytes);
	if (data == NULL || bytes > max) {
		printf("cannot load %s/%s\n", dir, name);
		free(data);
		return 0;
	}
	memcpy(arena + (at - ARENA_BASE), data, bytes);
	free(data);
	return 1;
}

static int build_stock(const char *dir)
{
	unsigned char *r = arena + (RSDP_AT - ARENA_BASE), *x = arena + (XSDT_AT - ARENA_BASE);
	unsigned char *f = arena + (FACP_AT - ARENA_BASE);
	memset(arena, 0, sizeof(arena));
	if (!place(dir, "FACP.aml", FACP_AT, 0x200) || !place(dir, "APIC.aml", APIC_AT, 0x200) ||
	    !place(dir, "SSDT1.aml", SSDT1_AT, 0x800) || !place(dir, "SSDT2.aml", SSDT2_AT, 0x2200) ||
	    !place(dir, "DSDT.aml", DSDT_AT, ARENA_BYTES - 0x3000u))
		return 0;
	memcpy(r, "RSD PTR ", 8);
	r[15] = 2;
	put32(r + 20, 36);
	put64(r + 24, XSDT_AT);
	fix_checksum(r, 20, 8);
	fix_checksum(r, 36, 32);
	memcpy(x, "XSDT", 4);
	put32(x + 4, 36 + 4 * 8);
	put64(x + 36, FACP_AT);
	put64(x + 44, APIC_AT);
	put64(x + 52, SSDT1_AT);
	put64(x + 60, SSDT2_AT);
	fix_checksum(x, 36 + 4 * 8, 9);
	/* The stored FADT names the DSDT at its address on the board. The test moves it into the arena: DSDT at 40,
	 * X_DSDT at 140 (ACPI 6.5 section 5.2.9). */
	put32(f + 40, (unsigned)DSDT_AT);
	put64(f + 140, DSDT_AT);
	fix_checksum(f, 276, 9);
	return 1;
}

static int stock_scan(const char *dir, const char *name, struct efl_aml_scan *s)
{
	size_t bytes = 0;
	unsigned char *data = load(dir, name, &bytes);
	int ok = data != NULL && efl_aml_scan(data, bytes, s);
	free(data);
	return ok;
}

static void fake_cpuid(unsigned function, struct cu_cpuid *out);
static unsigned long long fake_rdmsr(unsigned number);
static void model(const char *vendor, unsigned family, unsigned model_number, unsigned apm_edx);

static void test_stock(const char *dir)
{
	static struct efl_aml_scan s;
	static struct efl_acpi_table tables[EFL_ACPI_MAX_TABLES];
	static struct efl_acpi_madt_cpu madt[EFL_ACPI_MAX_MADT_CPUS];
	static struct cu_report_input in;
	static char text[CU_REPORT_BYTES];
	struct cu_report_power_summary sum;
	struct efl_acpi_fadt fadt;
	struct efl_acpi acpi;
	unsigned i, count = 0, dropped = 0, enabled = 0, cst = 0, pss = 0, n = 0;
	int truncated = 1;
	size_t chars;

	CHECK(dir != NULL);
	if (dir == NULL) return;

	/* SSDT1 "AMD CPU": the scopes \_PR_.C000 to \_PR_.C00B, each with _CST and _CSD. */
	CHECK(stock_scan(dir, "SSDT1.aml", &s));
	CHECK(s.objects == 12u && s.processor_objects == 0u && s.malformed == 0u && s.dropped == 0u);
	for (i = 0; i < s.objects; i++) {
		char want[16];
		snprintf(want, sizeof(want), "\\_PR_.C00%X", i);
		CHECK(!strcmp(s.object[i].path, want));
		CHECK(s.object[i].kind == EFL_AML_SCOPE);
		CHECK(s.object[i].names == ((1u << EFL_AML_CST) | (1u << EFL_AML_CSD)));
	}
	/* The DSDT: 16 Processor objects \_PR_.P000 to \_PR_.P00F, none with a power name. */
	CHECK(stock_scan(dir, "DSDT.aml", &s));
	CHECK(s.processor_objects == 16u && s.thermal_zones == 0u && s.dropped == 0u && s.malformed == 0u);
	for (i = 0; i < s.objects; i++) {
		if (s.object[i].kind == EFL_AML_PROCESSOR) {
			char want[16];
			snprintf(want, sizeof(want), "\\_PR_.P00%X", n++);
			CHECK(!strcmp(s.object[i].path, want));
		}
		if (s.object[i].names & (1u << EFL_AML_CST)) cst++;
		if (s.object[i].names & (1u << EFL_AML_PSS)) pss++;
	}
	CHECK(n == 16u && cst == 0u && pss == 0u);
	CHECK(s.counts[EFL_AML_TZ] == 0u && s.counts[EFL_AML_TMP] == 0u && s.counts[EFL_AML_FAN_STRING] == 0u &&
	      s.counts[EFL_AML_FAN_EISAID] == 0u);
	/* SSDT2 "AmdTable": no processor scope. */
	CHECK(stock_scan(dir, "SSDT2.aml", &s));
	CHECK(s.processor_objects == 0u);
	for (i = 0; i < s.objects; i++) CHECK((s.object[i].names & (1u << EFL_AML_CST)) == 0u);

	/* The same tables behind an RSDP and an XSDT. */
	CHECK(build_stock(dir));
	acpi.map = arena_map;
	acpi.context = NULL;
	acpi.rsdp = RSDP_AT;
	CHECK(efl_acpi_tables(&acpi, tables, EFL_ACPI_MAX_TABLES, &count, &dropped) == EFL_ACPI_OK);
	CHECK(count == 5u && dropped == 0u);
	CHECK(count == 5u && !strcmp(tables[0].signature, "FACP") && !strcmp(tables[2].signature, "SSDT") &&
	      !strcmp(tables[2].oem_table_id, "AMD CPU") && !strcmp(tables[4].signature, "DSDT"));
	for (i = 0; i < count; i++) CHECK(tables[i].checksum_ok && tables[i].bytes != NULL);

	cu_report_power_summary(&acpi, &sum);
	CHECK(sum.tables == 3u && sum.processor_objects == 16u);
	CHECK(sum.with_name[EFL_AML_CST] == 12u && sum.with_name[EFL_AML_CSD] == 12u);
	CHECK(sum.with_name[EFL_AML_PSS] == 0u && sum.with_name[EFL_AML_PCT] == 0u && sum.with_name[EFL_AML_PPC] == 0u &&
	      sum.with_name[EFL_AML_CPC] == 0u && sum.with_name[EFL_AML_PSD] == 0u && sum.with_name[EFL_AML_LPI] == 0u);

	CHECK(efl_acpi_madt_list(&acpi, madt, EFL_ACPI_MAX_MADT_CPUS, &count, &dropped) == EFL_ACPI_OK);
	for (i = 0; i < count; i++) if (madt[i].flags & 1u) enabled++;
	CHECK(count == 16u && dropped == 0u && enabled == 12u);
	CHECK(efl_acpi_madt_processors(&acpi, &n) == EFL_ACPI_OK && n == 12u);
	CHECK(efl_acpi_madt_list(&acpi, madt, 4, &count, &dropped) == EFL_ACPI_OK && count == 4u && dropped == 12u);

	CHECK(efl_acpi_fadt_fields(&acpi, &fadt) == EFL_ACPI_OK);
	CHECK(fadt.revision == 6u && fadt.length == 276u && fadt.has_reset);
	CHECK(fadt.p_lvl2_lat == 101u && fadt.p_lvl3_lat == 1001u);
	CHECK(fadt.reset_space == 1u && fadt.reset_width == 8u && fadt.reset_address == 0xCF9u && fadt.reset_value == 6u);

	/* The whole text, with two processors of test data. */
	memset(&in, 0, sizeof(in));
	in.version = "test";
	in.mp_present = 1;
	in.mp_all = 2;
	in.mp_enabled = 1;
	in.cpus = 2;
	in.cpu[0].info_ok = 1;
	in.cpu[0].status_flag = 0x7u;
	in.cpu[0].sampled = 1;
	model("AuthenticAMD", 0x17u, 0x47u, CU_CPUID_APM_HW_PSTATE);
	cu_cpu_sample(fake_cpuid, fake_rdmsr, &in.cpu[0].sample);
	in.cpu[1].number = 1;
	in.cpu[1].info_ok = 1;
	in.cpu[1].status_flag = 0x4u;
	in.cpus = 3;
	in.cpu[2].number = 2;
	in.cpu[2].info_ok = 1;
	in.cpu[2].status_flag = 0x6u;
	in.cpu[2].sample_status = 0x8000000000000012ull;	/* test data: EFI_TIMEOUT */
	in.acpi = &acpi;
	in.gops = 1;
	in.gop[0].max_mode = 2;
	in.gop[0].current = 1;
	in.gop[0].modes = 2;
	in.gop[0].mode[0].ok = 1;
	in.gop[0].mode[0].width = 800;
	in.gop[0].mode[0].height = 600;
	in.gop[0].mode[0].pixel_format = 1;
	in.gop[0].mode[0].pixels_per_line = 800;
	in.gop[0].mode[1].number = 1;
	chars = cu_report_write(&in, text, sizeof(text), &truncated);
	CHECK(chars > 0u && truncated == 0 && strlen(text) == chars);
	if (getenv("CU_REPORT_TEXT") != NULL) {	/* a copy of the text for a reader, at the path in that variable */
		FILE *f = fopen(getenv("CU_REPORT_TEXT"), "wb");
		if (f != NULL) { fwrite(text, 1, chars, f); fclose(f); }
	}
	CHECK(strstr(text, "\\_PR_.C000 (Scope): _CST _CSD\n") != NULL);
	CHECK(strstr(text, "\\_PR_.P00F (Processor): none of the names\n") != NULL);
	CHECK(strstr(text, "It is not an AML interpretation") != NULL);
	CHECK(strstr(text, "MADT: 16 processor entries listed, 12 enabled\n") != NULL);
	CHECK(strstr(text, "objects that define _CST 12 _CSD 12 _PSS 0 _PCT 0 _PPC 0 _PSD 0 _CPC 0 _LPI 0\n") != NULL);
	CHECK(strstr(text, "P_LVL2_LAT 101 us, P_LVL3_LAT 1001 us") != NULL);
	CHECK(strstr(text, "address 0xCF9; RESET_VALUE 0x6") != NULL);
	CHECK(strstr(text, "processor 1: not read, the processor is disabled\n") != NULL);
	CHECK(strstr(text, "processor 2: not read (StartupThisAP status 0x8000000000000012)\n") != NULL);
	CHECK(strstr(text, "mode 0: 800x600, PixelBlueGreenRedReserved8BitPerColor, 800 pixels per scan line\n") != NULL);
	CHECK(strstr(text, "mode 1: QueryMode failed\n") != NULL);
	CHECK(strstr(text, "open: the C-state configuration registers") != NULL);
	CHECK(strstr(text, "PStateDef[0] (MSR 0xC0010064) = 0x800000000000088C: PstateEn 1, CpuFid 0x8C, CpuDfsId 0x8") != NULL);
	/* A small buffer ends the text early and says so. */
	chars = cu_report_write(&in, text, 200, &truncated);
	CHECK(truncated == 1 && chars < 200u && strlen(text) == chars);
}

/* ---- the register guard ------------------------------------------------------------------------------- */

static struct { char vendor[13]; unsigned fms, ext_max, apm_edx, max_standard; } cpu_model;
static unsigned rdmsr_calls, rdmsr_numbers[32];

static void fake_cpuid(unsigned function, struct cu_cpuid *out)
{
	memset(out, 0, sizeof(*out));
	if (function == CU_CPUID_VENDOR) {
		out->eax = cpu_model.max_standard;
		memcpy(&out->ebx, cpu_model.vendor, 4);
		memcpy(&out->edx, cpu_model.vendor + 4, 4);
		memcpy(&out->ecx, cpu_model.vendor + 8, 4);
	} else if (function == CU_CPUID_FAM_MOD_STEP) {
		out->eax = cpu_model.fms;
		out->ebx = 5u << 24;
	} else if (function == CU_CPUID_EXT_MAX) {
		out->eax = cpu_model.ext_max;
	} else if (function == CU_CPUID_APM_INFO) {
		out->edx = cpu_model.apm_edx;
	}
}

static unsigned long long fake_rdmsr(unsigned number)
{
	if (rdmsr_calls < 32u) rdmsr_numbers[rdmsr_calls] = number;
	rdmsr_calls++;
	/* Test data: PStateDef with PstateEn, CpuFid 0x8C, CpuDfsId 8. */
	return number >= CU_MSR_PSTATE_DEF0 && number < CU_MSR_PSTATE_DEF0 + CU_MSR_PSTATE_DEFS ?
		(1ull << 63) | (8ull << 8) | 0x8Cu : 0x1000u + (number & 0xFFu);
}

static void model(const char *vendor, unsigned family, unsigned model_number, unsigned apm_edx)
{
	unsigned base_family = family >= 0xFu ? 0xFu : family, ext_family = family >= 0xFu ? family - 0xFu : 0;
	memcpy(cpu_model.vendor, vendor, 13);
	cpu_model.max_standard = 0x10u;
	cpu_model.fms = (ext_family << 20) | ((model_number >> 4) << 16) | (base_family << 8) | ((model_number & 0xFu) << 4) | 1u;
	cpu_model.ext_max = 0x80000020u;
	cpu_model.apm_edx = apm_edx;
	rdmsr_calls = 0;
}

static void test_msr_guard(void)
{
	struct cu_cpu_sample s;
	struct cu_pstate_def p;
	unsigned i;

	/* AMD, family 17h, HwPstate: every register of the list, and nothing else. */
	model("AuthenticAMD", 0x17u, 0x47u, CU_CPUID_APM_HW_PSTATE | CU_CPUID_APM_CPB);
	cu_cpu_sample(fake_cpuid, fake_rdmsr, &s);
	CHECK(s.id.amd && s.id.family == 0x17u && s.id.model == 0x47u && s.id.stepping == 1u && s.id.apic_id == 5u);
	CHECK(s.id.hw_pstate && s.id.cpb);
	CHECK(rdmsr_calls == CU_MSR_I_COUNT && s.read == (1u << CU_MSR_I_COUNT) - 1u);
	for (i = 0; i < CU_MSR_I_COUNT && i < rdmsr_calls; i++) CHECK(rdmsr_numbers[i] == cu_msr_def((enum cu_msr_index)i)->number);
	CHECK(cu_msr_def(CU_MSR_I_HWCR)->number == 0xC0010015u && cu_msr_def(CU_MSR_I_CSTATE_BASE_ADDR)->number == 0xC0010073u);
	CHECK(cu_msr_def((enum cu_msr_index)(CU_MSR_I_PSTATE_DEF0 + 7))->number == 0xC001006Bu);
	CHECK(cu_msr_def(CU_MSR_I_COUNT) == NULL);

	/* Without HwPstate: not PStateCurLim, PStateCtl, PStateStat. */
	model("AuthenticAMD", 0x17u, 0x47u, 0);
	cu_cpu_sample(fake_cpuid, fake_rdmsr, &s);
	CHECK(rdmsr_calls == CU_MSR_I_COUNT - 3u);
	CHECK(!(s.read & (1u << CU_MSR_I_PSTATE_CUR_LIM)) && !(s.read & (1u << CU_MSR_I_PSTATE_CTL)) &&
	      !(s.read & (1u << CU_MSR_I_PSTATE_STAT)) && (s.read & (1u << CU_MSR_I_HWCR)));

	/* Another family, another vendor, or no extended function: no read at all for the first two. */
	model("AuthenticAMD", 0x19u, 0x21u, CU_CPUID_APM_HW_PSTATE);
	cu_cpu_sample(fake_cpuid, fake_rdmsr, &s);
	CHECK(rdmsr_calls == 0u && s.read == 0u && s.id.family == 0x19u);
	model("GenuineIntel", 0x6u, 0x9Eu, CU_CPUID_APM_HW_PSTATE);
	cu_cpu_sample(fake_cpuid, fake_rdmsr, &s);
	CHECK(rdmsr_calls == 0u && s.read == 0u && !s.id.amd);
	model("AuthenticAMD", 0x17u, 0x47u, CU_CPUID_APM_HW_PSTATE);
	cpu_model.ext_max = 0x80000001u;
	cu_cpu_sample(fake_cpuid, fake_rdmsr, &s);
	CHECK(!s.id.apm_info && !s.id.hw_pstate && rdmsr_calls == CU_MSR_I_COUNT - 3u);
	model("AuthenticAMD", 0x17u, 0x47u, CU_CPUID_APM_HW_PSTATE);
	cpu_model.max_standard = 0;
	cu_cpu_sample(fake_cpuid, fake_rdmsr, &s);
	CHECK(rdmsr_calls == 0u && s.id.family == 0u);

	cu_pstate_def_decode((1ull << 63) | (2ull << 30) | (0x55ull << 22) | (0x30ull << 14) | (8ull << 8) | 0x8Cu, &p);
	CHECK(p.enabled == 1u && p.idd_div == 2u && p.idd_value == 0x55u && p.cpu_vid == 0x30u);
	CHECK(p.cpu_dfs_id == 8u && p.cpu_fid == 0x8Cu && p.core_cof_mhz == 3500u);
	cu_pstate_def_decode(0x8Cu, &p);
	CHECK(p.enabled == 0u && p.core_cof_mhz == 0u);
	CHECK(cu_pstate_max_val(0x25u) == 2u && cu_cur_pstate_limit(0x25u) == 5u && cu_pstate_low3(0xFAu) == 2u);
	CHECK(cu_cstate_addr(0x12345678ull) == 0x5678u && cu_hwcr_cpb_dis(1ull << 25) == 1u);
}

/* ---- the load option and the file name ------------------------------------------------------------------- */

static void wide(unsigned short *out, const char *s)
{
	while ((*out++ = (unsigned char)*s++) != 0) {}
}

static void test_report_option(void)
{
	unsigned short opt[64], path[64], file[64];
	struct stage_options so;
	struct cu_options o;
	unsigned i;
	wide(opt, "bc250-stage.efi report=1 reset=fadt");
	stage_options_parse(opt, 64, &so);
	cu_options_read(&so, &o);
	CHECK(o.report == 1 && o.reset == CU_RESET_FADT && o.key == 1 && stage_options_unclaimed(&so) == 0);
	wide(opt, "report=0");
	stage_options_parse(opt, 64, &so);
	cu_options_read(&so, &o);
	CHECK(o.report == 0 && stage_options_unclaimed(&so) == 0);
	wide(opt, "key=0");
	stage_options_parse(opt, 64, &so);
	cu_options_read(&so, &o);
	CHECK(o.key == 0 && o.report == 0 && stage_options_unclaimed(&so) == 0);
	wide(opt, "key=no");
	stage_options_parse(opt, 64, &so);
	cu_options_read(&so, &o);
	CHECK(o.key == 1 && stage_options_unclaimed(&so) == 1);
	wide(opt, "report=yes");
	stage_options_parse(opt, 64, &so);
	cu_options_read(&so, &o);
	CHECK(o.report == 0 && stage_options_unclaimed(&so) == 1);
	wide(opt, "reset=none");
	stage_options_parse(opt, 64, &so);
	cu_options_read(&so, &o);
	CHECK(o.report == 0);

	wide(file, "\\EFI\\BC250\\bc250-stage.efi");
	CHECK(efl_sibling_path(file, CU_REPORT_NAME, path, 64) == 1);
	wide(opt, "\\EFI\\BC250\\core-unlock-report.txt");
	for (i = 0; opt[i]; i++) CHECK(path[i] == opt[i]);
	CHECK(path[i] == 0);
	CHECK(efl_sibling_path(file, CU_REPORT_NAME, path, 20) == 0);
	CHECK(efl_sibling_path(file, NULL, path, 64) == 0);
}

unsigned report_tests(const char *evidence_dir, unsigned *check_count)
{
	test_aml_synthetic();
	test_msr_guard();
	test_stock(evidence_dir);
	test_report_option();
	*check_count = checks;
	return failures;
}
