/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The register BAR of the GPU function: its index in the PCI configuration header and the length that is mapped.
 * driver/kmd/mmio.c maps it under Windows, and tools/efi/core-unlock reaches the same registers through the PCI I/O
 * or the PCI Root Bridge I/O protocol before Windows starts. One definition for both, so the two can never
 * disagree. The index is the dword of the header, not the BarIndex of the PCI I/O protocol: the reference
 * implementation of that protocol counts a 64-bit BAR as one index (tools/efi/core-unlock/src/cu_bar.h). */
#ifndef BC250_MMIO_BAR_H
#define BC250_MMIO_BAR_H
#define BC250_BAR5_INDEX 5
#define BC250_BAR5_LENGTH 0x80000ul
#endif
