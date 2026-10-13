#ifndef BC250_ATOM_PREFLIGHT_H
#define BC250_ATOM_PREFLIGHT_H
#include "bc250_atom_image.h"

#define BC250_ATOM_TABLES 256u
#define BC250_ATOM_CODE_LIMIT 131072u /* 16-bit table offset + 16-bit length. */
#define BC250_ATOM_INSTRUCTIONS 32768u
#define BC250_ATOM_DEPTH 16u
#define BC250_ATOM_PARAMS 64u

typedef struct BC250_ATOM_PLAN_TABLE {
    BC250_ATOM_TABLE Table;
    uint8_t Seen, Colour, Height;
    uint16_t OwnParameters, TotalParameters;
    uint8_t Calls[BC250_ATOM_TABLES/8];
} BC250_ATOM_PLAN_TABLE;

/* Caller allocates this workspace from pool/heap, never from the kernel stack.
 * A successful preflight is structural only. Every runtime MMIO, parameter and
 * data read still needs its own checked callback and the shared execution budget. */
typedef struct BC250_ATOM_PLAN {
    const uint8_t *ImageBytes;
    uint32_t ImageLength, Root;
    BC250_ATOM_PLAN_TABLE Tables[BC250_ATOM_TABLES];
    uint8_t Boundary[BC250_ATOM_CODE_LIMIT];
    uint8_t Marks[BC250_ATOM_CODE_LIMIT];
    uint32_t Work[BC250_ATOM_INSTRUCTIONS];
    uint32_t Instructions, TableCount, FailedTable, FailedOffset;
    uint32_t Head, Tail;
    BC250_ATOM_STATUS Status;
} BC250_ATOM_PLAN;

BC250_ATOM_STATUS Bc250AtomPreflight(const BC250_ATOM_IMAGE *image, unsigned root,
    BC250_ATOM_PLAN *plan);
#endif
