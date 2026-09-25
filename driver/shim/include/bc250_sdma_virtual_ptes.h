/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#ifndef BC250_SDMA_VIRTUAL_PTES_H
#define BC250_SDMA_VIRTUAL_PTES_H
#include "bc250_sdma_paging.h"

/* One retained OS DMA span. Every offset is relative to its CPU pointer and VA.
 * Submit the IB through the privileged context's current root, using VMID2.
 * The OS owns backing/residency until the real completion fence. Source and
 * destination are privileged GPU VAs, never build-time physical translations.
 * The caller must keep DMA storage disjoint from either table, including aliases.
 * This builder does not choose a root, publish logical shadows or submit work. */
struct bc250_sdma_virtual_ptes {
    unsigned int bytes, ib_offset, ib_dwords, csa_offset, marker_offset, staging_offset;
};
int bc250_sdma_build_virtual_ptes(struct amdgpu_device *adev, void *buffer,
    unsigned int capacity_bytes, u64 dma_va, u64 source_va, u64 destination_va,
    unsigned int entries, struct bc250_sdma_virtual_ptes *built);
#endif
