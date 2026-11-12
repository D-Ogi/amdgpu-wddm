/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The part of <string.h> that the shared sources use, for the freestanding UEFI build (src/cu_libc.c). The host
 * build of the tests uses the C library and never sees this directory. */
#ifndef CU_LIBC_STRING_H
#define CU_LIBC_STRING_H
#include <stddef.h>
void *memset(void *dst, int value, size_t count);
void *memcpy(void *dst, const void *src, size_t count);
void *memmove(void *dst, const void *src, size_t count);
int memcmp(const void *a, const void *b, size_t count);
size_t strlen(const char *s);
#endif
