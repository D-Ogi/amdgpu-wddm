/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * vsnprintf and the functions on it, for the stage C runtime.
 *
 * The C99 conversions d i u o x X c s p %, with the flags - + space # 0, a width and a precision (also as *), and
 * the length modifiers hh h l ll j z t L. Floating point (a e f g) is not formatted: the argument is read and "?"
 * is written; libtpms and the OpenSSL parts that the engine uses do not print floating point values. %n writes
 * nothing. The return value is the length of the full text, as C99 says, also when it does not fit.
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct sink {
	char *out;
	size_t capacity;
	size_t length;	/* of the full text */
};

static void put(struct sink *s, char c)
{
	if (s->length + 1 < s->capacity) s->out[s->length] = c;
	s->length++;
}

static void put_repeat(struct sink *s, char c, int count)
{
	while (count-- > 0) put(s, c);
}

enum { F_LEFT = 1, F_PLUS = 2, F_SPACE = 4, F_ALT = 8, F_ZERO = 16 };

static void put_number(struct sink *s, unsigned long long value, int negative, unsigned base, int upper, int flags,
		       int width, int precision)
{
	char digits[24];
	const char *set = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	int n = 0, sign = 0, prefix = 0, zeros, pad;
	char sign_char = 0;

	while (value) {
		digits[n++] = set[value % base];
		value /= base;
	}
	if (precision < 0) precision = 1;
	else flags &= ~F_ZERO;	/* C99: with a precision the 0 flag is ignored */
	if (negative) sign_char = '-';
	else if (flags & F_PLUS) sign_char = '+';
	else if (flags & F_SPACE) sign_char = ' ';
	if (sign_char) sign = 1;
	if ((flags & F_ALT) && base == 16 && n) prefix = 2;
	if ((flags & F_ALT) && base == 8 && precision <= n) precision = n + 1;
	zeros = precision > n ? precision - n : 0;
	pad = width - (sign + prefix + zeros + n);
	if (pad > 0 && (flags & F_ZERO) && !(flags & F_LEFT)) {
		zeros += pad;
		pad = 0;
	}
	if (!(flags & F_LEFT)) put_repeat(s, ' ', pad);
	if (sign_char) put(s, sign_char);
	if (prefix) {
		put(s, '0');
		put(s, upper ? 'X' : 'x');
	}
	put_repeat(s, '0', zeros);
	while (n) put(s, digits[--n]);
	if (flags & F_LEFT) put_repeat(s, ' ', pad);
}

static void put_text(struct sink *s, const char *text, int flags, int width, int precision)
{
	size_t n;
	int pad;
	if (!text) text = "(null)";
	n = precision >= 0 ? strnlen(text, (size_t)precision) : strlen(text);
	pad = width > (int)n ? width - (int)n : 0;
	if (!(flags & F_LEFT)) put_repeat(s, ' ', pad);
	while (n--) put(s, *text++);
	if (flags & F_LEFT) put_repeat(s, ' ', pad);
}

int vsnprintf(char *out, size_t capacity, const char *format, va_list args)
{
	struct sink s = {out, capacity, 0};
	const char *f = format;

	while (*f) {
		int flags = 0, width = 0, precision = -1;
		char length = 0, conversion;
		unsigned long long u;
		long long d;

		if (*f != '%') {
			put(&s, *f++);
			continue;
		}
		f++;
		for (;; f++) {
			if (*f == '-') flags |= F_LEFT;
			else if (*f == '+') flags |= F_PLUS;
			else if (*f == ' ') flags |= F_SPACE;
			else if (*f == '#') flags |= F_ALT;
			else if (*f == '0') flags |= F_ZERO;
			else break;
		}
		if (*f == '*') {
			width = va_arg(args, int);
			if (width < 0) {
				flags |= F_LEFT;
				width = -width;
			}
			f++;
		} else {
			while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
		}
		if (*f == '.') {
			f++;
			precision = 0;
			if (*f == '*') {
				precision = va_arg(args, int);
				f++;
			} else {
				while (*f >= '0' && *f <= '9') precision = precision * 10 + (*f++ - '0');
			}
		}
		/* length: 'H' hh, 'h' h, 'l' l, 'q' ll, 'j' j, 'z' z, 't' t, 'L' L */
		if (*f == 'h') {
			length = 'h';
			if (*++f == 'h') {
				length = 'H';
				f++;
			}
		} else if (*f == 'l') {
			length = 'l';
			if (*++f == 'l') {
				length = 'q';
				f++;
			}
		} else if (*f == 'j' || *f == 'z' || *f == 't' || *f == 'L') {
			length = *f++;
		}
		conversion = *f;
		if (!conversion) break;
		f++;
		switch (conversion) {
		case 'd':
		case 'i':
			switch (length) {
			case 'l': d = va_arg(args, long); break;
			case 'q': d = va_arg(args, long long); break;
			case 'j': d = va_arg(args, intmax_t); break;
			case 'z': d = va_arg(args, long long); break;	/* the signed type of size_t on LLP64 */
			case 't': d = va_arg(args, ptrdiff_t); break;
			case 'H': d = (signed char)va_arg(args, int); break;
			case 'h': d = (short)va_arg(args, int); break;
			default: d = va_arg(args, int); break;
			}
			u = d < 0 ? 0ull - (unsigned long long)d : (unsigned long long)d;
			put_number(&s, u, d < 0, 10, 0, flags, width, precision);
			break;
		case 'u':
		case 'o':
		case 'x':
		case 'X':
			switch (length) {
			case 'l': u = va_arg(args, unsigned long); break;
			case 'q': u = va_arg(args, unsigned long long); break;
			case 'j': u = va_arg(args, uintmax_t); break;
			case 'z': u = va_arg(args, size_t); break;
			case 't': u = (unsigned long long)va_arg(args, ptrdiff_t); break;
			case 'H': u = (unsigned char)va_arg(args, unsigned); break;
			case 'h': u = (unsigned short)va_arg(args, unsigned); break;
			default: u = va_arg(args, unsigned); break;
			}
			put_number(&s, u, 0, conversion == 'u' ? 10 : conversion == 'o' ? 8 : 16, conversion == 'X',
				   flags & ~(F_PLUS | F_SPACE), width, precision);
			break;
		case 'p':
			u = (uintptr_t)va_arg(args, void *);
			put_number(&s, u, 0, 16, 0, (flags | F_ALT) & ~(F_PLUS | F_SPACE), width, precision);
			break;
		case 'c': {
			char c = (char)va_arg(args, int);
			int pad = width > 1 ? width - 1 : 0;
			if (!(flags & F_LEFT)) put_repeat(&s, ' ', pad);
			put(&s, c);
			if (flags & F_LEFT) put_repeat(&s, ' ', pad);
			break;
		}
		case 's':
			put_text(&s, va_arg(args, const char *), flags, width, precision);
			break;
		case 'a':
		case 'A':
		case 'e':
		case 'E':
		case 'f':
		case 'F':
		case 'g':
		case 'G':
			if (length == 'L') (void)va_arg(args, long double);
			else (void)va_arg(args, double);
			put_text(&s, "?", flags, width, -1);
			break;
		case 'n':
			(void)va_arg(args, void *);
			break;
		case '%':
			put(&s, '%');
			break;
		default:
			/* an unknown conversion: write it as it is */
			put(&s, '%');
			put(&s, conversion);
			break;
		}
	}
	if (capacity) out[s.length < capacity ? s.length : capacity - 1] = 0;
	return s.length > 0x7fffffff ? -1 : (int)s.length;
}

int snprintf(char *out, size_t capacity, const char *format, ...)
{
	va_list args;
	int n;
	va_start(args, format);
	n = vsnprintf(out, capacity, format, args);
	va_end(args);
	return n;
}

/* No bound: only for callers that size the buffer themselves. */
int vsprintf(char *out, const char *format, va_list args)
{
	return vsnprintf(out, 0x7fffffff, format, args);
}

int sprintf(char *out, const char *format, ...)
{
	va_list args;
	int n;
	va_start(args, format);
	n = vsprintf(out, format, args);
	va_end(args);
	return n;
}

int vasprintf(char **out, const char *format, va_list args)
{
	va_list copy;
	int n;
	char *text;

	*out = NULL;
	va_copy(copy, args);
	n = vsnprintf(NULL, 0, format, copy);
	va_end(copy);
	if (n < 0) return -1;
	text = malloc((size_t)n + 1);
	if (!text) return -1;
	vsnprintf(text, (size_t)n + 1, format, args);
	*out = text;
	return n;
}

int asprintf(char **out, const char *format, ...)
{
	va_list args;
	int n;
	va_start(args, format);
	n = vasprintf(out, format, args);
	va_end(args);
	return n;
}
