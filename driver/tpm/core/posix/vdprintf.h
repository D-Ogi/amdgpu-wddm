/* POSIX stand-in for the libtpms build on Windows: tpm_library.c prints its log to the console with vdprintf and dprintf, which a Windows C
   library does not declare. These declarations let the file compile and link. */
#ifndef BC250_POSIX_VDPRINTF_H
#define BC250_POSIX_VDPRINTF_H
#include <stdarg.h>
int vdprintf(int fd, const char *format, va_list ap);
int dprintf(int fd, const char *format, ...);
#endif
