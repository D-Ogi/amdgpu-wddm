/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The load options of the BC-250 stage: tokens of the form name=value, separated by spaces. The core parses them
 * once. Each module asks for its own names, and a name that no module asked for is reported on the console. Pure
 * code, tested on the host.
 *
 * Every module has one off switch, its name with the value 0: "unlock=0", "acpi=0", "sb=0".
 */
#ifndef STAGE_OPTIONS_H
#define STAGE_OPTIONS_H
#include <stddef.h>

#define STAGE_MAX_OPTIONS	24u
#define STAGE_OPTION_CHARS	64u

struct stage_option {
	char name[STAGE_OPTION_CHARS];
	char value[STAGE_OPTION_CHARS];
	int claimed;			/* a module read this token and understood its value */
};

struct stage_options {
	unsigned count;
	unsigned dropped;		/* tokens that are too long, not ASCII, or past STAGE_MAX_OPTIONS */
	struct stage_option option[STAGE_MAX_OPTIONS];
};

/* Parses UTF-16 text of at most chars characters, up to its first zero. A token without "=" (the image name that
 * a shell passes first) is skipped. */
void stage_options_parse(const unsigned short *text, size_t chars, struct stage_options *out);

/* The index in choices of the value of the last token with this name, or fallback when there is no such token or
 * its value is not one of the choices. A token whose value is a choice is marked as claimed. */
unsigned stage_option_choice(struct stage_options *options, const char *name, const char *const *choices,
			     unsigned count, unsigned fallback);

/* The value of the last token with this name as text, or NULL when there is no such token. The token is marked as
 * claimed: the caller says on its own when the value is not usable. */
const char *stage_option_text(struct stage_options *options, const char *name);

/* A switch with the values "0" and "1". */
int stage_option_flag(struct stage_options *options, const char *name, int fallback);

/* The tokens that no module claimed, plus the dropped ones. */
unsigned stage_options_unclaimed(const struct stage_options *options);

#endif
