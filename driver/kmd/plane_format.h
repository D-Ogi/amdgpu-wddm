#ifndef BC250_PLANE_FORMAT_H
#define BC250_PLANE_FORMAT_H
#include "surface_format.h"

// M15.14: the pixel format the display plane reads a scanned-out surface in, per D3DDDIFORMAT.
//
// Until 0.7.216.18 the flip programmed an address and a pitch and never a pixel format, so the plane read every
// surface as the firmware's ARGB8888 (DXGI B8G8R8A8). That was right only for the BGRA8 rows, and it is why
// the shared table marked only those SCANOUT_PRIMARY. A game's R8G8B8A8 swap chain (The Witcher 3, lab session
// 458) and any R10G10B10A2 one were therefore composed on every frame.
//
// Plain integers and no WDK header, like scanout_admit.h, so the host tests compile the production table. The
// register fields themselves are composed in dcn.c (DcnPlaneFormatRegisters) from the AMD masks of
// dcn_2_0_1_sh_mask.h. This header gives only the field values, and every value is AMD's, for DCN 2.0.1:
//
//   HUBP0_DCSURF_SURFACE_CONFIG.SURFACE_PIXEL_FORMAT and HUBPRET0_HUBPRET_CONTROL.CROSSBAR_SRC_CB_B /
//   CROSSBAR_SRC_CR_R: hubp1_program_pixel_format (linux v6.18 drivers/gpu/drm/amd/display/dc/hubp/dcn10/
//   dcn10_hubp.c:236-300), which dcn201_hubp.c:55 calls for this DCN. Red bar 3 and blue bar 2 by default;
//   2 and 3 ("swap for ABGR format") for ABGR8888, ABGR2101010 and ABGR16161616F. Format 8 is ARGB8888 and
//   ABGR8888, 10 is ARGB2101010 and ABGR2101010, 24 is the two FP16 orders.
//   CNVC_CFG0_CNVC_SURFACE_PIXEL_FORMAT: dpp201_cnv_setup (dc/dpp/dcn201/dcn201_dpp.c:44-176). 8 for both
//   8-bit orders, 10 for both 10-bit orders, 24 for ARGB16161616F and 25 for ABGR16161616F. dcn20 calls it
//   from dcn20_update_dchubp_dpp (dc/hwss/dcn20/dcn20_hwseq.c:1747) with the plane's format.
//
// AMD's names list channels from the highest bits, as D3DDDIFORMAT does: SURFACE_PIXEL_FORMAT_GRPH_ABGR8888
// is DRM_FORMAT_ABGR8888, red in the lowest byte, which is D3DDDIFMT_A8B8G8R8 and DXGI R8G8B8A8_UNORM.
// The firmware's plane reads 8 and crossbar 0x00E40000 on unit A (HUBPRET_CONTROL CROSSBAR_SRC_CR_R 3,
// CB_B 2, Y_G 1, ALPHA 0; evidence/linux/2026-09-22-E21-linux-reference-4/dmupre.txt and the Windows read of
// E22 run 001), which is AMD's ARGB8888 encoding.
//
// The same-size rule. All three admitted formats are 4 bytes a pixel, and AMD's bandwidth model treats them as
// one source format: dcn20_hubbub.c:148-151 gives 4 bytes an element for all of them and dcn20_fpu.c:1716 maps
// them to dm_444_32. So the request, deadline and watermark settings the firmware programmed for its ARGB8888
// plane stay valid, and the flip changes the four fields above and nothing else.
//
// Where FP16 slots in, and why it is not admitted yet. A16B16G16R16F would be BC250_PLANE_FORMAT_ABGR16161616F
// with SURFACE_PIXEL_FORMAT 24, CNVC 25 and the crossbar swap, at 8 bytes a pixel (the pitch field counts
// pixels, which DcnFlipWriteSequence already divides by the plane's bytes per pixel). Two things block it, and
// both are named follow-ups in docs/design/scanout-admission.md:
//   1. Bandwidth. dcn20_fpu.c:1703 maps it to dm_444_64. DML then gives other request sizes and swath heights
//      (HUBP0_DCHUBP_REQ_SIZE_CONFIG), other deadlines (HUBPREQ0 DLG/TTU) and other watermarks. This driver does
//      not run DML; it keeps the firmware's 32-bit values, which do not describe a 64-bit plane.
//   2. Transfer function. A DXGI FP16 swap chain holds linear scRGB values. The firmware's pipe applies no
//      degamma and no output gamma to its sRGB-encoded ARGB8888 plane, so linear values would be shown as if
//      they were sRGB-encoded. An sRGB output gamma (MPCC_OGAM) and a clamp of values above 1.0 are needed.
// The colour space itself never reaches this driver for a flip: DXGKARG_SETVIDPNSOURCEADDRESS has no colour
// space. Only the multiplane overlay DDI carries one (DXGK_MULTIPLANE_OVERLAY_ATTRIBUTES3.ColorSpaceType,
// ref/ddi-display/d3dkmddi.md:23606), and this driver does not implement that DDI.
#define BC250_PLANE_FORMAT_NONE 0ul          // no plane encoding: refused at the format clause
#define BC250_PLANE_FORMAT_ARGB8888 1ul      // DXGI B8G8R8A8, D3DDDI A8R8G8B8 and X8R8G8B8: the firmware's own
#define BC250_PLANE_FORMAT_ABGR8888 2ul      // DXGI R8G8B8A8, D3DDDI A8B8G8R8
#define BC250_PLANE_FORMAT_ABGR2101010 3ul   // DXGI R10G10B10A2, D3DDDI A2B10G10R10
#define BC250_PLANE_FORMATS 4ul

typedef struct _BC250_PLANE_ENCODING {
    unsigned long HubpPixelFormat;      // HUBP0_DCSURF_SURFACE_CONFIG.SURFACE_PIXEL_FORMAT
    unsigned long CnvcPixelFormat;      // CNVC_CFG0_CNVC_SURFACE_PIXEL_FORMAT.CNVC_SURFACE_PIXEL_FORMAT
    unsigned long CrossbarCbB;          // HUBPRET0_HUBPRET_CONTROL.CROSSBAR_SRC_CB_B
    unsigned long CrossbarCrR;          // HUBPRET0_HUBPRET_CONTROL.CROSSBAR_SRC_CR_R
    unsigned long BytesPerPixel;        // the unit of HUBPREQ0_DCSURF_SURFACE_PITCH.PITCH
} BC250_PLANE_ENCODING;

// The three whole register values a flip writes for a format change (dcn.c's DcnPlaneFormatRegisters composes
// them from the firmware's values with the format fields replaced, so every other bit stays the firmware's).
typedef struct _BC250_PLANE_REGISTERS {
    unsigned long SurfaceConfig;        // HUBP0_DCSURF_SURFACE_CONFIG
    unsigned long HubpretControl;       // HUBPRET0_HUBPRET_CONTROL
    unsigned long CnvcFormat;           // CNVC_CFG0_CNVC_SURFACE_PIXEL_FORMAT
} BC250_PLANE_REGISTERS;

static const BC250_PLANE_ENCODING g_Bc250PlaneEncodings[BC250_PLANE_FORMATS] = {
    { 0, 0, 0, 0, 0 },                  // NONE
    { 8, 8, 2, 3, 4 },                  // ARGB8888
    { 8, 8, 3, 2, 4 },                  // ABGR8888: hubp1 "swap for ABGR format"
    { 10, 10, 3, 2, 4 },                // ABGR2101010
};

static const char* const g_Bc250PlaneFormatNames[BC250_PLANE_FORMATS] = {
    "none", "argb8888", "abgr8888", "abgr2101010" };

// The plane format of a D3DDDIFORMAT, or BC250_PLANE_FORMAT_NONE. Keyed by the table's own D3DDDIFORMAT
// numbers, so a row the table renumbers cannot keep a stale encoding.
static __inline unsigned long Bc250PlaneFormatOf(unsigned long Format)
{
    switch (Format) {
    case AMDGPU_WDDM_D3DDDI_A8R8G8B8:
    case AMDGPU_WDDM_D3DDDI_X8R8G8B8: return BC250_PLANE_FORMAT_ARGB8888;
    case AMDGPU_WDDM_D3DDDI_A8B8G8R8: return BC250_PLANE_FORMAT_ABGR8888;
    case AMDGPU_WDDM_D3DDDI_A2B10G10R10: return BC250_PLANE_FORMAT_ABGR2101010;
    default: return BC250_PLANE_FORMAT_NONE;
    }
}

static __inline const BC250_PLANE_ENCODING* Bc250PlaneEncoding(unsigned long PlaneFormat)
{
    return PlaneFormat > BC250_PLANE_FORMAT_NONE && PlaneFormat < BC250_PLANE_FORMATS ?
        &g_Bc250PlaneEncodings[PlaneFormat] : 0;
}

static __inline const char* Bc250PlaneFormatText(unsigned long PlaneFormat)
{
    return PlaneFormat < BC250_PLANE_FORMATS ? g_Bc250PlaneFormatNames[PlaneFormat] : "unknown";
}

// The plane format the four fields name, or BC250_PLANE_FORMAT_NONE when no encoding of this header has
// exactly those values. dcn.c decodes the firmware's registers with it at start: only a firmware plane that
// decodes to ARGB8888 lets the driver program other formats, because restoring means writing those registers
// back, and a value this header does not know cannot be called "the firmware's BGRA8".
static __inline unsigned long Bc250PlaneFormatDecode(unsigned long HubpPixelFormat, unsigned long CnvcPixelFormat,
    unsigned long CrossbarCbB, unsigned long CrossbarCrR)
{
    unsigned long i;
    for (i = 1; i < BC250_PLANE_FORMATS; ++i) {
        const BC250_PLANE_ENCODING* e = &g_Bc250PlaneEncodings[i];
        if (e->HubpPixelFormat == HubpPixelFormat && e->CnvcPixelFormat == CnvcPixelFormat &&
            e->CrossbarCbB == CrossbarCbB && e->CrossbarCrR == CrossbarCrR) return i;
    }
    return BC250_PLANE_FORMAT_NONE;
}
#endif
