// Project adapter: PolyForm Noncommercial 1.0.0, see LICENSE.md and NOTICE.
#pragma once

enum BC250_LINEAR_FORMAT {
    BC250_LINEAR_BGRA8, BC250_LINEAR_RGBA8, BC250_LINEAR_RGB10A2,
    BC250_LINEAR_AMD_AYCRCB8, BC250_LINEAR_BGRX8, BC250_LINEAR_FORMAT_COUNT
};
typedef struct BC250_SURFACE_LAYOUT_REQUEST {
    unsigned Format, Width, Height, PitchBytes;
    unsigned Rotation, MirrorH, MirrorV; // Zero until the DCN201 path is proven.
    unsigned long long AllocationMcAddress, AllocationBytes, SurfaceOffset;
    unsigned long long McBase, VramPhysical, VramLength;
} BC250_SURFACE_LAYOUT_REQUEST;
typedef struct BC250_SURFACE_LAYOUT {
    // Prepared field values, not whole-register overwrite permissions.
    unsigned long Pitch, PitchC, SurfaceConfig, Crossbar;
    unsigned long AddressHigh, AddressLow;
    unsigned long long Physical, SurfaceBytes;
    unsigned Format, Width, Height, PitchBytes, HasPixelAlpha, Yuv, Ready;
} BC250_SURFACE_LAYOUT;

// CPU-only linear/uncompressed, mono progressive layout preparation. The owner
// supplies trusted allocation/segment bounds and retains residency through
// scanout retirement. Input/output can overlap; failure clears all output.
// MC->UMA conversion and the complete pitched range are checked. Alignment is
// the existing 4 KiB scanout policy; linear pitch is 32 pixels (AMD plane_size).
// No MMIO, format admission, DML, CSC, tiling, enable, latch or capability claim.
// AMD_AYCRCB8 names AMD's encoding only: mapping to DXGI_FORMAT_AYUV is not
// established. It is a preparation candidate, not proven hardware support.
// Other layout types need explicit implementations, not reinterpretation here.
int Bc250SurfaceLayoutPrepare(const BC250_SURFACE_LAYOUT_REQUEST* Request,
                             BC250_SURFACE_LAYOUT* Plan);

// Separate type: the existing graphics-address writer cannot consume this plan.
// One trusted, resident allocation contains two disjoint pitched component ranges.
// Offsets and pitches come from its actual GPU layout, not the staging layout.
typedef struct BC250_NV12_LAYOUT_REQUEST {
    unsigned Width, Height, PitchBytes, ChromaPitchBytes;
    unsigned long long AllocationMcAddress, AllocationBytes, SurfaceOffset, ChromaOffset;
    unsigned long long McBase, VramPhysical, VramLength;
} BC250_NV12_LAYOUT_REQUEST;
typedef struct BC250_NV12_LAYOUT {
    unsigned long Pitch, PitchC, SurfaceConfig, Crossbar;
    unsigned long AddressHigh, AddressLow, ChromaAddressHigh, ChromaAddressLow;
    unsigned long long Physical, ChromaPhysical, SurfaceBytes, ChromaBytes;
    unsigned Width, Height, PitchBytes, ChromaPitchBytes, Ready;
} BC250_NV12_LAYOUT;

// CPU-only, progressive linear NV12: even dimensions, 256-byte component
// pitches, existing 4 KiB scanout alignment on both addresses, no DCC.
// Chroma pitch is in bytes at this boundary, two-byte UV samples in hardware.
// No inferred chroma offset. Each full pitched range must fit the allocation
// and 48-bit UMA segment, with no overlap. Gaps and reversed placement work.
// Input/output can overlap; failure clears all output. This prepares fields
// only, not the video address sequence, CNVC/CSC, capability or hardware proof.
int Bc250Nv12LayoutPrepare(const BC250_NV12_LAYOUT_REQUEST* Request,
                          BC250_NV12_LAYOUT* Plan);
