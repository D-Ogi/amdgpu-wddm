/* Host test for BD-341: the firmware scan-out size that a start without a firmware display keeps clear at the bottom
 * of VRAM. The production bodies of modeset.c ModesetFirmwareScanoutBytes (amdgpu's gmc_v10_0_get_vbios_fb_size) and
 * vram.c VramFramebufferExtent (the 8 MiB rule of amdgpu_gmc_get_vbios_allocations) are compiled unchanged.
 * run_cold_scanout.ps1 writes cold_scanout_actual.inc from the two sources. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef unsigned long ULONG;
typedef unsigned long long ULONGLONG;
typedef long long LONGLONG;
typedef unsigned char BOOLEAN;
#define TRUE 1
#define FALSE 0
#include <sal.h>

#include "regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"
#include "display_io.h"
#include "display_start.h"

typedef struct { LONGLONG QuadPart; } PHYSICAL_ADDRESS;
typedef struct { ULONG Width, Height, Pitch; PHYSICAL_ADDRESS PhysicAddress; } FAKE_POST;
typedef struct {
    unsigned long DisplayStart;
    ULONG ColdScanoutBytes;
    ULONGLONG VramLength;
    FAKE_POST Post;
} BC250_DEVICE;

static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

/* vram.c: the NO_POST range starts at offset 0 (vram_geometry_test.c runs the production body of this one). */
static BOOLEAN VramFramebufferOffset(const BC250_DEVICE* Device, ULONGLONG* Offset)
{
    *Offset = 0;
    return Device->DisplayStart == BC250_DISPLAY_START_NO_POST;
}

#include "cold_scanout_actual.inc"

static ULONG Viewport(ULONG Width, ULONG Height)
{
    return ((Height << HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION__PRI_VIEWPORT_HEIGHT__SHIFT) &
            HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION__PRI_VIEWPORT_HEIGHT_MASK) |
           ((Width << HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION__PRI_VIEWPORT_WIDTH__SHIFT) &
            HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION__PRI_VIEWPORT_WIDTH_MASK);
}

static ULONGLONG Reserve(ULONGLONG VramLength, ULONG Bytes)
{
    BC250_DEVICE d;
    ULONGLONG offset = 1, bytes = 1;
    memset(&d, 0, sizeof(d));
    d.DisplayStart = BC250_DISPLAY_START_NO_POST;
    d.VramLength = VramLength;
    d.ColdScanoutBytes = Bytes;
    CHECK(VramFramebufferExtent(&d, &offset, &bytes) && offset == 0);
    return bytes;
}

int main(void)
{
    const ULONG enable = BC250_FIELD_D1VGA_CONTROL__D1VGA_MODE_ENABLE_MASK;
    const ULONGLONG vram = 8ull << 30;

    /* The field comes from the DCN 2.0.0 sh_mask header that gmc_v10_0.c includes, through gen_regs.py. */
    CHECK(enable != 0 && (enable & (enable - 1)) == 0);

    /* VGA engine on: the VGA allocation, whatever HUBP0 holds. */
    CHECK(ModesetFirmwareScanoutBytes(enable, 0, 0) == BC250_DISPLAY_START_VGA_ALLOCATION);
    CHECK(ModesetFirmwareScanoutBytes(enable | 0xFFFFFFFEul, Viewport(3840, 2160), 3840) ==
          BC250_DISPLAY_START_VGA_ALLOCATION);
    /* VGA engine off: HUBP0's viewport height times the pitch times 4, of a pipe that is off as well. */
    CHECK(ModesetFirmwareScanoutBytes(0, Viewport(1920, 1080), 1920) == 1080ul * 1920ul * 4ul);
    CHECK(ModesetFirmwareScanoutBytes(0xFFFFFFFEul, Viewport(1920, 1080), 1920) == 1080ul * 1920ul * 4ul);
    CHECK(ModesetFirmwareScanoutBytes(0, Viewport(1024, 768), 1024ul | ~(ULONG)HUBPREQ0_DCSURF_SURFACE_PITCH__PITCH_MASK) ==
          768ul * 1024ul * 4ul);
    /* A pipe that was never programmed reserves nothing. */
    CHECK(ModesetFirmwareScanoutBytes(0, 0, 0) == 0);
    CHECK(ModesetFirmwareScanoutBytes(0, Viewport(1920, 0), 1920) == 0);
    CHECK(ModesetFirmwareScanoutBytes(0, Viewport(1920, 1080), 0) == 0);
    /* The largest fields do not overflow 32 bits. */
    CHECK(ModesetFirmwareScanoutBytes(0, 0xFFFFFFFFul, 0xFFFFFFFFul) == 0x3FFFul * 0x3FFFul * 4ul);

    /* The layout keeps the size, and keeps none when less than 8 MiB of VRAM would remain. */
    CHECK(Reserve(vram, BC250_DISPLAY_START_VGA_ALLOCATION) == BC250_DISPLAY_START_VGA_ALLOCATION);
    CHECK(Reserve(vram, 1080ul * 1920ul * 4ul) == 1080ull * 1920 * 4);
    CHECK(Reserve(vram, 0) == 0);
    CHECK(Reserve(BC250_DISPLAY_START_VGA_ALLOCATION + BC250_DISPLAY_START_MIN_FREE, BC250_DISPLAY_START_VGA_ALLOCATION) ==
          BC250_DISPLAY_START_VGA_ALLOCATION);
    CHECK(Reserve(BC250_DISPLAY_START_VGA_ALLOCATION + BC250_DISPLAY_START_MIN_FREE - 1,
                  BC250_DISPLAY_START_VGA_ALLOCATION) == 0);
    CHECK(Reserve(4ull << 20, BC250_DISPLAY_START_VGA_ALLOCATION) == 0);

    printf("cold scan-out: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
