/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* Host tests of the core's load options (stage_options.c). */
#include "efl_test.h"
#include "stage_options.h"

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

unsigned stage_tests(unsigned *check_count)
{
	test_options();
	*check_count = checks;
	return failures;
}
