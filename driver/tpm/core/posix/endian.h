/* POSIX stand-in for the libtpms build on Windows: the TCG code reads byte order from a POSIX header. The stage target is little endian. */
#ifndef BC250_POSIX_ENDIAN_H
#define BC250_POSIX_ENDIAN_H
#define __LITTLE_ENDIAN 1234
#define __BIG_ENDIAN 4321
#define __BYTE_ORDER __LITTLE_ENDIAN
#endif
