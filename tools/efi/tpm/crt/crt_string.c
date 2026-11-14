/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The functions of <string.h> and <strings.h> of the stage C runtime, except the six of crt_mem.c. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void *memchr(const void *s, int c, size_t count)
{
	const unsigned char *p = s;
	for (; count; count--, p++)
		if (*p == (unsigned char)c) return (void *)p;
	return NULL;
}

size_t strnlen(const char *s, size_t max)
{
	size_t n = 0;
	while (n < max && s[n]) n++;
	return n;
}

int strncmp(const char *a, const char *b, size_t count)
{
	const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;
	for (; count; count--, x++, y++) {
		if (*x != *y) return *x < *y ? -1 : 1;
		if (!*x) return 0;
	}
	return 0;
}

int strcoll(const char *a, const char *b) { return strcmp(a, b); }

char *strcpy(char *dst, const char *src)
{
	char *d = dst;
	while ((*d++ = *src++)) {}
	return dst;
}

char *strncpy(char *dst, const char *src, size_t count)
{
	size_t i = 0;
	for (; i < count && src[i]; i++) dst[i] = src[i];
	for (; i < count; i++) dst[i] = 0;
	return dst;
}

char *strcat(char *dst, const char *src)
{
	strcpy(dst + strlen(dst), src);
	return dst;
}

char *strncat(char *dst, const char *src, size_t count)
{
	char *d = dst + strlen(dst);
	while (count-- && *src) *d++ = *src++;
	*d = 0;
	return dst;
}

char *strchr(const char *s, int c)
{
	for (;; s++) {
		if (*s == (char)c) return (char *)s;
		if (!*s) return NULL;
	}
}

char *strrchr(const char *s, int c)
{
	const char *last = NULL;
	for (;; s++) {
		if (*s == (char)c) last = s;
		if (!*s) return (char *)last;
	}
}

char *strstr(const char *haystack, const char *needle)
{
	size_t n = strlen(needle);
	if (!n) return (char *)haystack;
	for (; *haystack; haystack++)
		if (*haystack == *needle && !strncmp(haystack, needle, n)) return (char *)haystack;
	return NULL;
}

size_t strspn(const char *s, const char *accept)
{
	size_t n = 0;
	while (s[n] && strchr(accept, s[n])) n++;
	return n;
}

size_t strcspn(const char *s, const char *reject)
{
	size_t n = 0;
	while (s[n] && !strchr(reject, s[n])) n++;
	return n;
}

char *strpbrk(const char *s, const char *accept)
{
	s += strcspn(s, accept);
	return *s ? (char *)s : NULL;
}

char *strtok_r(char *s, const char *delimiters, char **state)
{
	char *end;
	if (!s) s = *state;
	s += strspn(s, delimiters);
	if (!*s) {
		*state = s;
		return NULL;
	}
	end = s + strcspn(s, delimiters);
	if (*end) *end++ = 0;
	*state = end;
	return s;
}

char *strdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *copy = malloc(n);
	if (copy) memcpy(copy, s, n);
	return copy;
}

char *strndup(const char *s, size_t max)
{
	size_t n = strnlen(s, max);
	char *copy = malloc(n + 1);
	if (copy) {
		memcpy(copy, s, n);
		copy[n] = 0;
	}
	return copy;
}

/* One static buffer: the runtime has no threads. */
char *strerror(int code)
{
	static char text[24];
	snprintf(text, sizeof(text), "error %d", code);
	return text;
}

int strcasecmp(const char *a, const char *b)
{
	int x, y;
	do {
		x = tolower((unsigned char)*a++);
		y = tolower((unsigned char)*b++);
	} while (x && x == y);
	return x - y;
}

int strncasecmp(const char *a, const char *b, size_t count)
{
	int x = 0, y = 0;
	while (count--) {
		x = tolower((unsigned char)*a++);
		y = tolower((unsigned char)*b++);
		if (!x || x != y) break;
	}
	return x - y;
}
