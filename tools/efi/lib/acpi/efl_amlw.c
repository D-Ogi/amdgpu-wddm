/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The AML writer (efl_amlw.h). Each opcode and prefix is named once below, from the ACPI Specification 6.5
 * section 20.2. */
#include <stddef.h>
#include <string.h>
#include "efl_amlw.h"

#define AML_ZERO_OP		0x00u
#define AML_ONE_OP		0x01u
#define AML_NAME_OP		0x08u
#define AML_BYTE_PREFIX		0x0Au
#define AML_WORD_PREFIX		0x0Bu
#define AML_DWORD_PREFIX	0x0Cu
#define AML_QWORD_PREFIX	0x0Eu
#define AML_SCOPE_OP		0x10u
#define AML_PACKAGE_OP		0x12u
#define AML_METHOD_OP		0x14u
#define AML_DUAL_NAME_PREFIX	0x2Eu
#define AML_MULTI_NAME_PREFIX	0x2Fu
#define AML_ROOT_CHAR		0x5Cu
#define AML_EXT_OP		0x5Bu	/* the extended opcode prefix of ACPI 6.5 section 20.2.5 */
#define AML_DEVICE_OP		0x82u
#define AML_STRING_PREFIX	0x0Du
#define AML_BUFFER_OP		0x11u
#define AML_IF_OP		0xA0u
#define AML_RETURN_OP		0xA4u
#define AML_LEQUAL_OP		0x93u
#define AML_ARG0_OP		0x68u

#define SDT_HEADER_BYTES	36u
#define SDT_LENGTH		4u
#define SDT_CHECKSUM		9u
#define NAME_SEG		4u
#define PKG_LENGTH_MAX_BYTES	4u
#define PKG_LENGTH_MAX		0x0FFFFFFFu

void efl_amlw_init(struct efl_amlw *w, unsigned char *out, size_t capacity)
{
	w->out = out;
	w->capacity = capacity;
	w->used = 0;
	w->overflow = out == NULL;
}

void efl_amlw_bytes(struct efl_amlw *w, const void *bytes, size_t count)
{
	if (w->overflow || count > w->capacity - w->used) {
		w->overflow = 1;
		return;
	}
	memcpy(w->out + w->used, bytes, count);
	w->used += count;
}

static void put8(struct efl_amlw *w, unsigned value)
{
	unsigned char b = (unsigned char)value;
	efl_amlw_bytes(w, &b, 1);
}

static void put_le(struct efl_amlw *w, unsigned long long value, unsigned bytes)
{
	unsigned i;
	for (i = 0; i < bytes; i++) put8(w, (unsigned)(value >> (8u * i)) & 0xFFu);
}

static void put_padded(struct efl_amlw *w, const char *text, size_t width)
{
	size_t i, n = strlen(text);
	for (i = 0; i < width; i++) put8(w, i < n ? (unsigned char)text[i] : ' ');
}

void efl_amlw_table_begin(struct efl_amlw *w, const char *signature, unsigned revision, const char *oem_id,
			  const char *oem_table_id, unsigned oem_revision, const char *creator_id,
			  unsigned creator_revision)
{
	put_padded(w, signature, 4);
	put_le(w, 0, 4);			/* the length, set at the end */
	put8(w, revision);
	put8(w, 0);				/* the checksum, set at the end */
	put_padded(w, oem_id, 6);
	put_padded(w, oem_table_id, 8);
	put_le(w, oem_revision, 4);
	put_padded(w, creator_id, 4);
	put_le(w, creator_revision, 4);
}

size_t efl_amlw_table_end(struct efl_amlw *w)
{
	unsigned char sum = 0;
	size_t i;
	if (w->overflow || w->used < SDT_HEADER_BYTES || w->used > 0xFFFFFFFFu) return 0;
	for (i = 0; i < 4u; i++) w->out[SDT_LENGTH + i] = (unsigned char)(w->used >> (8u * i));
	w->out[SDT_CHECKSUM] = 0;
	for (i = 0; i < w->used; i++) sum = (unsigned char)(sum + w->out[i]);
	w->out[SDT_CHECKSUM] = (unsigned char)(0x100u - sum);
	return w->used;
}

static int lead_char(char c) { return (c >= 'A' && c <= 'Z') || c == '_'; }
static int name_char(char c) { return lead_char(c) || (c >= '0' && c <= '9'); }

static int segment_ok(const char *s)
{
	return lead_char(s[0]) && name_char(s[1]) && name_char(s[2]) && name_char(s[3]);
}

void efl_amlw_path(struct efl_amlw *w, const char *path)
{
	size_t n = strlen(path), segments, i;
	/* "\" and then segments of 4 characters with one period between two of them. */
	if (n < 1u + NAME_SEG || path[0] != '\\' || (n - 1u + 1u) % (NAME_SEG + 1u) != 0) {
		w->overflow = 1;
		return;
	}
	segments = (n - 1u + 1u) / (NAME_SEG + 1u);
	for (i = 0; i < segments; i++) {
		const char *s = path + 1u + i * (NAME_SEG + 1u);
		if (!segment_ok(s) || (i + 1u < segments && s[NAME_SEG] != '.')) {
			w->overflow = 1;
			return;
		}
	}
	if (segments > 255u) {
		w->overflow = 1;
		return;
	}
	put8(w, AML_ROOT_CHAR);
	if (segments == 2u) put8(w, AML_DUAL_NAME_PREFIX);
	else if (segments > 2u) {
		put8(w, AML_MULTI_NAME_PREFIX);
		put8(w, (unsigned)segments);
	}
	for (i = 0; i < segments; i++) efl_amlw_bytes(w, path + 1u + i * (NAME_SEG + 1u), NAME_SEG);
}

void efl_amlw_integer(struct efl_amlw *w, unsigned long long value)
{
	if (value == 0) put8(w, AML_ZERO_OP);
	else if (value == 1u) put8(w, AML_ONE_OP);
	else if (value <= 0xFFu) { put8(w, AML_BYTE_PREFIX); put_le(w, value, 1); }
	else if (value <= 0xFFFFu) { put8(w, AML_WORD_PREFIX); put_le(w, value, 2); }
	else if (value <= 0xFFFFFFFFu) { put8(w, AML_DWORD_PREFIX); put_le(w, value, 4); }
	else { put8(w, AML_QWORD_PREFIX); put_le(w, value, 8); }
}

size_t efl_amlw_scope_begin(struct efl_amlw *w, const char *path)
{
	size_t mark;
	put8(w, AML_SCOPE_OP);
	mark = w->used;
	efl_amlw_path(w, path);
	return mark;
}

size_t efl_amlw_package_begin(struct efl_amlw *w, unsigned elements)
{
	size_t mark;
	if (elements > 255u) w->overflow = 1;
	put8(w, AML_PACKAGE_OP);
	mark = w->used;
	put8(w, elements);
	return mark;
}

void efl_amlw_end(struct efl_amlw *w, size_t mark)
{
	size_t body, own, total;
	unsigned char encoded[PKG_LENGTH_MAX_BYTES];
	if (w->overflow || mark > w->used) {
		w->overflow = 1;
		return;
	}
	body = w->used - mark;
	/* The package length counts its own bytes. One byte holds 6 bits, and each further byte adds 8 bits to the
	 * 4 bits that the first byte then holds. */
	if (body + 1u <= 0x3Fu) own = 1;
	else if (body + 2u <= 0xFFFu) own = 2;
	else if (body + 3u <= 0xFFFFFu) own = 3;
	else own = 4;
	total = body + own;
	if (total > PKG_LENGTH_MAX || own > w->capacity - w->used) {
		w->overflow = 1;
		return;
	}
	if (own == 1u) encoded[0] = (unsigned char)total;
	else {
		size_t i;
		encoded[0] = (unsigned char)(((own - 1u) << 6) | (total & 0x0Fu));
		for (i = 1; i < own; i++) encoded[i] = (unsigned char)(total >> (4u + 8u * (i - 1u)));
	}
	memmove(w->out + mark + own, w->out + mark, body);
	memcpy(w->out + mark, encoded, own);
	w->used += own;
}

void efl_amlw_name(struct efl_amlw *w, const char *segment)
{
	if (strlen(segment) != NAME_SEG || !segment_ok(segment)) {
		w->overflow = 1;
		return;
	}
	put8(w, AML_NAME_OP);
	efl_amlw_bytes(w, segment, NAME_SEG);
}

size_t efl_amlw_device_begin(struct efl_amlw *w, const char *segment)
{
	size_t mark;
	if (strlen(segment) != NAME_SEG || !segment_ok(segment)) {
		w->overflow = 1;
		return 0;
	}
	put8(w, AML_EXT_OP);
	put8(w, AML_DEVICE_OP);
	mark = w->used;
	efl_amlw_bytes(w, segment, NAME_SEG);
	return mark;
}

void efl_amlw_string(struct efl_amlw *w, const char *text)
{
	size_t n = strlen(text);
	if (n > 0x7FFFu) { w->overflow = 1; return; }
	put8(w, AML_STRING_PREFIX);
	efl_amlw_bytes(w, text, n + 1);
}

void efl_amlw_buffer(struct efl_amlw *w, const void *bytes, size_t count)
{
	if (count > 0x7FFFu) { w->overflow = 1; return; }
	put8(w, AML_BUFFER_OP);
	{
		size_t mark = w->used;
		efl_amlw_integer(w, (unsigned long long)count);
		efl_amlw_bytes(w, bytes, count);
		efl_amlw_end(w, mark);
	}
}

void efl_amlw_name_string(struct efl_amlw *w, const char *segment, const char *text)
{
	if (strlen(segment) != NAME_SEG || !segment_ok(segment)) {
		w->overflow = 1;
		return;
	}
	put8(w, AML_NAME_OP);
	efl_amlw_bytes(w, segment, NAME_SEG);
	efl_amlw_string(w, text);
}

size_t efl_amlw_method_begin(struct efl_amlw *w, const char *segment, unsigned arguments, int serialized)
{
	size_t mark;
	if (strlen(segment) != NAME_SEG || !segment_ok(segment) || arguments > 7u) {
		w->overflow = 1;
		return 0;
	}
	put8(w, AML_METHOD_OP);
	mark = w->used;
	efl_amlw_bytes(w, segment, NAME_SEG);
	put8(w, (unsigned char)(arguments | (serialized ? 0x08u : 0u)));
	return mark;
}

size_t efl_amlw_if_begin(struct efl_amlw *w)
{
	size_t mark;
	put8(w, AML_IF_OP);
	mark = w->used;
	return mark;
}

void efl_amlw_return(struct efl_amlw *w)
{
	put8(w, AML_RETURN_OP);
}

void efl_amlw_arg(struct efl_amlw *w, unsigned index)
{
	if (index > 6u) { w->overflow = 1; return; }
	put8(w, (unsigned char)(AML_ARG0_OP + index));
}

void efl_amlw_lequal(struct efl_amlw *w)
{
	put8(w, AML_LEQUAL_OP);
}

void efl_amlw_empty_method(struct efl_amlw *w, const char *segment, unsigned arguments)
{
	size_t mark;
	if (strlen(segment) != NAME_SEG || !segment_ok(segment) || arguments > 7u) {
		w->overflow = 1;
		return;
	}
	put8(w, AML_METHOD_OP);
	mark = w->used;
	efl_amlw_bytes(w, segment, NAME_SEG);
	put8(w, arguments);	/* MethodFlags: bits 2:0 the argument count, bit 3 clear for NotSerialized */
	efl_amlw_end(w, mark);
}
