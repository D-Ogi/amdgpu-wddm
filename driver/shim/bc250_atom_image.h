/* Checked, read-only views over caller-owned immutable firmware bytes. No hardware access. */
#ifndef BC250_ATOM_IMAGE_H
#define BC250_ATOM_IMAGE_H
#include "bc250_atom_types.h"

#define BC250_ATOM_MAX_IMAGE (1024u * 1024u)
#define BC250_ATOM_MAX_VFCT (16u * 1024u * 1024u)

typedef enum BC250_ATOM_STATUS {
    BC250_ATOM_OK = 0,
    BC250_ATOM_ARGUMENT,
    BC250_ATOM_BOUNDS,
    BC250_ATOM_SIGNATURE,
    BC250_ATOM_CHECKSUM,
    BC250_ATOM_IDENTITY_MISMATCH,
    BC250_ATOM_NOT_FOUND,
    BC250_ATOM_AMBIGUOUS,
    BC250_ATOM_VERSION,
    BC250_ATOM_OPCODE,
    BC250_ATOM_PARAMETER,
    BC250_ATOM_RECURSION,
    BC250_ATOM_BUDGET,
    BC250_ATOM_IO,
    BC250_ATOM_CANCELLED,
    BC250_ATOM_MEMORY
} BC250_ATOM_STATUS;

typedef struct BC250_ATOM_IDENTITY {
    uint32_t Bus, Device, Function;
    uint16_t VendorId, DeviceId;
} BC250_ATOM_IDENTITY;

typedef struct BC250_ATOM_IMAGE {
    const uint8_t *Bytes;
    uint32_t Length;
    uint16_t CommandMaster, DataMaster;
    uint16_t CommandCount, DataCount;
} BC250_ATOM_IMAGE;

typedef struct BC250_ATOM_TABLE {
    uint32_t Offset, Length;
    uint8_t Format, Content;
    uint8_t WorkspaceDwords, ParameterBytes;
} BC250_ATOM_TABLE;

/* Outputs are cleared on failure. A VFCT image must match all five PCI fields.
 * The caller must copy the selected image before freeing the VFCT, and must not mutate it during use. */
BC250_ATOM_STATUS Bc250AtomImageOpen(const void *bytes, size_t length,
    uint16_t vendor, uint16_t device, BC250_ATOM_IMAGE *out);
BC250_ATOM_STATUS Bc250AtomVfctSelect(const void *bytes, size_t length,
    const BC250_ATOM_IDENTITY *identity, BC250_ATOM_IMAGE *out);
BC250_ATOM_STATUS Bc250AtomGetTable(const BC250_ATOM_IMAGE *image, unsigned index,
    int command, BC250_ATOM_TABLE *out);
#endif
