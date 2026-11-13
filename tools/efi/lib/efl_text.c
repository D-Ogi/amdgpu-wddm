/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* Device paths and the console formatter (efl_text.h). */
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include "efl_text.h"

#define DP_HEADER_BYTES		4u
#define DP_TYPE_MEDIA		0x04u
#define DP_MEDIA_FILEPATH	0x04u
#define DP_TYPE_END		0x7Fu
#define DP_END_ENTIRE		0xFFu

/* ---- device paths ------------------------------------------------------------------------------ */

static size_t node_bytes(const unsigned char *node) { return (size_t)node[2] | ((size_t)node[3] << 8); }

size_t efl_devpath_bytes(const unsigned char *path, size_t max_bytes)
{
	size_t at = 0;
	if (path == NULL) return 0;
	while (at + DP_HEADER_BYTES <= max_bytes) {
		size_t bytes = node_bytes(path + at);
		if (bytes < DP_HEADER_BYTES || at + bytes > max_bytes) return 0;
		if (path[at] == DP_TYPE_END) return at + bytes;
		at += bytes;
	}
	return 0;
}

int efl_devpath_file(const unsigned char *path, size_t max_bytes, unsigned short *out, size_t capacity)
{
	size_t at = 0, used = 0, total = efl_devpath_bytes(path, max_bytes);
	if (total == 0 || out == NULL || capacity == 0) return 0;
	out[0] = 0;
	while (at < total && path[at] != DP_TYPE_END) {
		size_t bytes = node_bytes(path + at);
		if (path[at] == DP_TYPE_MEDIA && path[at + 1] == DP_MEDIA_FILEPATH) {
			const unsigned char *s = path + at + DP_HEADER_BYTES;
			size_t chars = (bytes - DP_HEADER_BYTES) / 2u, i;
			for (i = 0; i < chars; i++) {
				unsigned short c = (unsigned short)(s[2u * i] | (s[2u * i + 1u] << 8));
				if (c == 0) break;
				/* One backslash between two nodes, and none doubled. */
				if (i == 0 && used > 0 && out[used - 1] != '\\' && c != '\\') {
					if (used + 1 >= capacity) return 0;
					out[used++] = '\\';
				}
				if (i == 0 && used > 0 && out[used - 1] == '\\' && c == '\\') continue;
				if (used + 1 >= capacity) return 0;
				out[used++] = c;
			}
		}
		at += bytes;
	}
	out[used] = 0;
	return used > 0;
}

int efl_sibling_path(const unsigned short *image_file, const char *name, unsigned short *out, size_t capacity)
{
	size_t i, slash = 0, used, chars;
	int found = 0;
	if (image_file == NULL || name == NULL || out == NULL) return 0;
	chars = strlen(name);
	for (i = 0; image_file[i]; i++)
		if (image_file[i] == '\\') { slash = i; found = 1; }
	if (!found) return 0;
	used = slash + 1u;
	if (used + chars + 1u > capacity) return 0;
	for (i = 0; i < used; i++) out[i] = image_file[i];
	for (i = 0; i < chars; i++) out[used + i] = (unsigned char)name[i];
	out[used + chars] = 0;
	return 1;
}

size_t efl_devpath_with_file(const unsigned char *device, size_t max_bytes, const unsigned short *file,
			    unsigned char *out, size_t capacity)
{
	size_t total = efl_devpath_bytes(device, max_bytes), prefix, chars = 0, node, i;
	if (total == 0 || file == NULL || out == NULL) return 0;
	/* The prefix is every node before the first end node, which is the last node efl_devpath_bytes counted. */
	prefix = 0;
	while (device[prefix] != DP_TYPE_END) prefix += node_bytes(device + prefix);
	while (file[chars]) chars++;
	node = DP_HEADER_BYTES + 2u * (chars + 1u);
	if (node > 0xFFFFu || prefix + node + DP_HEADER_BYTES > capacity) return 0;
	memcpy(out, device, prefix);
	out[prefix] = DP_TYPE_MEDIA;
	out[prefix + 1] = DP_MEDIA_FILEPATH;
	out[prefix + 2] = (unsigned char)node;
	out[prefix + 3] = (unsigned char)(node >> 8);
	for (i = 0; i <= chars; i++) {
		out[prefix + DP_HEADER_BYTES + 2u * i] = (unsigned char)file[i];
		out[prefix + DP_HEADER_BYTES + 2u * i + 1u] = (unsigned char)(file[i] >> 8);
	}
	out += prefix + node;
	out[0] = DP_TYPE_END;
	out[1] = DP_END_ENTIRE;
	out[2] = DP_HEADER_BYTES;
	out[3] = 0;
	return prefix + node + DP_HEADER_BYTES;
}

/* ---- the formatter ------------------------------------------------------------------------------ */

struct sink { char *out; size_t capacity, used; };

static void put(struct sink *s, char c)
{
	if (s->used + 1u < s->capacity) s->out[s->used++] = c;
}

static void put_number(struct sink *s, unsigned long long value, unsigned base)
{
	char digits[24];
	int n = 0;
	do {
		unsigned d = (unsigned)(value % base);
		digits[n++] = (char)(d < 10u ? '0' + d : 'A' + d - 10u);
		value /= base;
	} while (value && n < (int)sizeof(digits));
	while (n) put(s, digits[--n]);
}

size_t efl_vformat(char *out, size_t capacity, const char *format, va_list args)
{
	struct sink s;
	s.out = out;
	s.capacity = capacity;
	s.used = 0;
	if (out == NULL || capacity == 0) return 0;
	for (; *format; format++) {
		int wide = 0;
		if (*format != '%') { put(&s, *format); continue; }
		format++;
		if (format[0] == 'l' && format[1] == 'l') { wide = 1; format += 2; }
		switch (*format) {
		case 's': {
			const char *t = va_arg(args, const char *);
			if (t == NULL) t = "(null)";
			while (*t) put(&s, *t++);
			break;
		}
		case 'd': {
			int v = va_arg(args, int);
			if (v < 0) { put(&s, '-'); put_number(&s, (unsigned long long)(-(long long)v), 10u); }
			else put_number(&s, (unsigned long long)v, 10u);
			break;
		}
		case 'u':
			put_number(&s, wide ? va_arg(args, unsigned long long) : va_arg(args, unsigned), 10u);
			break;
		case 'x':
			put_number(&s, wide ? va_arg(args, unsigned long long) : va_arg(args, unsigned), 16u);
			break;
		case '%':
			put(&s, '%');
			break;
		default:
			/* An unknown conversion is printed as it stands, and the format ends at a lone '%'. */
			put(&s, '%');
			if (*format == 0) { out[s.used] = 0; return s.used; }
			put(&s, *format);
			break;
		}
	}
	out[s.used] = 0;
	return s.used;
}

size_t efl_format(char *out, size_t capacity, const char *format, ...)
{
	va_list args;
	size_t n;
	va_start(args, format);
	n = efl_vformat(out, capacity, format, args);
	va_end(args);
	return n;
}
