/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * Host tests of the ACPI repair: the plan and the table builder (ar_plan.c), checked with the AML name scan and
 * with the checksum rule of the table header.
 *
 * Two sets of stock tables are read:
 * - The dump of unit A outside the repository (the build script gives its directory). When it is not there, that
 *   part is skipped and the test says so.
 * - The same tables as the repository stores them as evidence. The cases that change a table start from these.
 *
 * The APIC IDs of the processors 13 to 16 in the cases with 16 processors are test data. The IDs that the board
 * gives after the core unlock are not measured (docs/linux-session-wishlist.md).
 */
#include "efl_test.h"
#include "ar_plan.h"
#include "efl_amlw.h"

#define CST_BYTES	79u	/* the _CST object of the stock table */
#define SCOPE_BYTES	127u	/* one scope of the stock table */

struct stock {
	unsigned char *dsdt, *ssdt1, *ssdt2, *apic;
	size_t dsdt_bytes, ssdt1_bytes, ssdt2_bytes, apic_bytes;
};

static void fix_checksum(unsigned char *table, size_t bytes)
{
	unsigned char sum = 0;
	size_t i;
	table[9] = 0;
	for (i = 0; i < bytes; i++) sum = (unsigned char)(sum + table[i]);
	table[9] = (unsigned char)(0u - sum);
}

static void set_length(unsigned char *table, size_t bytes)
{
	table[4] = (unsigned char)bytes;
	table[5] = (unsigned char)(bytes >> 8);
	table[6] = (unsigned char)(bytes >> 16);
	table[7] = (unsigned char)(bytes >> 24);
	fix_checksum(table, bytes);
}

static int load_stock(const char *dir, struct stock *s)
{
	memset(s, 0, sizeof(*s));
	s->dsdt = efl_test_load(dir, "DSDT.aml", &s->dsdt_bytes);
	s->ssdt1 = efl_test_load(dir, "SSDT1.aml", &s->ssdt1_bytes);
	s->ssdt2 = efl_test_load(dir, "SSDT2.aml", &s->ssdt2_bytes);
	s->apic = efl_test_load(dir, "APIC.aml", &s->apic_bytes);
	return s->dsdt && s->ssdt1 && s->ssdt2 && s->apic;
}

static void free_stock(struct stock *s)
{
	free(s->dsdt);
	free(s->ssdt1);
	free(s->ssdt2);
	free(s->apic);
}

/* The enabled Local APIC entries of the MADT (ACPI 6.5 section 5.2.12.2: type 0, length 8, UID, APIC ID, flags). */
static unsigned madt_cpus(const unsigned char *apic, size_t bytes, struct ar_cpu *cpu, unsigned capacity)
{
	size_t at = 44;
	unsigned n = 0;
	while (at + 2u <= bytes && apic[at + 1] >= 2u && at + apic[at + 1] <= bytes) {
		if (apic[at] == 0 && apic[at + 1] == 8u && (apic[at + 4] & 1u) && n < capacity) {
			cpu[n].uid = apic[at + 2];
			cpu[n].apic_id = apic[at + 3];
			n++;
		}
		at += apic[at + 1];
	}
	return n;
}

static void stock_input(const struct stock *s, struct ar_input *in)
{
	memset(in, 0, sizeof(*in));
	in->dsdt.bytes = s->dsdt;
	in->dsdt.length = s->dsdt_bytes;
	in->ssdt[0].bytes = s->ssdt1;
	in->ssdt[0].length = s->ssdt1_bytes;
	in->ssdt[1].bytes = s->ssdt2;
	in->ssdt[1].length = s->ssdt2_bytes;
	in->ssdts = 2;
	in->complete = 1;
	in->cpus = madt_cpus(s->apic, s->apic_bytes, in->cpu, AR_MAX_CPUS);
	in->want_idle = 1;
	in->want_stubs = 1;
}

static unsigned count_of(const unsigned char *hay, size_t hay_bytes, const unsigned char *needle, size_t needle_bytes)
{
	unsigned n = 0;
	size_t i;
	for (i = 0; i + needle_bytes <= hay_bytes; i++)
		if (!memcmp(hay + i, needle, needle_bytes)) n++;
	return n;
}

static void save(const char *dir, const char *name, const unsigned char *data, size_t bytes)
{
	char path[1024];
	FILE *f;
	if (dir == NULL) return;
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	f = fopen(path, "wb");
	if (f == NULL) return;
	fwrite(data, 1, bytes, f);
	fclose(f);
}

/* The checks of a built idle table that do not use the builder's own check. */
static void check_idle_table(const struct ar_plan *plan, const unsigned char *table, size_t bytes, unsigned targets)
{
	static struct efl_aml_scan scan;
	unsigned char sum = 0;
	size_t i;
	unsigned k;
	CHECK(bytes >= 36u && table[4] + (table[5] << 8) == (int)bytes && table[6] == 0 && table[7] == 0);
	for (i = 0; i < bytes; i++) sum = (unsigned char)(sum + table[i]);
	CHECK(sum == 0);
	CHECK(!memcmp(table, "SSDT", 4) && !memcmp(table + 10, "BC250 CPUIDLE ", 14));
	CHECK(efl_aml_scan(table, bytes, &scan) == 1);
	CHECK(scan.objects == targets && scan.malformed == 0 && scan.dropped == 0 && scan.processor_objects == 0);
	for (k = 0; k < scan.objects && k < targets; k++) {
		char want[16];
		snprintf(want, sizeof(want), "\\_PR_.P%03X", k);
		CHECK(!strcmp(scan.object[k].path, want));
		CHECK(scan.object[k].kind == EFL_AML_SCOPE);
		CHECK(scan.object[k].names == ((1u << EFL_AML_CST) | (1u << EFL_AML_CSD)));
	}
	/* Every _CST is the firmware's object, byte for byte. */
	CHECK(plan->cst_bytes == CST_BYTES);
	CHECK(count_of(table, bytes, plan->cst, plan->cst_bytes) == targets);
	CHECK(ar_check_idle(plan, table, bytes) == 1);
}

/* The whole repair on the stock tables of one directory. */
static void test_stock(const char *dir, const char *label, const char *out_dir)
{
	static struct ar_input in;
	static struct ar_plan plan, again;
	unsigned char table[8192], stubs[256];
	struct stock s;
	size_t bytes, stub_bytes;
	unsigned k;

	if (!load_stock(dir, &s)) {
		printf("SKIP acpi-repair, %s: no DSDT.aml, SSDT1.aml, SSDT2.aml and APIC.aml in %s\n", label,
		       dir ? dir : "(no directory given)");
		free_stock(&s);
		return;
	}
	stock_input(&s, &in);
	CHECK(in.cpus == 12);
	ar_plan_make(&in, &plan);
	CHECK(plan.idle == AR_IDLE_REPAIR && plan.stock_ssdt == 0 && plan.stock_exact == 1);
	CHECK(plan.stock_scopes == 12 && plan.targets == 12 && plan.copied == 12 && plan.derived == 0 && plan.left_out == 0);
	CHECK(plan.stubs == AR_STUBS_REPAIR && plan.stub_mask == 7u);

	bytes = ar_build_idle(&plan, table, sizeof(table));
	/* The same objects in scopes of the same size: the table has the size of the stock table. */
	CHECK(bytes == s.ssdt1_bytes);
	check_idle_table(&plan, table, bytes, 12);
	for (k = 0; k < 12; k++) {
		CHECK(plan.target[k].domain == k / 2u);
		/* Scope k of the built table is scope k of the stock table but for the name of its target. */
		CHECK(!memcmp(table + 36u + k * SCOPE_BYTES + 13u, s.ssdt1 + 36u + k * SCOPE_BYTES + 13u, SCOPE_BYTES - 13u));
		CHECK(table[36u + k * SCOPE_BYTES + 9u] == 'P' && s.ssdt1[36u + k * SCOPE_BYTES + 9u] == 'C');
	}
	CHECK(ar_build_idle(&plan, table, bytes - 1u) == 0);		/* no room */
	bytes = ar_build_idle(&plan, table, sizeof(table));
	save(out_dir, "ar-idle.aml", table, bytes);

	stub_bytes = ar_build_stubs(&plan, stubs, sizeof(stubs));
	CHECK(stub_bytes == 36u + 3u * 7u);
	CHECK(ar_table_sound(stubs, stub_bytes) && ar_check_stubs(&plan, stubs, stub_bytes) == 1);
	CHECK(!memcmp(stubs + 10, "BC250 AMLSTUB ", 14));
	CHECK(!memcmp(stubs + 36, "\x14\x06" "APTS" "\x01" "\x14\x06" "AWAK" "\x01" "\x14\x06" "AFN7" "\x01", 21));
	CHECK(ar_build_stubs(&plan, stubs, 40) == 0);
	stub_bytes = ar_build_stubs(&plan, stubs, sizeof(stubs));
	save(out_dir, "ar-stubs.aml", stubs, stub_bytes);

	/* The TPM experiment: the same table with Device(TPM0) { Name(_HID, "BC250001") } after the stubs. */
	plan.device = 1;
	stub_bytes = ar_build_stubs(&plan, stubs, sizeof(stubs));
	CHECK(stub_bytes == 36u + 3u * 7u + 7u + 15u);
	CHECK(ar_table_sound(stubs, stub_bytes));
	CHECK(stubs[36u + 21u] == 0x5B && stubs[36u + 22u] == 0x82 && !memcmp(stubs + 36u + 24u, "TPM0", 4));
	CHECK(!memcmp(stubs + 36u + 28u, "\x08_HID\x0d" "BC250001", 15));
	CHECK(ar_build_stubs(&plan, stubs, 50) == 0);		/* no room for the device */
	plan.device = 0;
	/* The table goes back to the stub-only form that the second-plan checks below expect. */
	stub_bytes = ar_build_stubs(&plan, stubs, sizeof(stubs));

	/* The built table's own check refuses a changed table. */
	table[bytes - 10u] ^= 1u;
	CHECK(ar_check_idle(&plan, table, bytes) == 0);			/* the checksum */
	fix_checksum(table, bytes);
	CHECK(ar_check_idle(&plan, table, bytes) == 0);			/* the content */
	table[bytes - 10u] ^= 1u;
	fix_checksum(table, bytes);
	CHECK(ar_check_idle(&plan, table, bytes) == 1);

	/* A second run in the same start. With the stock table removed there is nothing to repair. With the stock
	 * table still there, the processors already have _CST. The stubs are defined in both cases. */
	in.ssdt[0].bytes = table;
	in.ssdt[0].length = bytes;
	in.ssdt[2].bytes = stubs;
	in.ssdt[2].length = stub_bytes;
	in.ssdts = 3;
	ar_plan_make(&in, &again);
	CHECK(again.idle == AR_IDLE_NO_STOCK_TABLE && again.targets == 0);
	CHECK(again.stubs == AR_STUBS_NOTHING_MISSING && again.stub_mask == 0);
	CHECK(ar_build_idle(&again, table, sizeof(table)) == 0 && ar_build_stubs(&again, stubs, sizeof(stubs)) == 0);
	bytes = ar_build_idle(&plan, table, sizeof(table));
	in.ssdt[0].bytes = s.ssdt1;
	in.ssdt[0].length = s.ssdt1_bytes;
	in.ssdt[2].bytes = table;
	in.ssdt[2].length = bytes;
	ar_plan_make(&in, &again);
	CHECK(again.idle == AR_IDLE_ALREADY_PRESENT);
	free_stock(&s);
}

/* The cases that start from the stock tables and change one thing. */
static void test_variants(const char *dir, const char *out_dir)
{
	static struct ar_input in;
	static struct ar_plan plan;
	static unsigned char table[8192], changed[32768], extra[256];
	struct efl_amlw w;
	struct stock s;
	size_t bytes, mark, extra_bytes;
	unsigned k;

	if (!load_stock(dir, &s)) {
		printf("SKIP acpi-repair, changed tables: no stock tables in %s\n", dir ? dir : "(no directory given)");
		free_stock(&s);
		return;
	}

	/* 16 processors, the four new ones in thread pairs: their _CSD follows the rule of the stock scopes. */
	stock_input(&s, &in);
	for (k = 12; k < 16; k++) {
		in.cpu[k].uid = k + 1u;
		in.cpu[k].apic_id = 16u + (k - 12u);
	}
	in.cpus = 16;
	ar_plan_make(&in, &plan);
	CHECK(plan.idle == AR_IDLE_REPAIR && plan.targets == 16 && plan.copied == 12 && plan.derived == 4 && plan.left_out == 0);
	CHECK(plan.target[12].domain == 6 && plan.target[15].domain == 7 && plan.target[15].stock_scope == -1);
	CHECK(!strcmp(plan.target[15].path, "\\_PR_.P00F"));
	bytes = ar_build_idle(&plan, table, sizeof(table));
	check_idle_table(&plan, table, bytes, 16);
	save(out_dir, "ar-idle-16.aml", table, bytes);

	/* 16 processors, the new ones not in pairs: they are left out, and the 12 are repaired. */
	for (k = 12; k < 16; k++) in.cpu[k].apic_id = 16u + 2u * (k - 12u);
	ar_plan_make(&in, &plan);
	CHECK(plan.idle == AR_IDLE_REPAIR && plan.targets == 12 && plan.derived == 0 && plan.left_out == 4);
	bytes = ar_build_idle(&plan, table, sizeof(table));
	check_idle_table(&plan, table, bytes, 12);
	/* 13 processors: the last one has no second thread. */
	in.cpus = 13;
	ar_plan_make(&in, &plan);
	CHECK(plan.idle == AR_IDLE_REPAIR && plan.targets == 12 && plan.left_out == 1);

	/* A stock table with 8 scopes (not the digest of unit A, the stock content). */
	stock_input(&s, &in);
	memcpy(changed, s.ssdt1, 36u + 8u * SCOPE_BYTES);
	set_length(changed, 36u + 8u * SCOPE_BYTES);
	in.ssdt[0].bytes = changed;
	in.ssdt[0].length = 36u + 8u * SCOPE_BYTES;
	ar_plan_make(&in, &plan);
	CHECK(plan.idle == AR_IDLE_REPAIR && plan.stock_exact == 0 && plan.stock_scopes == 8);
	CHECK(plan.targets == 12 && plan.copied == 8 && plan.derived == 4);
	bytes = ar_build_idle(&plan, table, sizeof(table));
	check_idle_table(&plan, table, bytes, 12);

	/* One changed byte in a _CST: not the stock content, so nothing is built. */
	memcpy(changed, s.ssdt1, s.ssdt1_bytes);
	changed[36u + 3u * SCOPE_BYTES + 13u + 40u] ^= 0x10u;
	fix_checksum(changed, s.ssdt1_bytes);
	in.ssdt[0].length = s.ssdt1_bytes;
	ar_plan_make(&in, &plan);
	CHECK(plan.idle == AR_IDLE_NOT_STOCK && plan.targets == 0);
	CHECK(ar_build_idle(&plan, table, sizeof(table)) == 0);
	/* A changed _CSD coordination type. */
	memcpy(changed, s.ssdt1, s.ssdt1_bytes);
	CHECK(count_of(changed + 36u, SCOPE_BYTES, (const unsigned char *)"\x0C\xFE\x00\x00\x00", 5) == 1);
	for (k = 36u; k < 36u + SCOPE_BYTES; k++)
		if (changed[k] == 0x0Cu && changed[k + 1u] == 0xFEu) changed[k + 1u] = 0xFCu;
	fix_checksum(changed, s.ssdt1_bytes);
	ar_plan_make(&in, &plan);
	CHECK(plan.idle == AR_IDLE_NOT_STOCK);
	/* A scope out of order. */
	memcpy(changed, s.ssdt1, s.ssdt1_bytes);
	changed[36u + SCOPE_BYTES + 12u] = '5';
	fix_checksum(changed, s.ssdt1_bytes);
	ar_plan_make(&in, &plan);
	CHECK(plan.idle == AR_IDLE_NOT_STOCK);
	/* A wrong checksum in any definition block: the set does not read whole, so nothing is built. */
	memcpy(changed, s.ssdt1, s.ssdt1_bytes);
	changed[9] ^= 1u;
	ar_plan_make(&in, &plan);
	CHECK(plan.idle == AR_IDLE_TABLES_INCOMPLETE && plan.stubs == AR_STUBS_TABLES_INCOMPLETE);
	CHECK(ar_build_idle(&plan, table, sizeof(table)) == 0 && ar_build_stubs(&plan, table, sizeof(table)) == 0);
	stock_input(&s, &in);
	memcpy(changed, s.ssdt2, s.ssdt2_bytes);
	changed[s.ssdt2_bytes - 1u] ^= 1u;
	in.ssdt[1].bytes = changed;
	ar_plan_make(&in, &plan);
	CHECK(plan.idle == AR_IDLE_TABLES_INCOMPLETE && plan.stubs == AR_STUBS_TABLES_INCOMPLETE);
	/* The caller says that the root table did not give every table. */
	stock_input(&s, &in);
	in.complete = 0;
	ar_plan_make(&in, &plan);
	CHECK(plan.idle == AR_IDLE_TABLES_INCOMPLETE && plan.stubs == AR_STUBS_TABLES_INCOMPLETE && plan.targets == 0);
	/* More definition blocks than the input holds. */
	stock_input(&s, &in);
	in.ssdts = AR_MAX_SSDTS + 1u;
	ar_plan_make(&in, &plan);
	CHECK(plan.idle == AR_IDLE_TABLES_INCOMPLETE && plan.stubs == AR_STUBS_TABLES_INCOMPLETE);
	/* A definition block that is sound as a table and does not scan as AML: its signature is not DSDT or SSDT. */
	stock_input(&s, &in);
	memcpy(changed, s.ssdt2, s.ssdt2_bytes);
	changed[0] = 'X';
	fix_checksum(changed, s.ssdt2_bytes);
	in.ssdt[1].bytes = changed;
	ar_plan_make(&in, &plan);
	CHECK(plan.idle == AR_IDLE_TABLES_INCOMPLETE && plan.stubs == AR_STUBS_TABLES_INCOMPLETE);
	/* The option turns a repair off before the table set matters. */
	in.want_idle = in.want_stubs = 0;
	ar_plan_make(&in, &plan);
	CHECK(plan.idle == AR_IDLE_OFF && plan.stubs == AR_STUBS_OFF);

	/* Another table defines C000: the stock table is not defective on this firmware. */
	stock_input(&s, &in);
	efl_amlw_init(&w, extra, sizeof(extra));
	efl_amlw_table_begin(&w, "SSDT", 2, "TEST", "C000", 1, "BCST", 1);
	mark = efl_amlw_scope_begin(&w, "\\_PR_");
	efl_amlw_name(&w, "C000");
	efl_amlw_integer(&w, 0);
	efl_amlw_end(&w, mark);
	extra_bytes = efl_amlw_table_end(&w);
	CHECK(extra_bytes != 0);
	in.ssdt[2].bytes = extra;
	in.ssdt[2].length = extra_bytes;
	in.ssdts = 3;
	ar_plan_make(&in, &plan);
	CHECK(plan.idle == AR_IDLE_TARGETS_EXIST);

	/* Another table gives a processor a _CSD and no _CST: a built table would define _CSD a second time. */
	efl_amlw_init(&w, extra, sizeof(extra));
	efl_amlw_table_begin(&w, "SSDT", 2, "TEST", "CSD", 1, "BCST", 1);
	mark = efl_amlw_scope_begin(&w, "\\_PR_.P003");
	efl_amlw_name(&w, "_CSD");
	efl_amlw_integer(&w, 0);
	efl_amlw_end(&w, mark);
	extra_bytes = efl_amlw_table_end(&w);
	CHECK(extra_bytes != 0);
	in.ssdt[2].length = extra_bytes;
	ar_plan_make(&in, &plan);
	CHECK(plan.idle == AR_IDLE_ALREADY_PRESENT && plan.targets == 0);

	/* One of the three methods is defined by another table: only the two others get a stub. */
	efl_amlw_init(&w, extra, sizeof(extra));
	efl_amlw_table_begin(&w, "SSDT", 2, "TEST", "AWAK", 1, "BCST", 1);
	efl_amlw_empty_method(&w, "AWAK", 1);
	extra_bytes = efl_amlw_table_end(&w);
	in.ssdt[2].length = extra_bytes;
	ar_plan_make(&in, &plan);
	CHECK(plan.idle == AR_IDLE_REPAIR && plan.stubs == AR_STUBS_REPAIR && plan.stub_mask == 5u);
	bytes = ar_build_stubs(&plan, table, sizeof(table));
	CHECK(bytes == 36u + 2u * 7u && ar_check_stubs(&plan, table, bytes) == 1);

	/* A call of another form: the name with a constant after it, not an ArgN or a LocalN. That method gets no
	 * stub, because the argument count of the call is then not known. */
	stock_input(&s, &in);
	memcpy(changed, s.dsdt, s.dsdt_bytes);
	for (k = 36u; k + 5u <= s.dsdt_bytes; k++)
		if (!memcmp(changed + k, "AWAK", 4)) { changed[k + 4u] = 0x0Au; break; }
	fix_checksum(changed, s.dsdt_bytes);
	in.dsdt.bytes = changed;
	ar_plan_make(&in, &plan);
	CHECK(plan.stubs == AR_STUBS_REPAIR && plan.stub_mask == 5u);

	/* The load options. */
	stock_input(&s, &in);
	in.want_idle = 0;
	ar_plan_make(&in, &plan);
	CHECK(plan.idle == AR_IDLE_OFF && plan.stubs == AR_STUBS_REPAIR);
	in.want_idle = 1;
	in.want_stubs = 0;
	ar_plan_make(&in, &plan);
	CHECK(plan.idle == AR_IDLE_REPAIR && plan.stubs == AR_STUBS_OFF && plan.stub_mask == 0);

	/* The MADT. */
	stock_input(&s, &in);
	in.cpus = 0;
	ar_plan_make(&in, &plan);
	CHECK(plan.idle == AR_IDLE_NO_PROCESSORS);
	stock_input(&s, &in);
	in.cpu[5].uid = 99;
	ar_plan_make(&in, &plan);
	CHECK(plan.idle == AR_IDLE_NO_PROCESSOR_OBJECT && plan.targets == 0);

	/* A DSDT of another firmware: no stubs. The idle repair does not depend on the DSDT header. */
	stock_input(&s, &in);
	memcpy(changed, s.dsdt, s.dsdt_bytes);
	changed[10] = 'X';
	fix_checksum(changed, s.dsdt_bytes);
	in.dsdt.bytes = changed;
	ar_plan_make(&in, &plan);
	CHECK(plan.stubs == AR_STUBS_NOT_STOCK_DSDT && plan.idle == AR_IDLE_REPAIR);
	free_stock(&s);
}

/* Tables that have nothing of the stock firmware. */
static void test_foreign(void)
{
	static struct ar_input in;
	static struct ar_plan plan;
	unsigned char dsdt[128], ssdt[128], out[256];
	struct efl_amlw w;
	size_t mark;

	efl_amlw_init(&w, dsdt, sizeof(dsdt));
	efl_amlw_table_begin(&w, "DSDT", 2, "OTHER", "BOARD", 1, "BCST", 1);
	efl_amlw_empty_method(&w, "APTS", 1);
	memset(&in, 0, sizeof(in));
	in.dsdt.bytes = dsdt;
	in.dsdt.length = efl_amlw_table_end(&w);
	efl_amlw_init(&w, ssdt, sizeof(ssdt));
	efl_amlw_table_begin(&w, "SSDT", 2, "OTHER", "CPU", 1, "BCST", 1);
	mark = efl_amlw_scope_begin(&w, "\\_PR_.C000");
	efl_amlw_name(&w, "_CST");
	efl_amlw_integer(&w, 0);
	efl_amlw_end(&w, mark);
	in.ssdt[0].bytes = ssdt;
	in.ssdt[0].length = efl_amlw_table_end(&w);
	in.ssdts = 1;
	in.cpus = 2;
	in.cpu[0].uid = 1;
	in.cpu[1].uid = 2;
	in.cpu[1].apic_id = 1;
	in.complete = 1;
	in.want_idle = in.want_stubs = 1;
	ar_plan_make(&in, &plan);
	CHECK(plan.idle == AR_IDLE_NO_STOCK_TABLE && plan.stubs == AR_STUBS_NOT_STOCK_DSDT);
	CHECK(ar_build_idle(&plan, out, sizeof(out)) == 0 && ar_build_stubs(&plan, out, sizeof(out)) == 0);

	/* No tables at all. */
	memset(&in, 0, sizeof(in));
	in.want_idle = in.want_stubs = 1;
	ar_plan_make(&in, &plan);
	CHECK(plan.idle == AR_IDLE_TABLES_INCOMPLETE && plan.stubs == AR_STUBS_TABLES_INCOMPLETE);
	CHECK(ar_table_sound(NULL, 0) == 0 && ar_table_sound(dsdt, 20) == 0);
	CHECK(ar_idle_text(AR_IDLE_COUNT) != NULL && ar_stubs_text(AR_STUBS_COUNT) != NULL && ar_stub_name(AR_STUBS) != NULL);
}

unsigned ar_tests(const char *evidence_dir, const char *dump_dir, const char *out_dir, unsigned *check_count)
{
	test_foreign();
	test_stock(evidence_dir, "the tables stored as evidence", NULL);
	test_stock(dump_dir, "the dump of unit A", out_dir);
	test_variants(evidence_dir, out_dir);
	*check_count = checks;
	return failures;
}
