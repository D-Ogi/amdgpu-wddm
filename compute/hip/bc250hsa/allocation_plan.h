/* Checked private GPUVA/child sizing. No Windows calls or backing allocations. */
#ifndef BC250HSA_ALLOCATION_PLAN_H
#define BC250HSA_ALLOCATION_PLAN_H
#include <stdint.h>
#include <stddef.h>

/* BD-117 measured rounded CreateAllocation2 sizes above 0xffff0000 refused.
 * Keep the existing 256 KiB physical granularity below that boundary. */
#define BC250HSA_ALLOCATION_CHILD_MAX UINT64_C(0xfffc0000)
#define BC250HSA_ALLOCATION_GRANULARITY UINT64_C(0x40000)
#define BC250HSA_RESERVATION_GRANULARITY UINT64_C(0x10000)

typedef struct bc250hsa_allocation_plan {
    uint64_t requested;
    uint64_t bytes;
    uint64_t alignment;
    uint64_t reservation_bytes;
    uint64_t child_bytes;
    uint32_t child_count;
} bc250hsa_allocation_plan;

static inline int bc250hsa_checked_round(uint64_t value, uint64_t alignment, uint64_t* out)
{
    if (out == NULL || alignment == 0u || (alignment & (alignment - 1u)) != 0u ||
        value > UINT64_MAX - (alignment - 1u)) { return 0; }
    *out = (value + alignment - 1u) & ~(alignment - 1u);
    return 1;
}

/* MaximumAddress is inclusive, as in KMT. cap is an explicit private test seam,
 * never an environment knob. Every returned reservation can satisfy alignment. */
static inline int bc250hsa_plan_allocation(uint64_t bytes, uint64_t alignment,
                                  uint64_t minimum, uint64_t maximum, uint64_t cap,
                                  bc250hsa_allocation_plan* out)
{
    bc250hsa_allocation_plan p;
    uint64_t first, count, slack;
    if (out == NULL || bytes == 0u || minimum > maximum ||
        cap == 0u || cap > BC250HSA_ALLOCATION_CHILD_MAX ||
        cap % BC250HSA_ALLOCATION_GRANULARITY != 0u) { return 0; }
    if (alignment == 0u) { alignment = BC250HSA_ALLOCATION_GRANULARITY; }
    if ((alignment & (alignment - 1u)) != 0u) { return 0; }
    if (alignment < BC250HSA_RESERVATION_GRANULARITY) {
        alignment = BC250HSA_RESERVATION_GRANULARITY;
    }
    if (!bc250hsa_checked_round(bytes, BC250HSA_ALLOCATION_GRANULARITY, &p.bytes) ||
        !bc250hsa_checked_round(minimum, BC250HSA_RESERVATION_GRANULARITY, &first)) { return 0; }
    slack = alignment - BC250HSA_RESERVATION_GRANULARITY;
    if (p.bytes > UINT64_MAX - slack) { return 0; }
    p.reservation_bytes = p.bytes + slack;
    if (first > maximum || p.reservation_bytes - 1u > maximum - first) { return 0; }
    count = p.bytes / cap + (p.bytes % cap != 0u);
    if (count > UINT32_MAX || count > SIZE_MAX / sizeof(uint64_t)) { return 0; }
    p.requested = bytes;
    p.alignment = alignment;
    p.child_bytes = cap;
    p.child_count = (uint32_t)count;
    *out = p;
    return 1;
}

static inline int bc250hsa_plan_child(const bc250hsa_allocation_plan* p, uint32_t index,
                              uint64_t* offset, uint64_t* bytes)
{
    uint64_t at;
    if (p == NULL || offset == NULL || bytes == NULL || index >= p->child_count ||
        p->child_bytes == 0u || index > UINT64_MAX / p->child_bytes) { return 0; }
    at = (uint64_t)index * p->child_bytes;
    if (at >= p->bytes) { return 0; }
    *offset = at;
    *bytes = p->bytes - at < p->child_bytes ? p->bytes - at : p->child_bytes;
    return 1;
}

/* The bytes of the local budget that the layout leaves free for later small allocations
 * (rings, code objects, kernel arguments), and for the page rounding of VidMm's usage count. */
#define BC250HSA_ALLOCATION_LOCAL_RESERVE UINT64_C(0x4000000)

typedef struct bc250hsa_child_layout {
    uint64_t offset;
    uint64_t bytes;
    uint32_t carve_out;   /* 1: the carve-out first; 0: system memory only */
} bc250hsa_child_layout;

/* The children of a plan under a local budget (BD-117). Children of the plan's size go to the
 * carve-out while the local bytes left, less the reserve, hold them; the first child that does
 * not fit is cut to what is left, rounded down to the granularity, and every child after it goes
 * to system memory. A cut child is the only one off the plan's size, so the count is at most
 * child_count + 1, which capacity must hold. local_left is UINT64_MAX when the budget is not
 * known: then every child has the plan's size and tries the carve-out first. *system_bytes is the
 * sum of the children for system memory. Returns the count, or 0 for a bad argument. */
static inline uint32_t bc250hsa_plan_layout(const bc250hsa_allocation_plan* p, uint64_t local_left,
                                            bc250hsa_child_layout* out, uint32_t capacity,
                                            uint64_t* system_bytes)
{
    uint64_t room, at = 0u, system = 0u;
    uint32_t n = 0u;
    if (p == NULL || out == NULL || system_bytes == NULL || p->child_bytes == 0u ||
        p->bytes == 0u || p->bytes % BC250HSA_ALLOCATION_GRANULARITY != 0u ||
        p->child_bytes % BC250HSA_ALLOCATION_GRANULARITY != 0u) { return 0u; }
    room = local_left > BC250HSA_ALLOCATION_LOCAL_RESERVE
               ? local_left - BC250HSA_ALLOCATION_LOCAL_RESERVE : 0u;
    room &= ~(BC250HSA_ALLOCATION_GRANULARITY - 1u);
    while (at < p->bytes) {
        uint64_t size = p->bytes - at < p->child_bytes ? p->bytes - at : p->child_bytes;
        uint32_t carve_out = 1u;
        if (room < size) {
            if (room != 0u) { size = room; } else { carve_out = 0u; }
        }
        if (n >= capacity) { return 0u; }
        out[n].offset = at;
        out[n].bytes = size;
        out[n].carve_out = carve_out;
        if (carve_out) { room -= size; } else { system += size; }
        at += size;
        ++n;
    }
    *system_bytes = system;
    return n;
}

static inline int bc250hsa_plan_base(const bc250hsa_allocation_plan* p, uint64_t reservation,
                             uint64_t minimum, uint64_t maximum, uint64_t* logical)
{
    uint64_t base;
    if (p == NULL || logical == NULL || reservation == 0u || reservation < minimum ||
        reservation > maximum || reservation % BC250HSA_RESERVATION_GRANULARITY != 0u ||
        p->reservation_bytes == 0u || p->reservation_bytes - 1u > maximum - reservation ||
        !bc250hsa_checked_round(reservation, p->alignment, &base) ||
        base - reservation > p->reservation_bytes ||
        p->bytes > p->reservation_bytes - (base - reservation)) { return 0; }
    *logical = base;
    return 1;
}
#endif
