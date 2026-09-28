// The VidPn flip's address conversion (ADR 0011 point 3 step 3, facts M85), factored out of dcn.c so it can be
// compiled and tested on the host with no WDK header at all: plain 64-bit integers in, plain integers out.
// dcn.c's DcnFlipSourceAddress is the run-time caller; driver/kmd/test/dcn_translate_test.c is the host one
// (driver/kmd/test/run_dcn_translate.ps1 builds and runs it, then compile-checks this file again with
// driver/kmd/build.ps1's own kernel flags, so a change here cannot pass the host test and fail the real build).
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// CardAddress is what dxgkrnl's DXGKARG_SETVIDPNSOURCEADDRESS.PrimaryAddress carries for this adapter's one
// local memory segment: BaseAddress-relative numbers, i.e. McBase plus an offset into the VRAM carve-out
// (wddm.c's WddmQuerySegment4, Device->VramMcBase) - never the CPU-visible system physical address DCN's
// DCSURF_PRIMARY_SURFACE_ADDRESS wants. *Physical is set to McBase's own carve-out offset added onto VramBase
// (Device->VramPhysical), the same arithmetic facts M85 measured, run from the numbers VramStart identified at
// start (never a literal). Returns 0 (refused, *Physical left 0) when CardAddress falls outside
// [McBase, McBase + VramLength) - outside the one segment this adapter ever declares.
int DcnTranslateCardAddress(unsigned long long CardAddress, unsigned long long McBase, unsigned long long VramBase,
                            unsigned long long VramLength, unsigned long long* Physical);

// The flip target's own rule, mirrored from dcn.c's AddressAllowed in plain integers so both the escape (which
// also allows the firmware's own address, a rule this function does not know about) and the WDDM DDI path share
// one range check: Physical must be 4 KiB aligned, and the whole pitched surface (SurfaceBytes) must fit between
// it and the top of [VramBase, VramBase + VramLength).
int DcnAddressFits(unsigned long long Physical, unsigned long long VramBase, unsigned long long VramLength,
                   unsigned long long SurfaceBytes);

// Linear 32-bit scanout geometry; pitch is in bytes, extent includes row padding.
int DcnSurfaceBytes(unsigned long Width, unsigned long Height, unsigned long Pitch,
                    unsigned long long* Bytes);
unsigned long DcnPrimaryPitch(unsigned long Width);

// LB7A shared-texture layout accepted by the DWM UMD's OpenResource checks.
int DcnSharedTextureLayout(unsigned long Width, unsigned long Height,
    unsigned long* Pitch, unsigned long long* Bytes);

// Linear GDI staging; AlignmentShift=2, A8 or 32-bit pixels.
int DcnStagingLayout(unsigned long Width, unsigned long Height,
    unsigned long BytesPerPixel, unsigned long* Pitch, unsigned long long* Bytes);

#ifdef __cplusplus
}
#endif
