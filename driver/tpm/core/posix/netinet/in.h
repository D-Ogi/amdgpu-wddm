/* POSIX stand-in for the libtpms build on Windows: the TCG code asks a POSIX system for the byte-order helpers. The target is little endian
   and the helpers are pure arithmetic, so no system call is involved. */
#ifndef BC250_POSIX_NETINET_IN_H
#define BC250_POSIX_NETINET_IN_H
#include <stdint.h>
static inline uint32_t htonl(uint32_t v) { return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) | ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24); }
static inline uint16_t htons(uint16_t v) { return (uint16_t)(((v & 0x00FFu) << 8) | ((v & 0xFF00u) >> 8)); }
static inline uint32_t ntohl(uint32_t v) { return htonl(v); }
static inline uint16_t ntohs(uint16_t v) { return htons(v); }
#endif
