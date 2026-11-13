/* The POSIX console calls that tpm_library.c uses for its log. The Windows C library has none, so the core sends the same text to stderr. */
#include <stdio.h>
#include <stdarg.h>

int vdprintf(int fd, const char *format, va_list ap)
{
	(void)fd;
	return vfprintf(stderr, format, ap);
}

int dprintf(int fd, const char *format, ...)
{
	va_list ap;
	int n;
	(void)fd;
	va_start(ap, format);
	n = vfprintf(stderr, format, ap);
	va_end(ap);
	return n;
}

