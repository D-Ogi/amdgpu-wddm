/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* <endian.h> of the stage C runtime (crt.h). The stage target is x86-64, little endian. */
#ifndef CRT_ENDIAN_H
#define CRT_ENDIAN_H

#define __LITTLE_ENDIAN 1234
#define __BIG_ENDIAN 4321
#define __BYTE_ORDER __LITTLE_ENDIAN
#define LITTLE_ENDIAN __LITTLE_ENDIAN
#define BIG_ENDIAN __BIG_ENDIAN
#define BYTE_ORDER __BYTE_ORDER

#define htobe16(x) __builtin_bswap16(x)
#define htobe32(x) __builtin_bswap32(x)
#define htobe64(x) __builtin_bswap64(x)
#define be16toh(x) __builtin_bswap16(x)
#define be32toh(x) __builtin_bswap32(x)
#define be64toh(x) __builtin_bswap64(x)
#define htole16(x) ((unsigned short)(x))
#define htole32(x) ((unsigned)(x))
#define htole64(x) ((unsigned long long)(x))
#define le16toh(x) ((unsigned short)(x))
#define le32toh(x) ((unsigned)(x))
#define le64toh(x) ((unsigned long long)(x))

#endif
