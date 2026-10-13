#include "bc250_atom_firmware.h"
#include <string.h>

static int SameIdentity(const BC250_ATOM_IDENTITY *a, const BC250_ATOM_IDENTITY *b)
{
    return a->Bus == b->Bus && a->Device == b->Device && a->Function == b->Function &&
        a->VendorId == b->VendorId && a->DeviceId == b->DeviceId;
}

static int ValidIdentity(const BC250_ATOM_IDENTITY *id)
{
    return id->Bus <= 255 && id->Device <= 31 && id->Function <= 7 &&
        id->VendorId && id->VendorId != 0xffff && id->DeviceId && id->DeviceId != 0xffff;
}

static uint32_t ReadId(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

BC250_ATOM_STATUS Bc250AtomFirmwareCapture(const BC250_ATOM_FIRMWARE_IO *io, BC250_ATOM_FIRMWARE *out)
{
    BC250_ATOM_IDENTITY first = {0}, last = {0};
    BC250_ATOM_FIRMWARE captured = {0};
    BC250_ATOM_IMAGE selected = {0};
    BC250_ATOM_STATUS status = BC250_ATOM_IO;
    uint8_t *ids = NULL, *vfct = NULL;
    uint32_t capacity = 0, returned = 0, i, matches = 0;

    if (!io || !out || out->Storage || out->Image.Bytes || !io->Identity || !io->Enumerate ||
        !io->FetchVfct || !io->Allocate || !io->Free) return BC250_ATOM_ARGUMENT;
    if (io->Identity(io->Context, &first)) return BC250_ATOM_IO;
    if (!ValidIdentity(&first)) return BC250_ATOM_IDENTITY_MISMATCH;
    if (io->Enumerate(io->Context, NULL, 0, &capacity)) return BC250_ATOM_IO;
    if (!capacity) return BC250_ATOM_NOT_FOUND;
    if (capacity > BC250_ATOM_MAX_TABLE_IDS || capacity % sizeof(uint32_t)) return BC250_ATOM_BOUNDS;
    ids = io->Allocate(io->Context, capacity);
    if (!ids) return BC250_ATOM_MEMORY;
    if (io->Enumerate(io->Context, ids, capacity, &returned)) goto done;
    if (returned != capacity) { status = BC250_ATOM_BOUNDS; goto done; }
    for (i = 0; i < returned; i += sizeof(uint32_t))
        if (ReadId(ids + i) == BC250_ATOM_VFCT_ID) ++matches;
    if (matches != 1) {
        status = matches ? BC250_ATOM_AMBIGUOUS : BC250_ATOM_NOT_FOUND;
        goto done;
    }
    io->Free(io->Context, ids);
    ids = NULL;
    capacity = 0;
    if (io->FetchVfct(io->Context, NULL, 0, &capacity)) goto done;
    if (!capacity || capacity > BC250_ATOM_MAX_VFCT) { status = BC250_ATOM_BOUNDS; goto done; }
    vfct = io->Allocate(io->Context, capacity);
    if (!vfct) { status = BC250_ATOM_MEMORY; goto done; }
    returned = 0;
    if (io->FetchVfct(io->Context, vfct, capacity, &returned)) goto done;
    if (returned != capacity) { status = BC250_ATOM_BOUNDS; goto done; }
    status = Bc250AtomVfctSelect(vfct, returned, &first, &selected);
    if (status != BC250_ATOM_OK) goto done;
    captured.Storage = io->Allocate(io->Context, selected.Length);
    if (!captured.Storage) { status = BC250_ATOM_MEMORY; goto done; }
    memcpy(captured.Storage, selected.Bytes, selected.Length);
    status = Bc250AtomImageOpen(captured.Storage, selected.Length, first.VendorId, first.DeviceId, &captured.Image);
    if (status != BC250_ATOM_OK) goto done;
    if (io->Identity(io->Context, &last)) { status = BC250_ATOM_IO; goto done; }
    if (!SameIdentity(&first, &last)) { status = BC250_ATOM_IDENTITY_MISMATCH; goto done; }
    captured.Identity = first;
    *out = captured;
    captured.Storage = NULL;
done:
    if (captured.Storage) io->Free(io->Context, captured.Storage);
    if (vfct) io->Free(io->Context, vfct);
    if (ids) io->Free(io->Context, ids);
    return status;
}

void Bc250AtomFirmwareRelease(const BC250_ATOM_FIRMWARE_IO *io, BC250_ATOM_FIRMWARE *firmware)
{
    if (!io || !io->Free || !firmware) return;
    if (firmware->Storage) io->Free(io->Context, firmware->Storage);
    memset(firmware, 0, sizeof(*firmware));
}
