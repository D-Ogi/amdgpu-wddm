/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The C library functions of include/libc/string.h, for the UEFI builds only. The compiler may also call memset
 * and memcpy by itself, so these must exist even where no source names them. Byte loops: the largest buffer here
 * is one boot image of a few megabytes, copied or compared a few times in one start. */
#include <string.h>

void *memset(void *dst, int value, size_t count)
{
	volatile unsigned char *d = dst;
	while (count--) *d++ = (unsigned char)value;
	return dst;
}

void *memcpy(void *dst, const void *src, size_t count)
{
	volatile unsigned char *d = dst;
	const unsigned char *s = src;
	while (count--) *d++ = *s++;
	return dst;
}

void *memmove(void *dst, const void *src, size_t count)
{
	volatile unsigned char *d = dst;
	const unsigned char *s = src;
	if (d == s || count == 0) return dst;
	if (d < s) {
		while (count--) *d++ = *s++;
	} else {
		d += count;
		s += count;
		while (count--) *--d = *--s;
	}
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
