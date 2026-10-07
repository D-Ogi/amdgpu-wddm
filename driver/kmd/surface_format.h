#ifndef BC250_SURFACE_FORMAT_H
#define BC250_SURFACE_FORMAT_H
#include "../contract/amdgpu_wddm_surface_format.h"

// The KMD's admission of a surface format, over the shared table in driver/contract, which alone says what a
// format is enabled for. Plain integers, so the host tests compile it without a WDK header; wddm.c checks the
// values against D3DDDIFORMAT with C_ASSERT. The stages are the KMD's:
//   Composed  a received LB7A blob of GDI type 0: the table's COMPOSED rows (A8R8G8B8, A8B8G8R8, A2B10G10R10 at
//             4 bytes, A16B16G16R16F at 8, and A8 at 1 for DirectComposition's shared atlases;
//             gdi_private.h sizes the row by the returned bytes, never by 4). A8 reaches the hardware at
//             its 1 byte a pixel since 0.7.207 (M14.1): DcnLinearSurfaceBytes (dcn_translate.c) takes 1,
//             4 and 8 bytes a pixel, so WddmSurfaceAdmitted admits a type-0 A8 blob. Bc250SourceModeFormats
//             (display_modes.h) still skips rows under 4 bytes, so A8 is never a VidPN source mode.
//             dxgkrnl's own standard allocations (shared primary, shadow, staging) arrive as type 0 as well, so
//             the SCANOUT_PRIMARY rows (X8R8G8B8) and X8B8G8R8, which the table does not describe, stay
//             admitted here exactly as before the table.
//   Gdi       a standard GDI surface, types 1..8. The table has no GDI stage; the set is the one before it
//             (A8 at 1 byte, the 32-bit formats at 4).
//   Scanout   the allocation of SetVidPnSourceAddress: the table's SCANOUT_PRIMARY rows (A8R8G8B8, X8R8G8B8,
//             and from 0.7.216.20 A8B8G8R8 and A2B10G10R10). The flip programs the plane's address, pitch and
//             pixel format (dcn.c, plane_format.h); scanout_admit.h also requires a plane encoding.
#define BC250_SURFACE_COMPOSED 0x1ul
#define BC250_SURFACE_GDI 0x2ul
#define BC250_SURFACE_SCANOUT 0x4ul

// Formats the KMD admits at a stage the shared table does not describe. A8 has a COMPOSED row in the table,
// but no GDI stage, so the KMD's own pre-table answer for the GDI types stands.
#define BC250_FORMAT_X8B8G8R8 33ul

// Bytes per pixel of Format at Stage, or 0 when the format is unknown or not admitted there.
static __inline unsigned long WddmSurfaceFormatBpp(unsigned long Format,unsigned long Stage)
{
    const AMDGPU_WDDM_SURFACE_FORMAT* row=amdgpu_wddm_surface_format_by_d3dddi((unsigned int)Format);
    const AMDGPU_WDDM_SURFACE_FORMAT* admitted=0;
    switch (Stage) {
    case BC250_SURFACE_COMPOSED:
        admitted=amdgpu_wddm_surface_admit(row,AMDGPU_WDDM_SURFACE_COMPOSED);
        if (!admitted) admitted=amdgpu_wddm_surface_admit(row,AMDGPU_WDDM_SURFACE_SCANOUT_PRIMARY);
        if (!admitted) return Format==BC250_FORMAT_X8B8G8R8 ? 4 : 0;
        break;
    case BC250_SURFACE_GDI:
        if (Format==AMDGPU_WDDM_D3DDDI_A8) return 1;
        return Format==AMDGPU_WDDM_D3DDDI_A8R8G8B8 || Format==AMDGPU_WDDM_D3DDDI_X8R8G8B8 ||
            Format==AMDGPU_WDDM_D3DDDI_A8B8G8R8 || Format==BC250_FORMAT_X8B8G8R8 ? 4 : 0;
    case BC250_SURFACE_SCANOUT:
        admitted=amdgpu_wddm_surface_admit(row,AMDGPU_WDDM_SURFACE_SCANOUT_PRIMARY);
        break;
    }
    return admitted ? admitted->bytes_per_pixel : 0;
}
#endif
