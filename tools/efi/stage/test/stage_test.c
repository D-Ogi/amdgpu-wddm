/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* Host tests of the core: the load options (stage_options.c) and the register BAR (stage_bar.c). */
#include "efl_test.h"
#include "stage_options.h"
#include "stage_bar.h"
#include "bc250_mmio_bar.h"

static void wide(unsigned short *out, const char *s)
{
	while ((*out++ = (unsigned char)*s++) != 0) {}
}

static void test_options(void)
{
	static const char *const modes[2] = { "audit", "enforce" };
	unsigned short text[512];
	struct stage_options o;
	char many[400];
	unsigned i;

	/* The image name has no "=" and is skipped. Each module claims its own names. */
	wide(text, "bc250-stage.efi unlock=0 sb.mode=enforce  acpi.idle=1 acpi.stubs=0");
	stage_options_parse(text, 512, &o);
	CHECK(o.count == 4 && o.dropped == 0);
	CHECK(stage_options_unclaimed(&o) == 4);
	CHECK(stage_option_flag(&o, "unlock", 1) == 0);
	CHECK(stage_option_flag(&o, "acpi", 1) == 1);			/* not given: the fallback */
	CHECK(stage_option_flag(&o, "acpi.idle", 0) == 1);
	CHECK(stage_option_choice(&o, "sb.mode", modes, 2, 0) == 1);
	CHECK(stage_option_flag(&o, "acpi.stubs", 1) == 0);
	CHECK(stage_options_unclaimed(&o) == 0);

	/* A value that is not a choice keeps the fallback and stays unclaimed. The last good token wins. */
	wide(text, "sb.mode=enforce sb.mode=audit sb.mode=strict sb=2");
	stage_options_parse(text, 512, &o);
	CHECK(stage_option_choice(&o, "sb.mode", modes, 2, 1) == 0);
	CHECK(stage_option_flag(&o, "sb", 1) == 1);
	CHECK(stage_options_unclaimed(&o) == 2);

	/* A name is matched whole, and its case matters. */
	wide(text, "SB=0 sbx=0 s=0");
	stage_options_parse(text, 512, &o);
	CHECK(stage_option_flag(&o, "sb", 1) == 1 && stage_options_unclaimed(&o) == 3);

	/* Tokens that cannot be options are dropped and counted: no name, no value, too long, not ASCII. */
	wide(text, "=1 name= a-name-of-more-than-sixty-three-characters-is-too-long-for-an-option=1 ok=1");
	text[2] = 0x142;
	stage_options_parse(text, 512, &o);
	CHECK(o.count == 1 && stage_option_flag(&o, "ok", 0) == 1);
	CHECK(o.dropped == 2 && stage_options_unclaimed(&o) == 2);

	/* A text value: the last token wins, and the token counts as claimed. */
	wide(text, "next=\\EFI\\OLD.EFI next=\\EFI\\BOOT\\BOOTX64.EFI other=1");
	stage_options_parse(text, 512, &o);
	CHECK(stage_option_text(&o, "next") != NULL && !strcmp(stage_option_text(&o, "next"), "\\EFI\\BOOT\\BOOTX64.EFI"));
	CHECK(stage_option_text(&o, "nex") == NULL && stage_options_unclaimed(&o) == 1);

	/* More tokens than the list holds. */
	many[0] = 0;
	for (i = 0; i < STAGE_MAX_OPTIONS + 3u; i++) snprintf(many + strlen(many), 16, "o%u=1 ", i);
	wide(text, many);
	stage_options_parse(text, 512, &o);
	CHECK(o.count == STAGE_MAX_OPTIONS && o.dropped == 3);

	/* The size ends the text. No text at all. */
	wide(text, "sb=0");
	stage_options_parse(text, 3, &o);
	CHECK(o.count == 0 && o.dropped == 1);
	stage_options_parse(NULL, 0, &o);
	CHECK(o.count == 0 && stage_options_unclaimed(&o) == 0 && stage_option_flag(&o, "sb", 1) == 1);
}

/* ---- the register BAR ---------------------------------------------------------------------------- */

static void put_le(unsigned char *p, unsigned long long v, unsigned bytes)
{
	unsigned i;
	for (i = 0; i < bytes; i++) p[i] = (unsigned char)(v >> (8u * i));
}

/* An address space descriptor of one of the four types at p, memory type. Returns its bytes. */
static size_t address_space(unsigned char *p, unsigned tag, unsigned long long minimum, unsigned long long length)
{
	unsigned width = tag == STAGE_BAR_TAG_WORD ? 2u : tag == STAGE_BAR_TAG_DWORD ? 4u : 8u;
	unsigned first = tag == STAGE_BAR_TAG_EXTENDED ? 8u : 6u;
	unsigned data = tag == STAGE_BAR_TAG_WORD ? 0x0Du : tag == STAGE_BAR_TAG_DWORD ? 0x17u : tag == STAGE_BAR_TAG_QWORD ? 0x2Bu : 0x35u;
	memset(p, 0, 3u + data);
	p[0] = (unsigned char)tag;
	put_le(p + 1, data, 2);
	p[3] = STAGE_BAR_SPACE_MEMORY;
	p[4] = 0x0C;
	p[5] = 0x01;
	put_le(p + first, 8u * width, width);
	put_le(p + first + width, minimum, width);
	put_le(p + first + 2u * width, minimum + length - 1u, width);
	put_le(p + first + 3u * width, 0, width);
	put_le(p + first + 4u * width, length, width);
	return 3u + data;
}

static void test_bar_parse(void)
{
	unsigned char b[256];
	struct stage_bar_list l;
	size_t at;

	/* Each address space type, a small item between them, and the end tag. */
	at = address_space(b, STAGE_BAR_TAG_QWORD, 0xFE800000ull, 0x80000ull);
	at += address_space(b + at, STAGE_BAR_TAG_DWORD, 0xC0000000ull, 0x10000000ull);
	b[at++] = 0x22; b[at++] = 0x01; b[at++] = 0x00;	/* an IRQ descriptor (small item, type 4, 2 bytes) */
	at += address_space(b + at, STAGE_BAR_TAG_WORD, 0xE000u, 0x100u);
	at += address_space(b + at, STAGE_BAR_TAG_EXTENDED, 0xF400000000ull, 0x200000000ull);
	b[at] = STAGE_BAR_TAG_END; b[at + 1] = 0;
	stage_bar_parse(b, at + 2u, &l);
	CHECK(l.complete && l.end_offset == at && l.count == 5u && l.seen == 5u);
	CHECK(l.resource[0].tag == STAGE_BAR_TAG_QWORD && l.resource[0].bytes == 46u && l.resource[0].address_space);
	CHECK(l.resource[0].minimum == 0xFE800000ull && l.resource[0].length == 0x80000ull);
	CHECK(l.resource[0].maximum == 0xFE87FFFFull && l.resource[0].granularity == 64u);
	CHECK(l.resource[0].general == 0x0Cu && l.resource[0].specific == 0x01u && l.resource[0].type == 0u);
	CHECK(l.resource[1].tag == STAGE_BAR_TAG_DWORD && l.resource[1].bytes == 26u && l.resource[1].length == 0x10000000ull);
	CHECK(l.resource[2].tag == 0x22u && l.resource[2].bytes == 3u && !l.resource[2].address_space);
	CHECK(l.resource[3].tag == STAGE_BAR_TAG_WORD && l.resource[3].minimum == 0xE000u && l.resource[3].length == 0x100u);
	CHECK(l.resource[4].tag == STAGE_BAR_TAG_EXTENDED && l.resource[4].bytes == 56u);
	CHECK(l.resource[4].minimum == 0xF400000000ull && l.resource[4].length == 0x200000000ull);

	/* No end tag inside the bound, a descriptor cut by the bound, a short descriptor, no list. */
	stage_bar_parse(b, 46u, &l);
	CHECK(!l.complete && l.count == 1u);
	stage_bar_parse(b, 45u, &l);
	CHECK(!l.complete && l.count == 0u);
	address_space(b, STAGE_BAR_TAG_QWORD, 0xFE800000ull, 0x80000ull);
	b[1] = 0x20;
	b[0x23] = STAGE_BAR_TAG_END;
	stage_bar_parse(b, sizeof(b), &l);
	CHECK(!l.complete && l.count == 0u);
	stage_bar_parse(NULL, 64u, &l);
	CHECK(!l.complete && l.count == 0u);

	/* An empty list. More descriptors than resource[] holds: the count stops, the parse goes on. */
	b[0] = STAGE_BAR_TAG_END; b[1] = 0;
	stage_bar_parse(b, 2u, &l);
	CHECK(l.complete && l.count == 0u && l.end_offset == 0u);
	for (at = 0; at < 10u * 3u; at += 3u) { b[at] = 0x22; b[at + 1] = 0; b[at + 2] = 0; }
	b[at] = STAGE_BAR_TAG_END; b[at + 1] = 0;
	stage_bar_parse(b, at + 2u, &l);
	CHECK(l.complete && l.count == STAGE_BAR_MAX_RESOURCES && l.seen == 10u);
}

/* The first 64 bytes of a configuration header with this command register and these six BAR dwords. */
static void header(unsigned char *c, unsigned command, const unsigned *bars)
{
	unsigned i;
	memset(c, 0, 64);
	put_le(c + 4, command, 2);
	for (i = 0; i < 6; i++) put_le(c + 16 + 4 * i, bars[i], 4);
}

static void test_bar_config(void)
{
	/* The BARs of unit A (lspci, evidence/linux/2026-09-22-E21-linux-reference-4). */
	static const unsigned unit_a[6] = { 0xC000000Cu, 0, 0xD000000Cu, 0, 0xE001u, 0xFE800000u };
	unsigned bars[6];
	unsigned char c[64];
	unsigned long long a = 1;

	header(c, 0x0406u, unit_a);
	CHECK(stage_bar_config_address(c, sizeof(c), &a) == STAGE_BAR_CONFIG_OK && a == 0xFE800000ull);
	header(c, 0x0405u, unit_a);
	CHECK(stage_bar_config_address(c, sizeof(c), &a) == STAGE_BAR_CONFIG_DECODE_OFF && a == 0);
	memcpy(bars, unit_a, sizeof(bars));
	bars[5] = 0xFE800001u;
	header(c, 0x0406u, bars);
	CHECK(stage_bar_config_address(c, sizeof(c), &a) == STAGE_BAR_CONFIG_NOT_MEMORY);
	bars[5] = 0xFE800004u;
	header(c, 0x0406u, bars);
	CHECK(stage_bar_config_address(c, sizeof(c), &a) == STAGE_BAR_CONFIG_NOT_MEMORY);
	bars[5] = 0;
	header(c, 0x0406u, bars);
	CHECK(stage_bar_config_address(c, sizeof(c), &a) == STAGE_BAR_CONFIG_EMPTY);
	bars[4] = 0xFE80000Cu; bars[5] = 0;
	header(c, 0x0406u, bars);
	CHECK(stage_bar_config_address(c, sizeof(c), &a) == STAGE_BAR_CONFIG_UPPER_HALF);
	CHECK(stage_bar_config_address(c, 32u, &a) == STAGE_BAR_CONFIG_EMPTY);
	CHECK(stage_bar_config_address(NULL, 64u, &a) == STAGE_BAR_CONFIG_EMPTY);
	CHECK(strcmp(stage_bar_config_text(STAGE_BAR_CONFIG_COUNT), "unknown result") == 0);
}

static void one_descriptor(struct stage_bar_list *l, unsigned long long minimum, unsigned long long length)
{
	unsigned char b[64];
	size_t at = address_space(b, STAGE_BAR_TAG_QWORD, minimum, length ? length : 1u);
	put_le(b + 38, length, 8);
	b[at] = STAGE_BAR_TAG_END; b[at + 1] = 0;
	stage_bar_parse(b, at + 2u, l);
}

static void test_bar_match(void)
{
	struct stage_bar_list l;
	/* The descriptor of kit r2 step s5a: length 0 at 0x300000. */
	one_descriptor(&l, 0x300000ull, 0);
	CHECK(stage_bar_match(&l, 0xFE800000ull, BC250_BAR5_LENGTH) == STAGE_BAR_MATCH_NONE);
	/* Length 0 at the address of BAR5 says nothing. */
	one_descriptor(&l, 0xFE800000ull, 0);
	CHECK(stage_bar_match(&l, 0xFE800000ull, BC250_BAR5_LENGTH) == STAGE_BAR_MATCH_NONE);
	/* Another address with the right length. */
	one_descriptor(&l, 0xFE900000ull, BC250_BAR5_LENGTH);
	CHECK(stage_bar_match(&l, 0xFE800000ull, BC250_BAR5_LENGTH) == STAGE_BAR_MATCH_NONE);
	/* The right case. */
	one_descriptor(&l, 0xFE800000ull, BC250_BAR5_LENGTH);
	CHECK(stage_bar_match(&l, 0xFE800000ull, BC250_BAR5_LENGTH) == STAGE_BAR_MATCH_EXACT);
	/* The address with another length. */
	one_descriptor(&l, 0xFE800000ull, 0x40000ull);
	CHECK(stage_bar_match(&l, 0xFE800000ull, BC250_BAR5_LENGTH) == STAGE_BAR_MATCH_CONFLICT);
	/* An I/O descriptor at the address does not count. */
	one_descriptor(&l, 0xFE800000ull, BC250_BAR5_LENGTH);
	l.resource[0].type = 1u;
	CHECK(stage_bar_match(&l, 0xFE800000ull, BC250_BAR5_LENGTH) == STAGE_BAR_MATCH_NONE);

	/* The windows of a root bridge. */
	one_descriptor(&l, 0xC0000000ull, 0x3F000000ull);
	CHECK(stage_bar_window_holds(&l, 0xFE800000ull, BC250_BAR5_LENGTH));
	CHECK(stage_bar_window_holds(&l, 0xFEF80000ull, BC250_BAR5_LENGTH));
	CHECK(!stage_bar_window_holds(&l, 0xFEF80001ull, BC250_BAR5_LENGTH));
	CHECK(!stage_bar_window_holds(&l, 0xBFFFF000ull, BC250_BAR5_LENGTH));
	CHECK(!stage_bar_window_holds(&l, 0xFE800000ull, 0));
	CHECK(!stage_bar_window_holds(&l, ~0ull, BC250_BAR5_LENGTH));
	one_descriptor(&l, 0xC0000000ull, 0);
	CHECK(!stage_bar_window_holds(&l, 0xFE800000ull, BC250_BAR5_LENGTH));
}

static void test_bar_choose(void)
{
	struct stage_bar_findings f;
	struct stage_bar_choice c;
	memset(&f, 0, sizeof(f));
	f.config = STAGE_BAR_CONFIG_OK;
	f.address = 0xFE800000ull;

	/* One BarIndex with the exact descriptor. */
	f.index[3] = STAGE_BAR_MATCH_EXACT;
	c = stage_bar_choose(&f);
	CHECK(c.path == STAGE_BAR_PATH_PCI_IO && c.index == 3u && c.address == 0xFE800000ull && c.reason == STAGE_BAR_WHY_INDEX);
	/* Two: refused. */
	f.index[5] = STAGE_BAR_MATCH_EXACT;
	c = stage_bar_choose(&f);
	CHECK(c.path == STAGE_BAR_PATH_NONE && c.reason == STAGE_BAR_WHY_AMBIGUOUS);
	/* A conflict refuses even with an exact one and a root bridge. */
	f.index[5] = STAGE_BAR_MATCH_CONFLICT;
	f.bridges = 1u; f.bridges_holding = 1u; f.bridges_described = 1u;
	c = stage_bar_choose(&f);
	CHECK(c.path == STAGE_BAR_PATH_NONE && c.reason == STAGE_BAR_WHY_CONFLICT);

	/* No descriptor: the root bridge that holds the address. */
	memset(f.index, 0, sizeof(f.index));
	f.bridges = 2u; f.bridges_holding = 1u; f.bridges_described = 2u; f.bridge = 1u;
	c = stage_bar_choose(&f);
	CHECK(c.path == STAGE_BAR_PATH_BRIDGE && c.bridge == 1u && c.reason == STAGE_BAR_WHY_BRIDGE);
	/* Two bridges hold it, or two bridges and none describes itself: refused. */
	f.bridges_holding = 2u;
	CHECK(stage_bar_choose(&f).reason == STAGE_BAR_WHY_NO_BRIDGE);
	f.bridges_holding = 0u; f.bridges_described = 0u;
	CHECK(stage_bar_choose(&f).reason == STAGE_BAR_WHY_NO_BRIDGE);
	/* The only bridge of the segment, without a configuration: it counts. With a complete list that does not
	 * hold the address: it does not. */
	f.bridges = 1u; f.bridge = 0u;
	CHECK(stage_bar_choose(&f).path == STAGE_BAR_PATH_BRIDGE);
	f.bridges_described = 1u;
	CHECK(stage_bar_choose(&f).path == STAGE_BAR_PATH_NONE);
	/* No bridge. */
	f.bridges = 0u; f.bridges_described = 0u;
	CHECK(stage_bar_choose(&f).reason == STAGE_BAR_WHY_NO_BRIDGE);
	/* No register BAR in the header: refused before the descriptors count. */
	f.index[3] = STAGE_BAR_MATCH_EXACT;
	f.config = STAGE_BAR_CONFIG_DECODE_OFF;
	c = stage_bar_choose(&f);
	CHECK(c.path == STAGE_BAR_PATH_NONE && c.reason == STAGE_BAR_WHY_CONFIG);
	CHECK(strcmp(stage_bar_reason_text(STAGE_BAR_WHY_COUNT), "unknown reason") == 0);
}

struct control_model { unsigned value; int fail; unsigned reads, offset; };

static int control_read(void *context, unsigned offset, unsigned *value)
{
	struct control_model *m = context;
	m->reads++;
	m->offset = offset;
	if (m->fail) return -1;
	*value = m->value;
	return 0;
}

static void test_bar_control(void)
{
	struct control_model m;
	unsigned v = 1;
	/* The offset is SOC15_REG_OFFSET(GC, 0, mmGB_ADDR_CONFIG); regcalc gives BAR5+0x098F8 for mmGB_ADDR_CONFIG. */
	CHECK(stage_bar_control_offset() == 0x098F8u);
	memset(&m, 0, sizeof(m));
	m.value = STAGE_BAR_CONTROL_VALUE;
	CHECK(stage_bar_control_read(control_read, &m, &v) == STAGE_BAR_CONTROL_OK && v == STAGE_BAR_CONTROL_VALUE);
	CHECK(m.reads == 1u && m.offset == stage_bar_control_offset());
	/* The golden value of amdgpu is not the value of this unit (fact M5). */
	m.value = 0x00100044u;
	CHECK(stage_bar_control_read(control_read, &m, &v) == STAGE_BAR_CONTROL_MISMATCH && v == 0x00100044u);
	m.value = 0xFFFFFFFFu;
	CHECK(stage_bar_control_read(control_read, &m, &v) == STAGE_BAR_CONTROL_MISMATCH);
	m.fail = 1;
	CHECK(stage_bar_control_read(control_read, &m, &v) == STAGE_BAR_CONTROL_READ_FAILED && v == 0);
	CHECK(strcmp(stage_bar_control_text(STAGE_BAR_CONTROL_COUNT), "unknown result") == 0);
}

unsigned stage_tests(unsigned *check_count)
{
	test_options();
	test_bar_parse();
	test_bar_config();
	test_bar_match();
	test_bar_choose();
	test_bar_control();
	*check_count = checks;
	return failures;
}
