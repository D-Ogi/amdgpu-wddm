/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The load options of the stage (stage_options.h). */
#include <stddef.h>
#include <string.h>
#include "stage_options.h"

/* Copies n UTF-16 characters as ASCII. 0 when a character is not printable ASCII or the text does not fit. */
static int copy_ascii(char *out, const unsigned short *text, size_t n)
{
	size_t i;
	if (n == 0 || n >= STAGE_OPTION_CHARS) return 0;
	for (i = 0; i < n; i++) {
		if (text[i] <= 0x20u || text[i] >= 0x7Fu) return 0;
		out[i] = (char)text[i];
	}
	out[n] = 0;
	return 1;
}

void stage_options_parse(const unsigned short *text, size_t chars, struct stage_options *out)
{
	size_t at = 0, end;
	memset(out, 0, sizeof(*out));
	if (text == NULL) return;
	for (end = 0; end < chars && text[end]; end++) {}
	while (at < end) {
		size_t start, eq = 0;
		struct stage_option option;
		while (at < end && text[at] == ' ') at++;
		start = at;
		while (at < end && text[at] != ' ') {
			if (text[at] == '=' && eq == 0 && at > start) eq = at;
			at++;
		}
		if (at == start || eq == 0) continue;
		memset(&option, 0, sizeof(option));
		if (out->count >= STAGE_MAX_OPTIONS || !copy_ascii(option.name, text + start, eq - start) ||
		    !copy_ascii(option.value, text + eq + 1u, at - eq - 1u)) {
			out->dropped++;
			continue;
		}
		out->option[out->count++] = option;
	}
}

unsigned stage_option_choice(struct stage_options *options, const char *name, const char *const *choices,
			     unsigned count, unsigned fallback)
{
	unsigned i, c, found = fallback;
	for (i = 0; i < options->count; i++) {
		struct stage_option *o = &options->option[i];
		if (strcmp(o->name, name) != 0) continue;
		for (c = 0; c < count; c++)
			if (!strcmp(o->value, choices[c])) {
				found = c;
				o->claimed = 1;
			}
	}
	return found;
}

const char *stage_option_text(struct stage_options *options, const char *name)
{
	const char *found = NULL;
	unsigned i;
	for (i = 0; i < options->count; i++)
		if (!strcmp(options->option[i].name, name)) {
			options->option[i].claimed = 1;
			found = options->option[i].value;
		}
	return found;
}

int stage_option_flag(struct stage_options *options, const char *name, int fallback)
{
	static const char *const values[2] = { "0", "1" };
	return (int)stage_option_choice(options, name, values, 2, fallback ? 1u : 0u);
}

unsigned stage_options_unclaimed(const struct stage_options *options)
{
	unsigned i, n = options->dropped;
	for (i = 0; i < options->count; i++)
		if (!options->option[i].claimed) n++;
	return n;
}
