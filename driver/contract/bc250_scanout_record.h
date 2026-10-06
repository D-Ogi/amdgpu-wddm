// SPDX-License-Identifier: MIT
#pragma once
// M15.14 increment 2: the E26R v3 resource record of a scan-out primary, in one builder.
//
// The compositor's D3D11 opener (driver/umd/dxvk/ddi-resource.cpp, decode_open_resource) takes exactly
// 64 bytes of E26R version 3 and validates every field of it against the allocation's own LB7A
// description. A client that writes any other shape cannot have its swap-chain buffer opened into the
// compositor's device, so the OS can never ask about the pair and no flip is possible. The fields below
// are therefore the opener's rule and not the producer's choice, which is why the producer (the D3D12
// shell's prepare_surface) and the test of the consumer share this one function.
//
// Nothing here decides placement or admits a flip: the access word asks, and the kernel driver's
// Bc250ScanoutAdmit still re-derives format, geometry, pitch, size, segment and address.
#include "../kmd/surface_resource_private.h"

// D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE and
// D3D11_TEXTURE_LAYOUT_UNDEFINED, as plain numbers: this header is included by C and by C++ units that
// do not pull in d3d11.h. decode_open_resource insists on these three and on CpuAccessFlags and
// MiscFlags of zero.
#define BC250_SCANOUT_RECORD_USAGE 0ul          /* D3D11_USAGE_DEFAULT */
#define BC250_SCANOUT_RECORD_BIND 0x28ul        /* RENDER_TARGET 0x20 | SHADER_RESOURCE 0x8 */
#define BC250_SCANOUT_RECORD_LAYOUT 0ul         /* D3D11_TEXTURE_LAYOUT_UNDEFINED */

// Width, Height and DxgiFormat are the bound image's, never chosen here. Shared stays 1: the OS
// composes this buffer again whenever a window overlaps the output, and the compositor can only open
// what the record shares.
static __inline void Bc250ScanoutRecordInit(BC250_SURFACE_RESOURCE_PRIVATE* Record,
    unsigned long Width, unsigned long Height, unsigned long DxgiFormat)
{
    if (!Record) return;
    Record->Magic = BC250_SURFACE_RESOURCE_MAGIC;
    Record->Version = BC250_SURFACE_RESOURCE_TEXTURE_VERSION;
    Record->Shared = 1ul;
    Record->Access = BC250_SURFACE_RESOURCE_PRIMARY | BC250_SURFACE_RESOURCE_SCANOUT;
    Record->Width = Width;
    Record->Height = Height;
    Record->MipLevels = 1ul;
    Record->ArraySize = 1ul;
    Record->Format = DxgiFormat;
    Record->SampleCount = 1ul;
    Record->SampleQuality = 0ul;
    Record->Usage = BC250_SCANOUT_RECORD_USAGE;
    Record->BindFlags = BC250_SCANOUT_RECORD_BIND;
    Record->CpuAccessFlags = 0ul;
    Record->MiscFlags = 0ul;
    Record->TextureLayout = BC250_SCANOUT_RECORD_LAYOUT;
}
typedef char bc250_scanout_record_size_check[sizeof(BC250_SURFACE_RESOURCE_PRIVATE)==64 ? 1 : -1];
