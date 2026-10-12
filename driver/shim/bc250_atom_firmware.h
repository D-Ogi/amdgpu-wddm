/* Read-only firmware acquisition. The device owner serializes capture, use and release. */
#ifndef BC250_ATOM_FIRMWARE_H
#define BC250_ATOM_FIRMWARE_H
#include "bc250_atom_image.h"

/* ACPI IDs in AuxKlib's table buffer have little-endian byte order. */
#define BC250_ATOM_VFCT_ID ((uint32_t)'V' | ((uint32_t)'F' << 8) | ((uint32_t)'C' << 16) | ((uint32_t)'T' << 24))
#define BC250_ATOM_MAX_TABLE_IDS (64u * 1024u)

typedef struct BC250_ATOM_FIRMWARE_IO {
    void *Context;
    int (*Identity)(void *context, BC250_ATOM_IDENTITY *identity);
    /* Return zero on success. For a NULL buffer, accept the provider's size-query result.
     * For a real buffer, success means it holds the complete result, not a prefix. */
    int (*Enumerate)(void *context, void *buffer, uint32_t capacity, uint32_t *returned);
    int (*FetchVfct)(void *context, void *buffer, uint32_t capacity, uint32_t *returned);
    void *(*Allocate)(void *context, size_t bytes);
    void (*Free)(void *context, void *buffer);
} BC250_ATOM_FIRMWARE_IO;

typedef struct BC250_ATOM_FIRMWARE {
    BC250_ATOM_IDENTITY Identity;
    BC250_ATOM_IMAGE Image;
    void *Storage;
} BC250_ATOM_FIRMWARE;

/* out must be empty; failure leaves it unchanged. A successful object owns only a
 * private copy of the selected ROM, never pointers into an ACPI provider buffer.
 * Only one VFCT table is admitted: AuxKlib cannot select a second table with the same ID.
 * There is no retry or fallback to arbitrary physical memory after an inconsistent read. */
BC250_ATOM_STATUS Bc250AtomFirmwareCapture(const BC250_ATOM_FIRMWARE_IO *io, BC250_ATOM_FIRMWARE *out);
void Bc250AtomFirmwareRelease(const BC250_ATOM_FIRMWARE_IO *io, BC250_ATOM_FIRMWARE *firmware);
#endif
