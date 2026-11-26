/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The definitions that the UEFI target of OpenSSL expects from its build environment. edk2 gives them through its
 * base types and the build flags of CryptoPkg; this stage has no edk2. build.ps1 includes this file in front of each
 * OpenSSL and libtpms object (-include), so that both sides of the OpenSSL headers see the same types.
 *
 * - The UEFI base types of include/openssl/e_os2.h. They are the same types as the <stdint.h> types of the target,
 *   so a translation unit that includes both sees one type for each name.
 * - The word size of the big numbers. The UEFI target does not read crypto/bn_conf.h. The stage target is LLP64,
 *   so a 64-bit word is "unsigned long long" (SIXTY_FOUR_BIT), not "unsigned long" (SIXTY_FOUR_BIT_LONG). */
#ifndef BC250_OSSL_PLATFORM_H
#define BC250_OSSL_PLATFORM_H

typedef signed char INT8;
typedef unsigned char UINT8;
typedef short INT16;
typedef unsigned short UINT16;
typedef int INT32;
typedef unsigned int UINT32;
typedef long long INT64;
typedef unsigned long long UINT64;
typedef long long INTN;
typedef unsigned long long UINTN;
#define MAX_INTN ((INTN)0x7FFFFFFFFFFFFFFFLL)

#undef SIXTY_FOUR_BIT_LONG
#undef THIRTY_TWO_BIT
#undef BN_LLONG
#define SIXTY_FOUR_BIT

#endif
