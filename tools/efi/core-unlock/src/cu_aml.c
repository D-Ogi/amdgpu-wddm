/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The name scan of a DSDT or SSDT (cu_aml.h). The encodings are those of the ACPI Specification 6.5 chapter 20, each
 * named once below. */
#include <stddef.h>
#include <string.h>
#include "cu_aml.h"

/* ACPI 6.5 section 20.2: the opcodes and prefixes that the scan follows or skips. */
#define AML_ZERO_OP		0x00u
#define AML_ALIAS_OP		0x06u
#define AML_NAME_OP		0x08u
#define AML_BYTE_PREFIX		0x0Au
#define AML_WORD_PREFIX		0x0Bu
#define AML_DWORD_PREFIX	0x0Cu
#define AML_STRING_PREFIX	0x0Du
#define AML_QWORD_PREFIX	0x0Eu
#define AML_SCOPE_OP		0x10u
#define AML_BUFFER_OP		0x11u
#define AML_PACKAGE_OP		0x12u
#define AML_VAR_PACKAGE_OP	0x13u
#define AML_METHOD_OP		0x14u
#define AML_EXTERNAL_OP		0x15u
#define AML_DUAL_NAME_PREFIX	0x2Eu
#define AML_MULTI_NAME_PREFIX	0x2Fu
#define AML_EXT_OP_PREFIX	0x5Bu
#define AML_ROOT_CHAR		0x5Cu
#define AML_PARENT_PREFIX_CHAR	0x5Eu
/* The second byte after AML_EXT_OP_PREFIX. */
#define AML_EXT_FIELD_OP	0x81u
#define AML_EXT_DEVICE_OP	0x82u
#define AML_EXT_PROCESSOR_OP	0x83u
#define AML_EXT_POWER_RES_OP	0x84u
#define AML_EXT_THERMAL_ZONE_OP	0x85u
#define AML_EXT_INDEX_FIELD_OP	0x86u
#define AML_EXT_BANK_FIELD_OP	0x87u
/* The fixed operands after the name of a Processor object (ProcID, PblkAddr, PblkLen) and of a PowerResource
 * object (SystemLevel, ResourceOrder). */
#define AML_PROCESSOR_OPERANDS	6u
#define AML_POWER_RES_OPERANDS	3u
/* An External statement: the name, ObjectType and ArgumentCount. */
#define AML_EXTERNAL_OPERANDS	2u

#define SDT_HEADER_BYTES	36u
#define SDT_LENGTH		4u
#define NAME_SEG		4u

static const char power_names[CU_AML_POWER_NAMES][NAME_SEG + 1u] = {
	"_CST", "_CSD", "_PSS", "_PCT", "_PPC", "_PSD", "_CPC", "_LPI"
};

const char *cu_aml_power_name_text(enum cu_aml_power_name name)
{
	return (unsigned)name < CU_AML_POWER_NAMES ? power_names[name] : "?";
}

const char *cu_aml_kind_text(enum cu_aml_kind kind)
{
	switch (kind) {
	case CU_AML_PROCESSOR: return "Processor";
	case CU_AML_DEVICE_CPU: return "Device ACPI0007";
	case CU_AML_DEVICE: return "Device";
	case CU_AML_SCOPE: return "Scope";
	default: return "?";
	}
}

static const char count_names[CU_AML_COUNTS][8] = {
	"_TZ_", "_TMP", "_CRT", "_PSV", "_AC0", "_FST", "PNP0C0B", "PNP0C0B"
};

const char *cu_aml_count_text(enum cu_aml_count count)
{
	switch (count) {
	case CU_AML_FAN_STRING: return "PNP0C0B (string)";
	case CU_AML_FAN_EISAID: return "PNP0C0B (EISA ID)";
	default: return (unsigned)count < CU_AML_COUNTS ? count_names[count] : "?";
	}
}

unsigned cu_aml_eisaid(const char *id)
{
	unsigned compressed = 0, product = 0, i;
	if (id == NULL) return 0;
	for (i = 0; i < 3u; i++) {
		if (id[i] < 'A' || id[i] > 'Z') return 0;
		compressed = (compressed << 5) | (unsigned)(id[i] - 0x40);
	}
	for (i = 3u; i < 7u; i++) {
		unsigned c = (unsigned char)id[i], v;
		if (c >= '0' && c <= '9') v = c - '0';
		else if (c >= 'A' && c <= 'F') v = c - 'A' + 10u;
		else return 0;
		product = (product << 4) | v;
	}
	if (id[7] != 0) return 0;
	/* The three letters and then the four hex digits, both as big-endian 16-bit values, in a little-endian DWORD. */
	return ((compressed >> 8) & 0xFFu) | ((compressed & 0xFFu) << 8) | ((product >> 8) << 16) | ((product & 0xFFu) << 24);
}

/* ---- the name strings ----------------------------------------------------------------------------- */

struct name {
	int absolute;
	unsigned parents;	/* the number of ParentPrefixChar */
	unsigned segments;
	unsigned char segment[CU_AML_MAX_SEGMENTS][NAME_SEG];
};

static int lead_char(unsigned char c) { return (c >= 'A' && c <= 'Z') || c == '_'; }
static int name_char(unsigned char c) { return lead_char(c) || (c >= '0' && c <= '9'); }

static int name_seg_ok(const unsigned char *p)
{
	return lead_char(p[0]) && name_char(p[1]) && name_char(p[2]) && name_char(p[3]);
}

/* A NameString at p (ACPI 6.5 section 20.2.2). Returns its bytes, 0 when it does not parse inside end. */
static size_t parse_name(const unsigned char *p, const unsigned char *end, struct name *out)
{
	const unsigned char *at = p;
	unsigned count, i;
	memset(out, 0, sizeof(*out));
	if (at < end && *at == AML_ROOT_CHAR) { out->absolute = 1; at++; }
	else while (at < end && *at == AML_PARENT_PREFIX_CHAR) { out->parents++; at++; }
	if (at >= end) return 0;
	if (*at == AML_ZERO_OP) return (size_t)(at + 1 - p);
	if (*at == AML_DUAL_NAME_PREFIX) { count = 2u; at++; }
	else if (*at == AML_MULTI_NAME_PREFIX) {
		if (at + 1 >= end) return 0;
		count = at[1];
		at += 2;
	} else count = 1u;
	if (count == 0 || count > CU_AML_MAX_SEGMENTS || (size_t)(end - at) < (size_t)count * NAME_SEG) return 0;
	for (i = 0; i < count; i++, at += NAME_SEG) {
		if (!name_seg_ok(at)) return 0;
		memcpy(out->segment[i], at, NAME_SEG);
	}
	out->segments = count;
	return (size_t)(at - p);
}

/* A PkgLength at p (ACPI 6.5 section 20.2.4). Returns its bytes and sets *length, 0 when it does not parse. */
static size_t parse_pkg_length(const unsigned char *p, const unsigned char *end, size_t *length)
{
	unsigned follow, i;
	if (p >= end) return 0;
	follow = p[0] >> 6;
	if ((size_t)(end - p) < 1u + follow) return 0;
	if (follow == 0) { *length = p[0] & 0x3Fu; return 1; }
	*length = p[0] & 0x0Fu;
	for (i = 0; i < follow; i++) *length |= (size_t)p[1 + i] << (4u + 8u * i);
	return 1u + follow;
}

/* ---- the scope stack ------------------------------------------------------------------------------ */

struct scope {
	const unsigned char *end;
	unsigned segments;
	unsigned char segment[CU_AML_MAX_SEGMENTS][NAME_SEG];
	int object;		/* the index of its list entry, or -1 */
	int opener;		/* the opcode that opened it, AML_SCOPE_OP or an extended opcode */
};

struct scanner {
	struct scope stack[CU_AML_MAX_DEPTH];
	unsigned depth;		/* stack[0] is the root of the namespace */
	struct cu_aml_scan *out;
};

/* The absolute path of a name seen in the current scope. 0 when it leaves the root or is too deep. */
static int resolve(const struct scope *current, const struct name *n, struct scope *into)
{
	unsigned i;
	if (n->absolute) into->segments = 0;
	else {
		if (n->parents > current->segments) return 0;
		into->segments = current->segments - n->parents;
		memcpy(into->segment, current->segment, (size_t)into->segments * NAME_SEG);
	}
	if (into->segments + n->segments > CU_AML_MAX_SEGMENTS) return 0;
	for (i = 0; i < n->segments; i++) memcpy(into->segment[into->segments + i], n->segment[i], NAME_SEG);
	into->segments += n->segments;
	return 1;
}

static void format_path(const struct scope *s, char *out)
{
	size_t used = 0;
	unsigned i;
	out[used++] = '\\';
	for (i = 0; i < s->segments && used + NAME_SEG + 2u <= CU_AML_PATH_CHARS; i++) {
		if (i) out[used++] = '.';
		memcpy(out + used, s->segment[i], NAME_SEG);
		used += NAME_SEG;
	}
	out[used] = 0;
}

/* The list entry of the current scope, made on first use. -1 when the list is full. */
static int object_of(struct scanner *sc, enum cu_aml_kind kind)
{
	struct scope *top = &sc->stack[sc->depth];
	struct cu_aml_object *o;
	if (top->object >= 0) return top->object;
	if (sc->out->objects >= CU_AML_MAX_OBJECTS) {
		sc->out->dropped++;
		return -1;
	}
	o = &sc->out->object[sc->out->objects];
	format_path(top, o->path);
	o->kind = kind;
	o->names = 0;
	top->object = (int)sc->out->objects++;
	return top->object;
}

static enum cu_aml_kind kind_of_opener(int opener)
{
	return opener == AML_EXT_DEVICE_OP ? CU_AML_DEVICE : CU_AML_SCOPE;
}

/* Opens a scope at p, whose PkgLength starts at pkg. Returns the bytes up to the end of the name and its fixed
 * operands, 0 when the opening does not parse. */
static size_t open_scope(struct scanner *sc, const unsigned char *p, const unsigned char *pkg, int opener,
			 size_t operands)
{
	struct scope *top = &sc->stack[sc->depth], next;
	const unsigned char *body;
	struct name n;
	size_t length, lb, nb;
	lb = parse_pkg_length(pkg, top->end, &length);
	if (lb == 0 || length < lb) return 0;
	nb = parse_name(pkg + lb, top->end, &n);
	/* Scope (\) names the root with no segment. Every object that a scope opening creates has a name. */
	if (nb == 0 || (n.segments == 0 && (opener != AML_SCOPE_OP || !n.absolute))) return 0;
	body = pkg + lb + nb + operands;
	next.end = pkg + length;
	if (next.end > top->end || body > next.end) return 0;
	if (sc->depth + 1u >= CU_AML_MAX_DEPTH || !resolve(top, &n, &next)) return 0;
	next.object = -1;
	next.opener = opener;
	sc->stack[++sc->depth] = next;
	if (opener == AML_EXT_PROCESSOR_OP) {
		sc->out->processor_objects++;
		(void)object_of(sc, CU_AML_PROCESSOR);
	} else if (opener == AML_EXT_THERMAL_ZONE_OP) {
		sc->out->thermal_zones++;
	}
	return (size_t)(body - p);
}

/* A Name or Method that defines one segment in the current scope. */
static void defined(struct scanner *sc, const struct name *n, const unsigned char *after, const unsigned char *end)
{
	static const unsigned char cpu_hid[] = "ACPI0007";	/* ACPI 6.5 section 8.4: the processor device */
	unsigned i;
	int o;
	if (n->absolute || n->parents || n->segments != 1u) return;
	for (i = 0; i < CU_AML_POWER_NAMES; i++)
		if (!memcmp(n->segment[0], power_names[i], NAME_SEG)) {
			o = object_of(sc, kind_of_opener(sc->stack[sc->depth].opener));
			if (o >= 0) sc->out->object[o].names |= 1u << i;
			return;
		}
	if (!memcmp(n->segment[0], "_HID", NAME_SEG) && sc->stack[sc->depth].opener == AML_EXT_DEVICE_OP &&
	    (size_t)(end - after) >= sizeof(cpu_hid) + 1u && after[0] == AML_STRING_PREFIX &&
	    !memcmp(after + 1, cpu_hid, sizeof(cpu_hid))) {
		o = object_of(sc, CU_AML_DEVICE_CPU);
		if (o >= 0) sc->out->object[o].kind = CU_AML_DEVICE_CPU;
	}
}

/* The bytes of a construct with a PkgLength at pkg, counted from p. 0 when it does not parse. */
static size_t skip_package(const unsigned char *p, const unsigned char *pkg, const unsigned char *end)
{
	size_t length, lb = parse_pkg_length(pkg, end, &length);
	if (lb == 0 || length < lb || (size_t)(end - pkg) < length) return 0;
	return (size_t)(pkg + length - p);
}

static size_t count_bytes(const unsigned char *t, size_t bytes, const unsigned char *pattern, size_t n)
{
	size_t i, hits = 0;
	for (i = 0; i + n <= bytes; i++)
		if (!memcmp(t + i, pattern, n)) hits++;
	return hits;
}

int cu_aml_scan(const unsigned char *table, size_t bytes, struct cu_aml_scan *out)
{
	static struct scanner sc;	/* the stack is large for a firmware stack */
	const unsigned char *p, *end;
	size_t length;
	unsigned i;
	memset(out, 0, sizeof(*out));
	if (table == NULL || bytes < SDT_HEADER_BYTES) return 0;
	if (memcmp(table, "DSDT", 4) != 0 && memcmp(table, "SSDT", 4) != 0) return 0;
	length = (size_t)table[SDT_LENGTH] | ((size_t)table[SDT_LENGTH + 1] << 8) | ((size_t)table[SDT_LENGTH + 2] << 16) |
		 ((size_t)table[SDT_LENGTH + 3] << 24);
	if (length < SDT_HEADER_BYTES || length > bytes) return 0;

	for (i = 0; i < CU_AML_COUNTS; i++) {
		unsigned char eisa[4];
		unsigned v;
		if (i == CU_AML_FAN_EISAID) {
			v = cu_aml_eisaid("PNP0C0B");	/* ACPI 6.5 section 11.3: the fan device */
			eisa[0] = (unsigned char)v; eisa[1] = (unsigned char)(v >> 8);
			eisa[2] = (unsigned char)(v >> 16); eisa[3] = (unsigned char)(v >> 24);
			out->counts[i] = (unsigned)count_bytes(table, length, eisa, 4);
		} else {
			out->counts[i] = (unsigned)count_bytes(table, length, (const unsigned char *)count_names[i],
							      strlen(count_names[i]));
		}
	}

	memset(&sc, 0, sizeof(sc));
	sc.out = out;
	end = table + length;
	sc.stack[0].end = end;
	sc.stack[0].object = -1;
	sc.stack[0].opener = AML_SCOPE_OP;
	p = table + SDT_HEADER_BYTES;
	while (p < end) {
		struct name n;
		size_t step = 0, nb;
		const unsigned char *limit;
		while (sc.depth > 0 && p >= sc.stack[sc.depth].end) sc.depth--;
		limit = sc.stack[sc.depth].end;
		switch (*p) {
		case AML_SCOPE_OP:
			step = open_scope(&sc, p, p + 1, AML_SCOPE_OP, 0);
			if (step == 0) out->malformed++;
			break;
		case AML_EXT_OP_PREFIX:
			if (p + 1 >= limit) break;
			switch (p[1]) {
			case AML_EXT_DEVICE_OP:
			case AML_EXT_THERMAL_ZONE_OP:
				step = open_scope(&sc, p, p + 2, p[1], 0);
				if (step == 0) out->malformed++;
				break;
			case AML_EXT_PROCESSOR_OP:
				step = open_scope(&sc, p, p + 2, p[1], AML_PROCESSOR_OPERANDS);
				if (step == 0) out->malformed++;
				break;
			case AML_EXT_POWER_RES_OP:
				step = open_scope(&sc, p, p + 2, p[1], AML_POWER_RES_OPERANDS);
				if (step == 0) out->malformed++;
				break;
			case AML_EXT_FIELD_OP:
			case AML_EXT_INDEX_FIELD_OP:
			case AML_EXT_BANK_FIELD_OP:
				step = skip_package(p, p + 2, limit);	/* a field list holds names of field units */
				break;
			default:
				break;
			}
			break;
		case AML_NAME_OP:
			nb = parse_name(p + 1, limit, &n);
			if (nb) {
				defined(&sc, &n, p + 1 + nb, limit);
				step = 1u + nb;
			}
			break;
		case AML_METHOD_OP: {
			size_t lb = parse_pkg_length(p + 1, limit, &length);
			if (lb && length >= lb && (size_t)(limit - (p + 1)) >= length) {
				nb = parse_name(p + 1 + lb, p + 1 + length, &n);
				if (nb) defined(&sc, &n, p + 1 + lb + nb, p + 1 + length);
				step = 1u + length;	/* the body is code, not definitions */
			}
			break;
		}
		case AML_EXTERNAL_OP:
			nb = parse_name(p + 1, limit, &n);
			if (nb && (size_t)(limit - (p + 1 + nb)) >= AML_EXTERNAL_OPERANDS) step = 1u + nb + AML_EXTERNAL_OPERANDS;
			break;
		case AML_ALIAS_OP:
			nb = parse_name(p + 1, limit, &n);
			if (nb) {
				size_t nb2 = parse_name(p + 1 + nb, limit, &n);
				if (nb2) step = 1u + nb + nb2;
			}
			break;
		case AML_BUFFER_OP:
		case AML_PACKAGE_OP:
		case AML_VAR_PACKAGE_OP:
			/* Data: no name is defined inside, and the element count or a byte of data could look like an
			 * opcode. */
			step = skip_package(p, p + 1, limit);
			break;
		case AML_STRING_PREFIX: {
			const unsigned char *s = p + 1;
			while (s < limit && *s) s++;
			if (s < limit) step = (size_t)(s + 1 - p);
			break;
		}
		case AML_BYTE_PREFIX: step = 2; break;
		case AML_WORD_PREFIX: step = 3; break;
		case AML_DWORD_PREFIX: step = 5; break;
		case AML_QWORD_PREFIX: step = 9; break;
		default:
			break;
		}
		if (step == 0 || step > (size_t)(limit - p)) step = 1;
		p += step;
	}
	return 1;
}
