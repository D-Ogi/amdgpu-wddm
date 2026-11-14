/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The six functions that the stage image has already: tools/efi/lib/efl_libc.c defines memset, memcpy, memmove,
 * memcmp, strlen and strcmp. This file defines the same six and nothing else, so that a link with efl_libc.o takes
 * those and never this member of the archive, and a link without it (the host test) takes these.
 *
 * Word loops for the long copies: libtpms copies its NV image (128 KiB) at each NV store.
 */
#include <stdint.h>
#include <string.h>

void *memset(void *dst, int value, size_t count)
{
	unsigned char *d = dst;
	uint64_t word = (unsigned char)value * 0x0101010101010101ull;
	while (count && ((uintptr_t)d & 7)) {
		*d++ = (unsigned char)value;
		count--;
	}
	for (; count >= 8; count -= 8, d += 8) __builtin_memcpy(d, &word, 8);
	while (count--) *d++ = (unsigned char)value;
	return dst;
}

void *memcpy(void *dst, const void *src, size_t count)
{
	unsigned char *d = dst;
	const unsigned char *s = src;
	for (; count >= 8; count -= 8, d += 8, s += 8) {
		uint64_t word;
		__builtin_memcpy(&word, s, 8);
		__builtin_memcpy(d, &word, 8);
	}
	while (count--) *d++ = *s++;
	return dst;
}

void *memmove(void *dst, const void *src, size_t count)
{
	unsigned char *d = dst;
	const unsigned char *s = src;
	if (d == s || count == 0) return dst;
	if (d < s || d >= s + count) return memcpy(dst, src, count);
	d += count;
	s += count;
	while (count--) *--d = *--s;
	return dst;
}

int memcmp(const void *a, const void *b, size_t count)
{
	const unsigned char *x = a, *y = b;
	for (; count; count--, x++, y++)
		if (*x != *y) return *x < *y ? -1 : 1;
	return 0;
}

size_t strlen(const char *s)
{
	size_t n = 0;
	while (s[n]) n++;
	return n;
}

int strcmp(const char *a, const char *b)
{
	const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;
	for (; *x && *x == *y; x++, y++) {}
	return *x == *y ? 0 : (*x < *y ? -1 : 1);
}
