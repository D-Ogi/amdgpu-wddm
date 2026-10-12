#pragma once
#include <stddef.h>
/* Returns 1 only for the documented BC-250 identities; reason is BOARD (2) or FIRMWARE (3). */
unsigned BoardIdentitySelect(const unsigned char* pci, size_t pciBytes,
                             const unsigned char* rsmb, size_t rsmbBytes, unsigned* reason);
/* Qualification only; caller must first accept BoardIdentitySelect. Failure zeros outputs. */
int BoardIdentityQualification(const unsigned char* rsmb, size_t bytes,
                               unsigned* biosId, unsigned char machineId[16]);
