/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* <string.h> of the stage C runtime (crt.h). Freestanding: the build gives -nostdlibinc, so this header is the only
 * one. */
#ifndef CRT_STRING_H
#define CRT_STRING_H
#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t count);
void *memmove(void *dst, const void *src, size_t count);
void *memset(void *dst, int value, size_t count);
int memcmp(const void *a, const void *b, size_t count);
void *memchr(const void *s, int c, size_t count);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t count);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t count);
char *strcat(char *dst, const char *src);
char *strncat(char *dst, const char *src, size_t count);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char *strpbrk(const char *s, const char *accept);
char *strtok_r(char *s, const char *delimiters, char **state);
char *strdup(const char *s);
char *strndup(const char *s, size_t max);
char *strerror(int code);
int strcoll(const char *a, const char *b);

#endif
