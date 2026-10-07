// Host test of display_modes.h (0.7.201): the pixel formats of the VidPN source modes display.c offers, the set
// CommitVidPn and IsSupportedVidPn accept, and the stride of each mode. Built with no WDK header by
// run_display_modes.ps1. The D3DDDIFORMAT numbers below are checked against d3dukmdt.h by wddm.c's C_ASSERTs on
// the shared table; here they only name the expected rows.
#include <stdio.h>
#include "../display_modes.h"

static int g_failures, g_checks;

#define CHECK(cond) \
    do { g_checks++; if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)

#define FMT_A8 28ul
#define FMT_A8R8G8B8 21ul
#define FMT_X8R8G8B8 22ul
#define FMT_A2B10G10R10 31ul
#define FMT_A8B8G8R8 32ul
#define FMT_X8B8G8R8 33ul
#define FMT_A2R10G10B10 35ul
#define FMT_A16B16G16R16F 113ul

int main(void)
{
    unsigned long f[BC250_SOURCE_MODE_MAX];
    unsigned long n, i, j;

    // Display-only table (and the OfferComposedSourceModes switch at 0): the one mode of 0.7.200.1.
    n = Bc250SourceModeFormats(0, f, BC250_SOURCE_MODE_MAX);
    CHECK(n == 1);
    CHECK(f[0] == FMT_A8R8G8B8);

    // Full table: the scan-out format first (the desktop takes the first GDI format), then the composed formats
    // DXGI names: R8G8B8A8, R10G10B10A2, R16G16B16A16_FLOAT. X8R8G8B8 has no DXGI display format and stays out.
    // A8 is COMPOSED and DXGI names it (A8_UNORM), but one byte is not a displayable pixel, so it stays out too.
    n = Bc250SourceModeFormats(1, f, BC250_SOURCE_MODE_MAX);
    CHECK(n == 4);
    CHECK(f[0] == FMT_A8R8G8B8);
    CHECK(f[1] == FMT_A8B8G8R8);
    CHECK(f[2] == FMT_A2B10G10R10);
    CHECK(f[3] == FMT_A16B16G16R16F);
    for (i = 0; i < n; ++i)
        for (j = i + 1; j < n; ++j) CHECK(f[i] != f[j]);
    for (i = 0; i < n; ++i) {
        const AMDGPU_WDDM_SURFACE_FORMAT* row = amdgpu_wddm_surface_format_by_d3dddi((unsigned int)f[i]);
        CHECK(row != 0 && row->dxgi != 0);
        CHECK(row != 0 && (row->policy & (AMDGPU_WDDM_SURFACE_COMPOSED | AMDGPU_WDDM_SURFACE_SCANOUT_PRIMARY)) != 0);
    }

    // A short caller buffer: the count is the full count, nothing is written past Max.
    {
        unsigned long two[3] = {0, 0, 0xdeadbeeful};
        CHECK(Bc250SourceModeFormats(1, two, 2) == 4);
        CHECK(two[0] == FMT_A8R8G8B8 && two[1] == FMT_A8B8G8R8 && two[2] == 0xdeadbeeful);
    }

    // From 0.7.216.20 the flip programs the plane's pixel format (plane_format.h), so the two added 4-byte
    // formats scan out as well; the FP16 one stays composed. The list itself is the one before (FIRMWARE_PLANE
    // first, then the composed rows), so the display-only table still offers the firmware's format alone.
    CHECK(WddmSurfaceFormatBpp(f[0], BC250_SURFACE_SCANOUT) == 4);
    CHECK(WddmSurfaceFormatBpp(f[1], BC250_SURFACE_SCANOUT) == 4);
    CHECK(WddmSurfaceFormatBpp(f[2], BC250_SURFACE_SCANOUT) == 4);
    CHECK(WddmSurfaceFormatBpp(f[3], BC250_SURFACE_SCANOUT) == 0);

    // What CommitVidPn and IsSupportedVidPn accept follows the offer exactly.
    CHECK(Bc250SourceModeAdmitted(0, FMT_A8R8G8B8));
    CHECK(!Bc250SourceModeAdmitted(0, FMT_A8B8G8R8));
    CHECK(!Bc250SourceModeAdmitted(0, FMT_A2B10G10R10));
    CHECK(!Bc250SourceModeAdmitted(0, FMT_A16B16G16R16F));
    CHECK(Bc250SourceModeAdmitted(1, FMT_A8R8G8B8));
    CHECK(Bc250SourceModeAdmitted(1, FMT_A8B8G8R8));
    CHECK(Bc250SourceModeAdmitted(1, FMT_A2B10G10R10));
    CHECK(Bc250SourceModeAdmitted(1, FMT_A16B16G16R16F));
    CHECK(!Bc250SourceModeAdmitted(1, FMT_X8R8G8B8));
    CHECK(!Bc250SourceModeAdmitted(1, FMT_X8B8G8R8));
    CHECK(!Bc250SourceModeAdmitted(1, FMT_A2R10G10B10));
    CHECK(!Bc250SourceModeAdmitted(1, 0));
    // The 1-byte composed row (A8, the DirectComposition atlases of 0.7.207) is a shared surface and never a
    // source mode, with the composed table and without it. Its bytes are still 1 at the composed stage.
    CHECK(!Bc250SourceModeAdmitted(0, FMT_A8));
    CHECK(!Bc250SourceModeAdmitted(1, FMT_A8));
    CHECK(WddmSurfaceFormatBpp(FMT_A8, BC250_SURFACE_COMPOSED) == 1);
    CHECK(WddmSurfaceFormatBpp(FMT_A8, BC250_SURFACE_SCANOUT) == 0);
    {
        const AMDGPU_WDDM_SURFACE_FORMAT* row = amdgpu_wddm_surface_format_by_d3dddi(FMT_A8);
        CHECK(row != 0 && row->dxgi != 0 && row->bytes_per_pixel == 1);
        CHECK(row != 0 && amdgpu_wddm_surface_admit(row, AMDGPU_WDDM_SURFACE_COMPOSED) != 0);
    }

    // Stride: the firmware pitch at 4 bytes a pixel, scaled to the row's size. 1920 pixels with the lab's
    // 7680-byte pitch, and a padded pitch of 7936 (1984 pixels).
    CHECK(Bc250SourceModeStride(FMT_A8R8G8B8, 7680) == 7680);
    CHECK(Bc250SourceModeStride(FMT_A8B8G8R8, 7680) == 7680);
    CHECK(Bc250SourceModeStride(FMT_A2B10G10R10, 7680) == 7680);
    CHECK(Bc250SourceModeStride(FMT_A16B16G16R16F, 7680) == 15360);
    CHECK(Bc250SourceModeStride(FMT_A16B16G16R16F, 7936) == 15872);
    CHECK(Bc250SourceModeStride(FMT_A8R8G8B8, 0) == 0);
    CHECK(Bc250SourceModeStride(FMT_A8R8G8B8, 7681) == 0);
    CHECK(Bc250SourceModeStride(FMT_A2R10G10B10, 7680) == 0);
    /* A8 is the one row the size bound alone keeps out of the mode list, so it is the one row whose stride could
     * disagree with the offer. It must not: a stride of a format the plane cannot show is a row size a caller
     * would take for a mode. The stride answers for X8R8G8B8 and X8B8G8R8, which are 4-byte rows with no DXGI
     * display format, and that is the offer's own reason for leaving them out, not a size. */
    CHECK(Bc250SourceModeStride(FMT_A8, 7680) == 0);
    CHECK(Bc250SourceModeStride(FMT_A8, 256) == 0);
    for (i = 0; i < 256; ++i) {
        const AMDGPU_WDDM_SURFACE_FORMAT* row = amdgpu_wddm_surface_format_by_d3dddi((unsigned int)i);
        if (Bc250SourceModeStride(i, 7680) != 0) CHECK(row != 0 && row->bytes_per_pixel >= 4);
        if (row != 0 && row->bytes_per_pixel < 4) CHECK(Bc250SourceModeStride(i, 7680) == 0);
    }
    CHECK(Bc250SourceModeStride(FMT_A16B16G16R16F, 0xfffffffcul) == 0);
    CHECK(Bc250SourceModeStride(FMT_A16B16G16R16F, 0x7ffffffcul) == 0xfffffff8ul);

    printf("display_modes_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
