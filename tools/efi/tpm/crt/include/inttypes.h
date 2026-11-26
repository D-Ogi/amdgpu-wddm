/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* <inttypes.h> of the stage C runtime (crt.h): the printf macros for the LLP64 model of the stage target. */
#ifndef CRT_INTTYPES_H
#define CRT_INTTYPES_H
#include <stdint.h>

#define PRId8 "d"
#define PRId16 "d"
#define PRId32 "d"
#define PRId64 "lld"
#define PRIi32 "i"
#define PRIi64 "lli"
#define PRIu8 "u"
#define PRIu16 "u"
#define PRIu32 "u"
#define PRIu64 "llu"
#define PRIx8 "x"
#define PRIx16 "x"
#define PRIx32 "x"
#define PRIx64 "llx"
#define PRIX32 "X"
#define PRIX64 "llX"
#define PRIdPTR "lld"
#define PRIuPTR "llu"
#define PRIxPTR "llx"
#define PRIdMAX "lld"
#define PRIuMAX "llu"
#define SCNu32 "u"
#define SCNu64 "llu"

#endif
