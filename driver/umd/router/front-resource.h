// SPDX-License-Identifier: MIT
//
// M15.14 increment 1: what the front remembers about a resource, and why it is a map and not an appended
// block.
//
// WHAT IS REMEMBERED. `pfnCheckDirectFlipSupport` is handed two `D3D10DDI_HRESOURCE` and nothing else.
// To answer it the front needs, for each side: the geometry and format, whether the surface is a primary
// of a swap chain, which video present source it names, whether this device created it or opened it from
// another process, and the E26R resource record its creator wrote. All of that is in front of the front
// at create or at open, and nowhere afterwards.
//
// WHY NOT AN APPENDED BLOCK. The front's DEVICE record is appended to the hosted driver's device block,
// because the hosted private device size is a function of the create arguments alone and the front can
// recompute it. A resource's private size is a function of the resource's own shape, so an appended
// resource record has no offset the front can recover from `hResource` later - and putting the record
// FIRST would change `hDrvResource` for every forwarded entry, which is the one thing the front may not
// do (front-adapter.h). So the record is kept beside the resource, in a map keyed by the driver handle.
//
// WHY THE MAP IS SMALL. Only a surface that could ever be one side of a DirectFlip pair is recorded: one
// created with `D3D10_DDI_BIND_PRESENT` or a `pPrimaryDesc`, or a shared one, or any opened one (an
// opened resource is shared by definition). An ordinary texture or buffer is not recorded at all, so the
// map holds the compositor's own primaries and the surfaces it opened - a handful, not a population. A
// handle the map does not know is answered FALSE with the reason "record", which is the safe answer.
//
// The map is a fixed open-addressing table with no allocation: a full table refuses to record further
// surfaces, says so once, and the rule then answers FALSE for them. It is read under a shared lock and
// written under an exclusive one; neither happens on a draw path. Create, open and destroy of a
// candidate surface are the only writers.
#ifndef BC250_FRONT_RESOURCE_H
#define BC250_FRONT_RESOURCE_H

#include "front-adapter.h"
#include "../../kmd/gdi_private.h"              // the placement derivation and the two scannable rules
#include "../../kmd/surface_resource_private.h"  // the E26R record, exactly as the kernel driver defines it

namespace bc250front {

// One surface the front may be asked about. Plain data: front-direct-flip.h is a pure function of two of
// these and the adapter's caps trailer, and the host gate builds them by hand.
struct Resource {
    bool recorded;        // this struct was filled from a create or an open, not zero-initialized
    bool opened;          // came from pfnOpenResource (the application's side of a pair)
    bool primary;         // BIND_PRESENT, or a pPrimaryDesc, or an E26R record that says PRIMARY
    bool shared;          // the create asked for a shared surface, or the record says so
    unsigned int vidpn_source;
    unsigned int width, height;
    // 0 means "not known here". An opened surface carries its pitch in the LB7A blob the kernel driver
    // wrote; a surface this device created does not, because the kernel driver chooses the pitch at
    // CreateAllocation and nothing hands it back to user mode. front-direct-flip.h refuses an unknown
    // pitch under its own name rather than guessing one.
    unsigned int pitch;
    unsigned int format;  // DXGI_FORMAT of the storage
    // The E26R record. At open it is the one the creator wrote, decoded from the open arguments and
    // normalized to v3 so that one struct carries every version (DecodeRecord below). At create it is
    // the record this device's own create implies, which is what amendment 5 means by "the compositor
    // side comes from its own creation record".
    BC250_SURFACE_RESOURCE_PRIVATE record;
};

// The E26R blob as it arrives: v1 is 12 bytes, v2 is 16, v3 is 64 (driver/kmd/surface_resource_private.h).
// A v1 or v2 blob is admitted by the kernel driver's own parser and carries no texture fields, so the
// front normalizes it into a v3-shaped struct but keeps the version it had: the DirectFlip rule demands
// a real v3 record of the application's side and must not be fooled by a normalized v1 one.
//   Returns false for a blob the kernel driver's parser refuses, and for one with no E26R magic at all
// (the desktop's live surfaces are 48-byte GDI1 blobs or carry no record). `out` is zeroed first either
// way, so a refused decode leaves a record that both scannable rules refuse.
inline bool DecodeRecord(const void *data, unsigned int bytes, BC250_SURFACE_RESOURCE_PRIVATE *out)
{
    const unsigned long *words = (const unsigned long *)data;
    int shared = 0, cached = 0;
    unsigned long version = 0, access = 0;
    ZeroMemory(out, sizeof(*out));
    if (!Bc250SurfaceResourcePolicy(data, bytes, &shared, &cached)) return false;
    if (!Bc250SurfaceResourceIntent(data, bytes, &version, &access)) return false;
    if (!data || bytes < 3 * sizeof(unsigned long) || words[0] != BC250_SURFACE_RESOURCE_MAGIC) return false;
    if (bytes == sizeof(BC250_SURFACE_RESOURCE_PRIVATE) && version == BC250_SURFACE_RESOURCE_TEXTURE_VERSION) {
        memcpy(out, data, sizeof(*out));
        return true;
    }
    out->Magic = BC250_SURFACE_RESOURCE_MAGIC;
    out->Version = version;
    out->Shared = (unsigned long)(shared ? 1 : 0);
    out->Access = access;
    return true;
}

// The LB7A allocation blob the kernel driver wrote, which is where an opened surface's geometry is. The
// 32-byte v1 prefix is all the front reads; a 48-byte GDI1 blob carries the same prefix unchanged
// (driver/kmd/gdi_private.h). Returns false for anything shorter or without the magic.
inline bool DecodeAllocation(const void *data, unsigned int bytes, unsigned int *width,
                             unsigned int *height, unsigned int *pitch, unsigned int *d3dddi_format)
{
    const BC250_WDDM_ALLOCATION_PRIVATE *lb7a = (const BC250_WDDM_ALLOCATION_PRIVATE *)data;
    *width = *height = *pitch = *d3dddi_format = 0;
    if (!data || bytes < sizeof(BC250_WDDM_ALLOCATION_PRIVATE)) return false;
    if (lb7a->Magic != BC250_WDDM_ALLOCATION_PRIVATE_MAGIC) return false;
    *width = (unsigned int)lb7a->Width;
    *height = (unsigned int)lb7a->Height;
    *pitch = (unsigned int)lb7a->Pitch;
    *d3dddi_format = (unsigned int)lb7a->Format;
    return true;
}

// The map. `RecordResource` overwrites an existing entry for the same handle (a handle the runtime reused
// after a destroy) and `ForgetResource` drops one. `FindResource` COPIES the record out under the lock
// rather than returning a pointer into the table: the reader is the compositor's thread and a destroy on
// another thread must not be able to change the record while the rule is reading it.
bool RecordResource(Device *device, void *handle, const Resource &resource);
void ForgetResource(void *handle);
bool FindResource(void *handle, Resource *out);
// For the host gate: how many entries the map holds, and an empty start.
unsigned int RecordedResources();
void ResetResources();
// Open addressing, a power of two, no allocation. The compositor opens one shared surface per redirection
// surface and per DirectComposition surface, so the population is in the hundreds and every destroy frees
// its slot; a full table is named once and the surfaces that did not fit are answered FALSE.
static const unsigned int kResourceSlots = 1024;

}  // namespace bc250front

#endif
