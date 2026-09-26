/* The private-data blobs bc250kmd reads. Layout is driver/contract/bc250_umd_submit.h and
 * bc250_umd_private.h. Offsets below were printed from that header (scratch/tmp/blob_off.c).
 * This file does not include the contract header: Mesa's own amdgpu_drm.h is a different copy.
 */
#ifndef RADV_WDDM2_BC250_H
#define RADV_WDDM2_BC250_H

#include <stdint.h>

#define BC250_ALLOC_MAGIC   0x41324342u /* "BC2A" */
#define BC250_CONTEXT_MAGIC 0x43324342u /* "BC2C" */
#define BC250_SUBMIT_MAGIC  0x53324342u /* "BC2S" */
#define BC250_CAPS_MAGIC    0x35324342u /* "BC25" */

#define BC250_HEAP_GTT  0x2u
#define BC250_HEAP_VRAM 0x4u
#define BC250_IP_GFX    0u

#define BC250_A_EXACT_VA 0x1u

struct bc250_alloc_blob {
   uint32_t magic, version, size, flags;
   uint64_t alloc_size;
   uint64_t phys_alignment;
   uint32_t preferred_heap;
   uint32_t reserved0;
   uint64_t gem_flags;
   uint64_t requested_va;
   uint64_t va_size;
   uint64_t va_flags;
   uint64_t va_offset;
   uint32_t metadata_size;
   uint32_t metadata[16];
   uint32_t reserved[11];
};

struct bc250_context_blob {
   uint32_t magic, version, size, flags;
   uint32_t ip_type, ip_instance, ring, priority, stable_pstate;
   uint32_t reserved[7];
   uint32_t node_ordinal;
   uint32_t reserved_v2[3];
};

struct bc250_ib_blob {
   uint64_t va_start;
   uint32_t ib_bytes;
   uint32_t ip_type;
   uint32_t ip_instance;
   uint32_t ring;
   uint32_t ib_flags;
   uint32_t reserved;
};

struct bc250_submit_blob {
   uint32_t magic, version, size, flags;
   uint32_t ip_type;
   uint32_t num_ibs;
   uint64_t fence_va;
   uint64_t fence_value;
   struct bc250_ib_blob ib[16];
};

#endif
