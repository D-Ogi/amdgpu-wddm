/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include "sb_crc32.h"

unsigned int sb_crc32(const void *data, size_t bytes)
{
	const unsigned char *p = (const unsigned char *)data;
	unsigned int crc = 0xFFFFFFFFu;
	size_t i;
	int bit;
	for (i = 0; i < bytes; i++) {
		crc ^= p[i];
		for (bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
	}
	return crc ^ 0xFFFFFFFFu;
}
