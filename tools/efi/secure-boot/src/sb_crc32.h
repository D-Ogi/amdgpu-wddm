/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * CRC-32 of the UEFI tables (UEFI Specification 2.10, "EFI_TABLE_HEADER": CRC32 is the standard CRC-32 of the table
 * with the CRC32 field set to zero). The same algorithm as EFI_BOOT_SERVICES.CalculateCrc32; a pure function, so the
 * host test drives exactly what the image runs.
 */
#ifndef SB_CRC32_H
#define SB_CRC32_H

#include <stddef.h>

/* The CRC-32 of a byte range: polynomial 0xEDB88320 (the reflected 0x04C11DB7), start 0xFFFFFFFF, final xor
 * 0xFFFFFFFF. A NULL range with 0 bytes gives 0. */
unsigned int sb_crc32(const void *data, size_t bytes);

#endif
