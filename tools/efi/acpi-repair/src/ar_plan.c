/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The plan and the builder of the ACPI repair (ar_plan.h). The AML encodings are those of the ACPI Specification
 * 6.5 chapter 20, and the objects are _CST and _CSD of its chapter 8 ("Processor Configuration and Control"). */
#include <stddef.h>
#include <string.h>
#include "ar_plan.h"
#include "efl_amlw.h"
#include "efl_sha256.h"

#define SDT_HEADER_BYTES	36u
#define SDT_LENGTH		4u
#define SDT_OEM_ID		10u
#define SDT_OEM_TABLE_ID	16u
#define NAME_SEG		4u

#define AML_ZERO_OP		0x00u
#define AML_ONE_OP		0x01u
#define AML_NAME_OP		0x08u
#define AML_BYTE_PREFIX		0x0Au
#define AML_WORD_PREFIX		0x0Bu
#define AML_DWORD_PREFIX	0x0Cu
#define AML_QWORD_PREFIX	0x0Eu
#define AML_SCOPE_OP		0x10u
#define AML_LOCAL0_OP		0x60u	/* Local0 to Local7 are 0x60 to 0x67, Arg0 to Arg6 are 0x68 to 0x6E */
#define AML_ARG6_OP		0x6Eu
#define AML_PACKAGE_OP		0x12u

/* _CSD (ACPI 6.5 chapter 8): one package of NumEntries, Revision, Domain, CoordType, NumProcessors, Index. */
#define CSD_ENTRIES		6u
#define CSD_REVISION		0u
#define CSD_COORD_HW_ALL	0xFEu
#define CSD_INDEX		0u
/* The stock table puts the two threads of one core in one domain. */
#define CSD_THREADS		2u

/* The identity of the stock CPU table of BIOS P3.00, fact M965. The header fields are the bytes of the table,
 * with their padding. The digests are SHA-256: of the _CST object of each scope (NameOp, the name and the package,
 * 79 bytes), and of the whole table of the capture with 12 logical processors (1560 bytes). */
static const unsigned char stock_oem_id[6] = { 'A', 'M', 'D', 0, 0, 0 };
static const unsigned char stock_oem_table_id[8] = { 'A', 'M', 'D', ' ', 'C', 'P', 'U', 0 };
static const unsigned char stock_cst_sha256[EFL_SHA256_BYTES] = {
	0x0a, 0x4b, 0x37, 0x46, 0x67, 0x42, 0xce, 0x2c, 0x77, 0x88, 0x01, 0x0c, 0x91, 0x3e, 0x9c, 0x46,
	0x26, 0x8a, 0x92, 0x0d, 0xd9, 0x36, 0x24, 0xe9, 0x2c, 0x8d, 0x8a, 0x25, 0x82, 0x74, 0x2e, 0x3c
};
static const unsigned char stock_table_sha256[EFL_SHA256_BYTES] = {
	0x2f, 0x0c, 0x27, 0x55, 0x57, 0x23, 0xa8, 0xec, 0xda, 0x6a, 0x96, 0x80, 0x59, 0xf6, 0x6b, 0xdf,
	0x08, 0x2e, 0x63, 0x19, 0x4d, 0x31, 0x89, 0x87, 0x92, 0xac, 0x71, 0xcb, 0x1f, 0x86, 0xa4, 0xd1
};

/* The header of the stock DSDT, and the three methods that it calls and no stock table defines. Each call has
 * one operand after the name (fact M966), so each stub has one argument. */
static const unsigned char dsdt_oem_id[6] = { 'A', 'L', 'A', 'S', 'K', 'A' };
static const unsigned char dsdt_oem_table_id[8] = { 'A', ' ', 'M', ' ', 'I', ' ', 0, 0 };
static const char *const stub_names[AR_STUBS] = { "APTS", "AWAK", "AFN7" };
#define STUB_ARGUMENTS		1u

static const char *const idle_text[AR_IDLE_COUNT] = {
	"repair",
	"off by the load option",
	"no SSDT with the header of the stock CPU table: nothing to repair",
	"the CPU SSDT does not have the stock content: left alone",
	"the names that the stock CPU table targets are defined: left alone",
	"a processor already has _CST or _CSD: left alone",
	"the MADT gives no usable list of enabled processors: left alone",
	"an enabled processor has no Processor object in the DSDT: left alone",
	"the ACPI tables cannot be read whole, so a name collision cannot be excluded: left alone",
};

static const char *const stubs_text[AR_STUBS_COUNT] = {
	"repair",
	"off by the load option",
	"the DSDT does not have the header of the stock DSDT: left alone",
	"no called method is without a definition: nothing to repair",
	"the ACPI tables cannot be read whole, so a name collision cannot be excluded: left alone",
};

const char *ar_idle_text(enum ar_idle state)
{
	return (unsigned)state < AR_IDLE_COUNT ? idle_text[state] : "?";
}

const char *ar_stubs_text(enum ar_stubs state)
{
	return (unsigned)state < AR_STUBS_COUNT ? stubs_text[state] : "?";
}

const char *ar_stub_name(unsigned index)
{
	return index < AR_STUBS ? stub_names[index] : "?";
}

static size_t le32(const unsigned char *p)
{
	return (size_t)p[0] | ((size_t)p[1] << 8) | ((size_t)p[2] << 16) | ((size_t)p[3] << 24);
}

int ar_table_sound(const unsigned char *table, size_t bytes)
{
	unsigned char sum = 0;
	size_t i;
	if (table == NULL || bytes < SDT_HEADER_BYTES || le32(table + SDT_LENGTH) != bytes) return 0;
	for (i = 0; i < bytes; i++) sum = (unsigned char)(sum + table[i]);
	return sum == 0;
}

static int header_is(const struct ar_table *t, const char *signature, const unsigned char *oem_id,
		     const unsigned char *oem_table_id)
{
	return ar_table_sound(t->bytes, t->length) && !memcmp(t->bytes, signature, 4) &&
	       !memcmp(t->bytes + SDT_OEM_ID, oem_id, 6) && !memcmp(t->bytes + SDT_OEM_TABLE_ID, oem_table_id, 8);
}

/* ---- the stock CPU table --------------------------------------------------------------------------- */

/* An integer constant at p. Returns its bytes, 0 when p does not start one. */
static size_t parse_integer(const unsigned char *p, const unsigned char *end, unsigned long long *value)
{
	unsigned width = 0, i;
	if (p >= end) return 0;
	switch (*p) {
	case AML_ZERO_OP: *value = 0; return 1;
	case AML_ONE_OP: *value = 1; return 1;
	case AML_BYTE_PREFIX: width = 1; break;
	case AML_WORD_PREFIX: width = 2; break;
	case AML_DWORD_PREFIX: width = 4; break;
	case AML_QWORD_PREFIX: width = 8; break;
	default: return 0;
	}
	if ((size_t)(end - p) < 1u + width) return 0;
	*value = 0;
	for (i = 0; i < width; i++) *value |= (unsigned long long)p[1u + i] << (8u * i);
	return 1u + width;
}

/* "Name (segment, Package ...)" at p: NameOp, the segment, PackageOp and the package. Returns the bytes of the
 * whole object, 0 when p does not start one. *inside is the first byte after the package length. */
static size_t named_package(const unsigned char *p, const unsigned char *end, const char *segment,
			    const unsigned char **inside)
{
	size_t length, lb;
	if ((size_t)(end - p) < 1u + NAME_SEG + 2u || p[0] != AML_NAME_OP || memcmp(p + 1, segment, NAME_SEG) != 0 ||
	    p[1u + NAME_SEG] != AML_PACKAGE_OP)
		return 0;
	lb = efl_aml_pkg_length(p + 2u + NAME_SEG, end, &length);
	if (lb == 0 || length < lb + 1u || (size_t)(end - (p + 2u + NAME_SEG)) < length) return 0;
	*inside = p + 2u + NAME_SEG + lb;
	return 2u + NAME_SEG + length;
}

/* The domain of a _CSD object that has the stock form, or -1. */
static long csd_domain(const unsigned char *object, size_t bytes)
{
	const unsigned char *end = object + bytes, *p, *inner_end;
	unsigned long long v[CSD_ENTRIES];
	size_t length, lb, n;
	unsigned i;
	if (named_package(object, end, "_CSD", &p) != bytes) return -1;
	/* One element, which is a package of six integers. */
	if (p + 2 > end || p[0] != 1u || p[1] != AML_PACKAGE_OP) return -1;
	lb = efl_aml_pkg_length(p + 2, end, &length);
	if (lb == 0 || p + 2 + length != end) return -1;
	inner_end = end;
	p += 2u + lb;
	if (p >= inner_end || *p++ != CSD_ENTRIES) return -1;
	for (i = 0; i < CSD_ENTRIES; i++) {
		n = parse_integer(p, inner_end, &v[i]);
		if (n == 0) return -1;
		p += n;
	}
	if (p != inner_end) return -1;
	if (v[0] != CSD_ENTRIES || v[1] != CSD_REVISION || v[3] != CSD_COORD_HW_ALL || v[4] != CSD_THREADS ||
	    v[5] != CSD_INDEX || v[2] > 0xFFFFu)
		return -1;
	return (long)v[2];
}

static void stock_path(unsigned index, char *path)
{
	static const char hex[] = "0123456789ABCDEF";
	memcpy(path, "\\_PR_.C000", 11);
	path[7] = hex[(index >> 8) & 0xFu];
	path[8] = hex[(index >> 4) & 0xFu];
	path[9] = hex[index & 0xFu];
}

/* Reads the stock CPU table into the plan. 1 when every byte of it has the stock form: scopes \_PR_.C000 and up
 * with no gap, each with the stock _CST object and a _CSD object of the stock form, and nothing else. */
static int parse_stock(const struct ar_table *t, struct ar_plan *plan)
{
	const unsigned char *p = t->bytes + SDT_HEADER_BYTES, *end = t->bytes + t->length;
	unsigned char digest[EFL_SHA256_BYTES];
	unsigned scopes = 0;
	while (p < end) {
		const unsigned char *scope_end, *q, *inside;
		char path[EFL_AML_PATH_CHARS], want[EFL_AML_PATH_CHARS];
		size_t length, lb, nb, cst, csd;
		if (scopes >= AR_MAX_CPUS || *p != AML_SCOPE_OP) return 0;
		lb = efl_aml_pkg_length(p + 1, end, &length);
		if (lb == 0 || length < lb || (size_t)(end - (p + 1)) < length) return 0;
		scope_end = p + 1 + length;
		nb = efl_aml_absolute_path(p + 1 + lb, scope_end, path);
		stock_path(scopes, want);
		if (nb == 0 || strcmp(path, want) != 0) return 0;
		q = p + 1 + lb + nb;
		cst = named_package(q, scope_end, "_CST", &inside);
		if (cst == 0) return 0;
		efl_sha256(q, cst, digest);
		if (memcmp(digest, stock_cst_sha256, sizeof(digest)) != 0) return 0;
		if (scopes == 0) {
			plan->cst = q;
			plan->cst_bytes = cst;
		}
		q += cst;
		csd = named_package(q, scope_end, "_CSD", &inside);
		if (csd == 0 || q + csd != scope_end || csd_domain(q, csd) < 0) return 0;
		plan->csd[scopes] = q;
		plan->csd_bytes[scopes] = csd;
		scopes++;
		p = scope_end;
	}
	plan->stock_scopes = scopes;
	return scopes > 0;
}

/* ---- the plan -------------------------------------------------------------------------------------- */

/* 1 when the input is the whole set of definition blocks and each one is a sound table. */
static int tables_whole(const struct ar_input *in)
{
	unsigned i;
	if (!in->complete || in->ssdts > AR_MAX_SSDTS || !ar_table_sound(in->dsdt.bytes, in->dsdt.length)) return 0;
	for (i = 0; i < in->ssdts; i++)
		if (!ar_table_sound(in->ssdt[i].bytes, in->ssdt[i].length)) return 0;
	return 1;
}

/* 1 when the two threads of the core of position k are neighbours in the list and in their APIC IDs. */
static int thread_pair(const struct ar_input *in, unsigned k)
{
	unsigned first = k & ~1u;
	if (first + 1u >= in->cpus) return 0;
	return (in->cpu[first].apic_id & 1u) == 0 && in->cpu[first + 1u].apic_id == in->cpu[first].apic_id + 1u;
}

static void plan_idle(const struct ar_input *in, struct ar_plan *plan)
{
	static struct efl_aml_scan scan;	/* large for a firmware stack */
	static const char *const watch[1] = { "C000" };
	const unsigned idle_names = (1u << EFL_AML_CST) | (1u << EFL_AML_CSD);
	unsigned char digest[EFL_SHA256_BYTES];
	unsigned i, k, rule_holds = 1;

	plan->stock_ssdt = -1;
	if (!in->want_idle) { plan->idle = AR_IDLE_OFF; return; }
	if (!tables_whole(in)) { plan->idle = AR_IDLE_TABLES_INCOMPLETE; return; }
	for (i = 0; i < in->ssdts; i++)
		if (header_is(&in->ssdt[i], "SSDT", stock_oem_id, stock_oem_table_id)) {
			plan->stock_ssdt = (int)i;
			break;
		}
	if (plan->stock_ssdt < 0) { plan->idle = AR_IDLE_NO_STOCK_TABLE; return; }
	if (!parse_stock(&in->ssdt[plan->stock_ssdt], plan)) { plan->idle = AR_IDLE_NOT_STOCK; return; }
	efl_sha256(in->ssdt[plan->stock_ssdt].bytes, in->ssdt[plan->stock_ssdt].length, digest);
	plan->stock_exact = !memcmp(digest, stock_table_sha256, sizeof(digest));

	/* The defect is that no table defines the objects that the stock table extends. */
	for (i = 0; i <= in->ssdts; i++) {
		const struct ar_table *t = i == 0 ? &in->dsdt : &in->ssdt[i - 1u];
		if (i > 0 && (int)(i - 1u) == plan->stock_ssdt) continue;
		/* A table that does not scan, or whose object list is cut, can hide a definition. */
		if (!efl_aml_scan_watch(t->bytes, t->length, watch, 1, &scan) || scan.dropped) {
			plan->idle = AR_IDLE_TABLES_INCOMPLETE;
			return;
		}
		if (scan.watched[0]) { plan->idle = AR_IDLE_TARGETS_EXIST; return; }
		/* The built table defines _CST and _CSD. A scope that has one of them in another table needs no repair,
		 * and a second definition of the name would collide. */
		if (i > 0)
			for (k = 0; k < scan.objects; k++)
				if (scan.object[k].names & idle_names) { plan->idle = AR_IDLE_ALREADY_PRESENT; return; }
	}

	if (in->cpus == 0 || in->cpus > AR_MAX_CPUS) { plan->idle = AR_IDLE_NO_PROCESSORS; return; }
	if (in->dsdt.bytes == NULL || !efl_aml_scan(in->dsdt.bytes, in->dsdt.length, &scan) || scan.dropped) {
		plan->idle = AR_IDLE_NO_PROCESSOR_OBJECT;
		return;
	}
	for (k = 0; k < scan.objects; k++)
		if (scan.object[k].names & idle_names) { plan->idle = AR_IDLE_ALREADY_PRESENT; return; }

	/* The rule that the stock scopes obey: position k is in domain k / 2, with the other thread of its core. A
	 * processor past the stock scopes gets a _CSD only when every stock scope obeys the rule. */
	for (k = 0; k < plan->stock_scopes && k < in->cpus; k++)
		if (csd_domain(plan->csd[k], plan->csd_bytes[k]) != (long)(k / CSD_THREADS) || !thread_pair(in, k))
			rule_holds = 0;

	for (k = 0; k < in->cpus; k++) {
		struct ar_target *target = &plan->target[plan->targets];
		const struct efl_aml_object *object = NULL;
		unsigned o;
		for (o = 0; o < scan.objects; o++)
			if (scan.object[o].kind == EFL_AML_PROCESSOR && scan.object[o].proc_id == in->cpu[k].uid) {
				if (object != NULL) { plan->idle = AR_IDLE_NO_PROCESSOR_OBJECT; return; }	/* not unique */
				object = &scan.object[o];
			}
		if (object == NULL) { plan->idle = AR_IDLE_NO_PROCESSOR_OBJECT; return; }
		if (k < plan->stock_scopes) {
			target->stock_scope = (int)k;
			target->domain = (unsigned)csd_domain(plan->csd[k], plan->csd_bytes[k]);
			plan->copied++;
		} else if (rule_holds && thread_pair(in, k)) {
			target->stock_scope = -1;
			target->domain = k / CSD_THREADS;
			plan->derived++;
		} else {
			plan->left_out++;
			continue;
		}
		memcpy(target->path, object->path, sizeof(target->path));
		plan->targets++;
	}
	plan->idle = AR_IDLE_REPAIR;
}

/* 1 when the table names the method, and each place that names it has exactly one operand of one byte after
 * the name: an ArgN or a LocalN object. AML does not encode the argument count of a call, so a stub must have the
 * count that the call sites show. A call with one such operand is the only form that the stock DSDT has (fact
 * M966). A stub with one argument takes that operand. On any other form the method gets no stub. */
static int calls_with_one_operand(const unsigned char *t, size_t bytes, const char *name)
{
	size_t i, hits = 0;
	for (i = SDT_HEADER_BYTES; i + NAME_SEG <= bytes; i++) {
		if (memcmp(t + i, name, NAME_SEG) != 0) continue;
		if (i + NAME_SEG >= bytes || t[i + NAME_SEG] < AML_LOCAL0_OP || t[i + NAME_SEG] > AML_ARG6_OP) return 0;
		hits++;
	}
	return hits > 0;
}

static void plan_stubs(const struct ar_input *in, struct ar_plan *plan)
{
	static struct efl_aml_scan scan;
	unsigned defined[AR_STUBS] = { 0 }, i, s;
	if (!in->want_stubs) { plan->stubs = AR_STUBS_OFF; return; }
	if (!tables_whole(in)) { plan->stubs = AR_STUBS_TABLES_INCOMPLETE; return; }
	if (!header_is(&in->dsdt, "DSDT", dsdt_oem_id, dsdt_oem_table_id)) { plan->stubs = AR_STUBS_NOT_STOCK_DSDT; return; }
	for (i = 0; i <= in->ssdts; i++) {
		const struct ar_table *t = i == 0 ? &in->dsdt : &in->ssdt[i - 1u];
		if (!efl_aml_scan_watch(t->bytes, t->length, stub_names, AR_STUBS, &scan)) {
			plan->stubs = AR_STUBS_TABLES_INCOMPLETE;
			return;
		}
		for (s = 0; s < AR_STUBS; s++) defined[s] += scan.watched[s];
	}
	for (s = 0; s < AR_STUBS; s++)
		if (defined[s] == 0 && calls_with_one_operand(in->dsdt.bytes, in->dsdt.length, stub_names[s]))
			plan->stub_mask |= 1u << s;
	plan->stubs = plan->stub_mask ? AR_STUBS_REPAIR : AR_STUBS_NOTHING_MISSING;
}

void ar_plan_make(const struct ar_input *in, struct ar_plan *plan)
{
	memset(plan, 0, sizeof(*plan));
	plan->device = in->want_device;
	plan_idle(in, plan);
	if (plan->idle != AR_IDLE_REPAIR) plan->targets = plan->copied = plan->derived = plan->left_out = 0;
	plan_stubs(in, plan);
}

/* ---- the builder ----------------------------------------------------------------------------------- */

size_t ar_build_idle(const struct ar_plan *plan, unsigned char *out, size_t capacity)
{
	struct efl_amlw w;
	unsigned k;
	if (plan->idle != AR_IDLE_REPAIR || plan->targets == 0) return 0;
	efl_amlw_init(&w, out, capacity);
	/* Revision 2: the integers of this table are 64 bits wide. Every integer in it fits in 32 bits. */
	efl_amlw_table_begin(&w, "SSDT", 2, AR_OEM_ID, AR_IDLE_TABLE_ID, 1, "BCST", 1);
	for (k = 0; k < plan->targets; k++) {
		const struct ar_target *target = &plan->target[k];
		size_t scope = efl_amlw_scope_begin(&w, target->path);
		efl_amlw_bytes(&w, plan->cst, plan->cst_bytes);
		if (target->stock_scope >= 0) {
			efl_amlw_bytes(&w, plan->csd[target->stock_scope], plan->csd_bytes[target->stock_scope]);
		} else {
			size_t outer, inner;
			efl_amlw_name(&w, "_CSD");
			outer = efl_amlw_package_begin(&w, 1);
			inner = efl_amlw_package_begin(&w, CSD_ENTRIES);
			efl_amlw_integer(&w, CSD_ENTRIES);
			efl_amlw_integer(&w, CSD_REVISION);
			efl_amlw_integer(&w, target->domain);
			efl_amlw_integer(&w, CSD_COORD_HW_ALL);
			efl_amlw_integer(&w, CSD_THREADS);
			efl_amlw_integer(&w, CSD_INDEX);
			efl_amlw_end(&w, inner);
			efl_amlw_end(&w, outer);
		}
		efl_amlw_end(&w, scope);
	}
	return efl_amlw_table_end(&w);
}

size_t ar_build_stubs(const struct ar_plan *plan, unsigned char *out, size_t capacity)
{
	struct efl_amlw w;
	unsigned s;
	if (plan->stubs != AR_STUBS_REPAIR && !plan->device) return 0;
	if (plan->stubs == AR_STUBS_REPAIR && plan->stub_mask == 0 && !plan->device) return 0;
	efl_amlw_init(&w, out, capacity);
	efl_amlw_table_begin(&w, "SSDT", 2, AR_OEM_ID, AR_STUB_TABLE_ID, 1, "BCST", 1);
	for (s = 0; s < AR_STUBS; s++)
		if (plan->stub_mask & (1u << s)) efl_amlw_empty_method(&w, stub_names[s], STUB_ARGUMENTS);
	if (plan->device) {
		size_t scope = efl_amlw_scope_begin(&w, "\\_SB_");
		size_t mark = efl_amlw_device_begin(&w, "TPM0");
		efl_amlw_name_string(&w, "_HID", "BC250001");
		if (plan->crb_base != 0) {
			/* Memory32Fixed (0x86) of the reserved page, then the end tag: the CRB window of the TPM work. */
			unsigned char crs[12];
			unsigned long long base = plan->crb_base;
			unsigned k;
			crs[0] = 0x86; crs[1] = 0x09; crs[2] = 0x00; crs[3] = 0x00;
			for (k = 0; k < 4; k++) crs[4 + k] = (unsigned char)(base >> (8 * k));
			for (k = 0; k < 4; k++) crs[8 + k] = (unsigned char)(0x1000u >> (8 * k));
			crs[8] = 0x00; crs[9] = 0x10; crs[10] = 0x00; crs[11] = 0x00;
			efl_amlw_name(&w, "_CRS");
			efl_amlw_buffer(&w, crs, sizeof(crs));
		}
		efl_amlw_end(&w, mark);
		efl_amlw_end(&w, scope);
	}
	return efl_amlw_table_end(&w);
}

size_t ar_build_tpm2(const struct ar_plan *plan, unsigned char *out, size_t capacity)
{
	struct efl_amlw w;
	unsigned char body[16];
	unsigned long long base = plan->crb_base;
	unsigned k;
	if (!plan->device || base == 0) return 0;
	efl_amlw_init(&w, out, capacity);
	efl_amlw_table_begin(&w, "TPM2", 4, AR_OEM_ID, "BC250TPM", 1, "BCST", 1);
	/* PlatformClass 0 in the low half of the flags, the control area of locality 0 (page + 0x40), start method 7
	 * (CRB), then the 12 zero bytes of the start method specific parameters that revision 4 defines. */
	body[0] = body[1] = body[2] = body[3] = 0;
	for (k = 0; k < 8; k++) body[4 + k] = (unsigned char)((base + 0x40ull) >> (8 * k));
	body[12] = 7; body[13] = 0; body[14] = 0; body[15] = 0;
	efl_amlw_bytes(&w, body, sizeof(body));
	{
		unsigned char params[12];
		for (k = 0; k < sizeof(params); k++) params[k] = 0;
		efl_amlw_bytes(&w, params, sizeof(params));
	}
	return efl_amlw_table_end(&w);
}

int ar_check_idle(const struct ar_plan *plan, const unsigned char *table, size_t bytes)
{
	static struct efl_aml_scan scan;
	const unsigned want = (1u << EFL_AML_CST) | (1u << EFL_AML_CSD);
	unsigned k;
	if (!ar_table_sound(table, bytes) || !efl_aml_scan(table, bytes, &scan)) return 0;
	if (scan.malformed || scan.dropped || scan.processor_objects || scan.objects != plan->targets) return 0;
	for (k = 0; k < scan.objects; k++) {
		const unsigned char *p = table + SDT_HEADER_BYTES, *end = table + bytes;
		long domain = -1;
		unsigned at;
		if (scan.object[k].kind != EFL_AML_SCOPE || scan.object[k].names != want ||
		    strcmp(scan.object[k].path, plan->target[k].path) != 0)
			return 0;
		/* The _CSD of scope k, read back with the reader of the stock table. */
		for (at = 0; at <= k && p < end; at++) {
			size_t length, lb = efl_aml_pkg_length(p + 1, end, &length);
			char path[EFL_AML_PATH_CHARS];
			size_t nb;
			if (*p != AML_SCOPE_OP || lb == 0 || (size_t)(end - (p + 1)) < length) return 0;
			if (at == k) {
				const unsigned char *q, *scope_end = p + 1 + length;
				nb = efl_aml_absolute_path(p + 1 + lb, scope_end, path);
				if (nb == 0) return 0;
				q = p + 1 + lb + nb;
				if ((size_t)(scope_end - q) < plan->cst_bytes || memcmp(q, plan->cst, plan->cst_bytes) != 0) return 0;
				q += plan->cst_bytes;
				domain = csd_domain(q, (size_t)(scope_end - q));
			}
			p += 1u + length;
		}
		if (domain != (long)plan->target[k].domain) return 0;
	}
	return 1;
}

int ar_check_stubs(const struct ar_plan *plan, const unsigned char *table, size_t bytes)
{
	static struct efl_aml_scan scan;
	unsigned s;
	if (!ar_table_sound(table, bytes) || !efl_aml_scan_watch(table, bytes, stub_names, AR_STUBS, &scan)) return 0;
	if (scan.malformed || scan.objects) return 0;
	for (s = 0; s < AR_STUBS; s++)
		if (scan.watched[s] != ((plan->stub_mask >> s) & 1u)) return 0;
	return 1;
}
