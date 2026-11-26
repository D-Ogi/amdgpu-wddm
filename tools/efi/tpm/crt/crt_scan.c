/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * sscanf of the stage C runtime: the conversions d i u x o (with hh h l ll z j), s, c and %, white space and literal
 * characters. That covers the calls in OpenSSL (OSSL_parse_url reads a port number with %u and %d). A conversion
 * that it does not know ends the scan, as a failed match does.
 */
#include <ctype.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

static void store_signed(va_list *args, char length, long long value)
{
	switch (length) {
	case 'H': *va_arg(*args, signed char *) = (signed char)value; break;
	case 'h': *va_arg(*args, short *) = (short)value; break;
	case 'l': *va_arg(*args, long *) = (long)value; break;
	case 'q':
	case 'j':
	case 'z': *va_arg(*args, long long *) = value; break;
	default: *va_arg(*args, int *) = (int)value; break;
	}
}

static void store_unsigned(va_list *args, char length, unsigned long long value)
{
	switch (length) {
	case 'H': *va_arg(*args, unsigned char *) = (unsigned char)value; break;
	case 'h': *va_arg(*args, unsigned short *) = (unsigned short)value; break;
	case 'l': *va_arg(*args, unsigned long *) = (unsigned long)value; break;
	case 'q':
	case 'j':
	case 'z': *va_arg(*args, unsigned long long *) = value; break;
	default: *va_arg(*args, unsigned *) = (unsigned)value; break;
	}
}

static int vsscanf_impl(const char *s, const char *f, va_list *args)
{
	int assigned = 0;

	while (*f) {
		char length = 0, conversion;
		int skip = 0, width = 0;

		if (isspace((unsigned char)*f)) {
			while (isspace((unsigned char)*f)) f++;
			while (isspace((unsigned char)*s)) s++;
			continue;
		}
		if (*f != '%') {
			if (*s != *f) break;
			s++;
			f++;
			continue;
		}
		f++;
		if (*f == '%') {
			while (isspace((unsigned char)*s)) s++;
			if (*s != '%') break;
			s++;
			f++;
			continue;
		}
		if (*f == '*') {
			skip = 1;
			f++;
		}
		while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
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
		} else if (*f == 'z' || *f == 'j') {
			length = *f++;
		}
		conversion = *f++;
		if (conversion == 'c') {
			int n = width ? width : 1, i;
			char *out = skip ? NULL : va_arg(*args, char *);
			for (i = 0; i < n; i++) {
				if (!s[i]) return assigned ? assigned : EOF;
				if (out) out[i] = s[i];
			}
			s += n;
			if (out) assigned++;
			continue;
		}
		while (isspace((unsigned char)*s)) s++;
		if (!*s) return assigned ? assigned : EOF;
		if (conversion == 's') {
			char *out = skip ? NULL : va_arg(*args, char *);
			int n = 0;
			while (s[n] && !isspace((unsigned char)s[n]) && (!width || n < width)) {
				if (out) out[n] = s[n];
				n++;
			}
			if (out) {
				out[n] = 0;
				assigned++;
			}
			s += n;
			continue;
		}
		if (conversion == 'd' || conversion == 'i' || conversion == 'u' || conversion == 'x' || conversion == 'X' ||
		    conversion == 'o') {
			int base = conversion == 'd' || conversion == 'u' ? 10 : conversion == 'i' ? 0 : conversion == 'o' ? 8 : 16;
			char *end;
			if (conversion == 'd' || conversion == 'i') {
				long long v = strtoll(s, &end, base);
				if (end == s) break;
				if (!skip) {
					store_signed(args, length, v);
					assigned++;
				}
			} else {
				unsigned long long v = strtoull(s, &end, base);
				if (end == s) break;
				if (!skip) {
					store_unsigned(args, length, v);
					assigned++;
				}
			}
			s = end;
			continue;
		}
		break;
	}
	return assigned;
}

int sscanf(const char *s, const char *format, ...)
{
	va_list args;
	int n;
	va_start(args, format);
	n = vsscanf_impl(s, format, &args);
	va_end(args);
	return n;
}
