/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The functions of <stdlib.h> of the stage C runtime that are not memory or the failure path. */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* The Microsoft x64 ABI: an object that uses floating point refers to _fltused, and the C library defines it. */
int _fltused;

/* The stage has no environment. */
char *getenv(const char *name)
{
	(void)name;
	return NULL;
}

char *secure_getenv(const char *name)
{
	(void)name;
	return NULL;
}

/*
 * rand() is not a source of random numbers here: it returns the same value at each call. libtpms reads rand()
 * only in the fall-back path of _plat__GetEntropy (TPMCmd/Platform/src/Entropy.c), after RAND_bytes of OpenSSL
 * failed. That path compares two outputs and reports an entropy failure when they are equal, so a constant makes
 * the TPM enter failure mode instead of running on weak entropy.
 */
int rand(void) { return 0x2a2a; }
void srand(unsigned seed) { (void)seed; }

static int digit_value(int c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'z') return c - 'a' + 10;
	if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
	return 99;
}

/* The common part of the strto* functions: the magnitude, the sign and the end. overflow is set when the magnitude
 * is larger than limit. */
static unsigned long long parse(const char *s, char **end, int base, unsigned long long limit, int *negative,
				int *overflow)
{
	const char *p = s;
	unsigned long long value = 0;
	int any = 0;

	*negative = 0;
	*overflow = 0;
	while (isspace((unsigned char)*p)) p++;
	if (*p == '+' || *p == '-') *negative = *p++ == '-';
	if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && digit_value(p[2]) < 16) {
		p += 2;
		base = 16;
	} else if (base == 0) {
		base = p[0] == '0' ? 8 : 10;
	}
	if (base < 2 || base > 36) {
		crt_errno = EINVAL;
		if (end) *end = (char *)s;
		return 0;
	}
	for (; digit_value((unsigned char)*p) < base; p++) {
		unsigned d = (unsigned)digit_value((unsigned char)*p);
		any = 1;
		if (value > (limit - d) / (unsigned)base) *overflow = 1;
		else value = value * (unsigned)base + d;
	}
	if (end) *end = (char *)(any ? p : s);
	return value;
}

unsigned long long strtoull(const char *s, char **end, int base)
{
	int negative, overflow;
	unsigned long long v = parse(s, end, base, ULLONG_MAX, &negative, &overflow);
	if (overflow) {
		crt_errno = ERANGE;
		return ULLONG_MAX;
	}
	return negative ? 0ull - v : v;
}

unsigned long strtoul(const char *s, char **end, int base)
{
	int negative, overflow;
	unsigned long long v = parse(s, end, base, ULONG_MAX, &negative, &overflow);
	if (overflow) {
		crt_errno = ERANGE;
		return ULONG_MAX;
	}
	return (unsigned long)(negative ? 0ull - v : v);
}

long long strtoll(const char *s, char **end, int base)
{
	int negative, overflow;
	unsigned long long v = parse(s, end, base, (unsigned long long)LLONG_MAX + 1, &negative, &overflow);
	if (overflow || (!negative && v > (unsigned long long)LLONG_MAX)) {
		crt_errno = ERANGE;
		return negative ? LLONG_MIN : LLONG_MAX;
	}
	return negative ? (long long)(0ull - v) : (long long)v;
}

long strtol(const char *s, char **end, int base)
{
	int negative, overflow;
	unsigned long long v = parse(s, end, base, (unsigned long long)LONG_MAX + 1, &negative, &overflow);
	if (overflow || (!negative && v > (unsigned long long)LONG_MAX)) {
		crt_errno = ERANGE;
		return negative ? LONG_MIN : LONG_MAX;
	}
	return negative ? (long)(0ull - v) : (long)v;
}

int atoi(const char *s) { return (int)strtol(s, NULL, 10); }
long atol(const char *s) { return strtol(s, NULL, 10); }
int abs(int value) { return value < 0 ? -value : value; }
long labs(long value) { return value < 0 ? -value : value; }
long long llabs(long long value) { return value < 0 ? -value : value; }

static void swap_bytes(unsigned char *a, unsigned char *b, size_t size)
{
	while (size--) {
		unsigned char t = *a;
		*a++ = *b;
		*b++ = t;
	}
}

static void sift_down(unsigned char *base, size_t root, size_t count, size_t size,
		      int (*compare)(const void *, const void *))
{
	for (;;) {
		size_t child = 2 * root + 1;
		if (child >= count) return;
		if (child + 1 < count && compare(base + child * size, base + (child + 1) * size) < 0) child++;
		if (compare(base + root * size, base + child * size) >= 0) return;
		swap_bytes(base + root * size, base + child * size, size);
		root = child;
	}
}

/* Heap sort: no recursion and no memory, O(n log n) in every case. Not stable, as qsort need not be. */
void qsort(void *base, size_t count, size_t size, int (*compare)(const void *, const void *))
{
	unsigned char *b = base;
	size_t i;
	if (count < 2 || size == 0) return;
	for (i = count / 2; i-- > 0;) sift_down(b, i, count, size, compare);
	for (i = count - 1; i > 0; i--) {
		swap_bytes(b, b + i * size, size);
		sift_down(b, 0, i, size, compare);
	}
}

void *bsearch(const void *key, const void *base, size_t count, size_t size,
	      int (*compare)(const void *, const void *))
{
	const unsigned char *b = base;
	size_t low = 0, high = count;
	while (low < high) {
		size_t mid = low + (high - low) / 2;
		int c = compare(key, b + mid * size);
		if (c == 0) return (void *)(b + mid * size);
		if (c < 0) high = mid;
		else low = mid + 1;
	}
	return NULL;
}
