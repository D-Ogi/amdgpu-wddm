/* The WDK kernel CRT has no stdint.h. Keep the ATOM wire types equal in host and KMD builds. */
#ifndef BC250_ATOM_TYPES_H
#define BC250_ATOM_TYPES_H
#include <stddef.h>
#if defined(BC250_SHIM_KERNEL)
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned __int64 uint64_t;
#ifndef UINT32_C
#define UINT32_C(value) value##U
#endif
#ifndef UINT32_MAX
#define UINT32_MAX 0xffffffffU
#endif
#else
#include <stdint.h>
#endif
typedef char AtomUint8Size[(sizeof(uint8_t) == 1) ? 1 : -1];
typedef char AtomUint16Size[(sizeof(uint16_t) == 2) ? 1 : -1];
typedef char AtomUint32Size[(sizeof(uint32_t) == 4) ? 1 : -1];
typedef char AtomUint64Size[(sizeof(uint64_t) == 8) ? 1 : -1];
#endif
