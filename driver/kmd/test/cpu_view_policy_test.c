// Host test for driver/kmd/cpu_view_policy.h (BD-352): the one CpuVisibleOnDemand rule of the UMD (BD-210 G1) and the
// type-0 LB7A allocations, with every record class and every segment set this driver can give. The records are built
// the way their producers build them: the E26R v2/v3/v4 layouts of surface_resource_private.h, the CDD shadow and
// BD-156 staging record of gdi_admission.h (v2, Shared 1, CPU_READ), and the placement of WddmGdiRecordPolicy.
// Run by run_umd_blob.ps1 next to umd_blob_test.c (the 'blob-abi' gate of quick.ps1).
#include <stdio.h>
#include <string.h>

#include "../cpu_view_policy.h"
#include "../gdi_private.h"

static int g_failures;

#define CHECK(cond) \
    do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)

#define VRAM_SET 0x1ul
#define APERTURE_SET 0x2ul

static unsigned int V2(unsigned long* w, unsigned long shared, unsigned long access)
{
    w[0] = BC250_SURFACE_RESOURCE_MAGIC; w[1] = 2; w[2] = shared; w[3] = access;
    return 4u * (unsigned int)sizeof(unsigned long);
}

static unsigned int V3(BC250_SURFACE_RESOURCE_PRIVATE* r, unsigned long shared, unsigned long access)
{
    memset(r, 0, sizeof(*r));
    r->Magic = BC250_SURFACE_RESOURCE_MAGIC; r->Version = BC250_SURFACE_RESOURCE_TEXTURE_VERSION;
    r->Shared = shared; r->Access = access;
    r->Width = 1920; r->Height = 1080; r->MipLevels = 1; r->ArraySize = 1; r->Format = 87; r->SampleCount = 1;
    return (unsigned int)sizeof(*r);
}

// The create path of a type-0 record: WddmGdiRecordPolicy places it, and the supported set is the one wddm.c gives
// (the aperture when policy.Aperture, else the local segment). Returns the on-demand answer, or -1 when refused.
static int Decide(const void* record, unsigned int bytes, unsigned long* cls)
{
    BC250_GDI_ALLOCATION_POLICY policy;
    unsigned long set;
    if (!WddmGdiRecordPolicy(record, bytes, &policy)) return -1;
    set = policy.Aperture ? APERTURE_SET : VRAM_SET;
    *cls = Bc250CpuViewClassOfRecord(record, bytes);
    return Bc250CpuViewOnDemandAllowed(*cls, policy.CpuVisible, set, APERTURE_SET);
}

int main(void)
{
    unsigned long w[4], cls = 99;
    BC250_SURFACE_RESOURCE_PRIVATE r;
    BC250_SURFACE_GPU_ONLY_RECORD g;
    unsigned int bytes;
    unsigned long c;

    // The rule itself: only the two deferrable classes, only CPU-visible, only the aperture alone.
    for (c = 0; c <= 6; c++) {
        const int deferrable = c == BC250_CPU_VIEW_UMD_MAPPABLE || c == BC250_CPU_VIEW_LB7A_PLAIN;
        CHECK(Bc250CpuViewOnDemandAllowed(c, 1, APERTURE_SET, APERTURE_SET) == deferrable);
        CHECK(!Bc250CpuViewOnDemandAllowed(c, 0, APERTURE_SET, APERTURE_SET));
        CHECK(!Bc250CpuViewOnDemandAllowed(c, 1, VRAM_SET, APERTURE_SET));
        CHECK(!Bc250CpuViewOnDemandAllowed(c, 1, VRAM_SET | APERTURE_SET, APERTURE_SET));   // shared residency
        CHECK(!Bc250CpuViewOnDemandAllowed(c, 1, 0ul, APERTURE_SET));
        CHECK(!Bc250CpuViewOnDemandAllowed(c, 1, 0ul, 0ul));                               // no aperture offered
    }

    // LB7A_PLAIN: a shared v3 texture of the D3D12 shell's envelope, and a shared v2 record with no access bit.
    bytes = V3(&r, 1, 0);
    CHECK(Decide(&r, bytes, &cls) == 1 && cls == BC250_CPU_VIEW_LB7A_PLAIN);
    bytes = V2(w, 1, 0);
    CHECK(Decide(w, bytes, &cls) == 1 && cls == BC250_CPU_VIEW_LB7A_PLAIN);
    // The same records not shared: the local segment, so the create-time view stays (the set decides, not the class).
    bytes = V3(&r, 0, 0);
    CHECK(Decide(&r, bytes, &cls) == 0 && cls == BC250_CPU_VIEW_LB7A_PLAIN);
    bytes = V2(w, 0, 0);
    CHECK(Decide(w, bytes, &cls) == 0 && cls == BC250_CPU_VIEW_LB7A_PLAIN);

    // LB7A_CPU_READ: the CDD shadow and the BD-156 staging record (gdi_admission.h): aperture, cached, keeps its view.
    bytes = V2(w, 1, BC250_SURFACE_RESOURCE_CPU_READ);
    CHECK(Decide(w, bytes, &cls) == 0 && cls == BC250_CPU_VIEW_LB7A_CPU_READ);
    bytes = V3(&r, 1, BC250_SURFACE_RESOURCE_CPU_READ);
    CHECK(Decide(&r, bytes, &cls) == 0 && cls == BC250_CPU_VIEW_LB7A_CPU_READ);

    // LB7A_DISPLAY: PRIMARY in the aperture keeps its view; SCANOUT (+PRIMARY or +DISPLAYABLE) is local anyway.
    bytes = V2(w, 1, BC250_SURFACE_RESOURCE_PRIMARY);
    CHECK(Decide(w, bytes, &cls) == 0 && cls == BC250_CPU_VIEW_LB7A_DISPLAY);
    bytes = V3(&r, 1, BC250_SURFACE_RESOURCE_PRIMARY);
    CHECK(Decide(&r, bytes, &cls) == 0 && cls == BC250_CPU_VIEW_LB7A_DISPLAY);
    bytes = V3(&r, 1, BC250_SURFACE_RESOURCE_PRIMARY | BC250_SURFACE_RESOURCE_SCANOUT);
    CHECK(Decide(&r, bytes, &cls) == 0 && cls == BC250_CPU_VIEW_LB7A_DISPLAY);
    bytes = V3(&r, 1, BC250_SURFACE_RESOURCE_SCANOUT | BC250_SURFACE_RESOURCE_DISPLAYABLE);
    CHECK(Decide(&r, bytes, &cls) == 0 && cls == BC250_CPU_VIEW_LB7A_DISPLAY);
    // Even on an aperture-only set the display class would keep its view.
    CHECK(!Bc250CpuViewOnDemandAllowed(Bc250CpuViewClassOfRecord(&r, bytes), 1, APERTURE_SET, APERTURE_SET));

    // GPU_ONLY: the v4 private group (BD-210), shared or not: no view at all, so nothing to defer.
    CHECK(Bc250SurfaceGpuOnlyRecordInit(&g, 1));
    CHECK(Decide(&g, (unsigned int)sizeof(g), &cls) == 0 && cls == BC250_CPU_VIEW_GPU_ONLY);
    CHECK(Bc250SurfaceGpuOnlyRecordInit(&g, 0));
    CHECK(Decide(&g, (unsigned int)sizeof(g), &cls) == 0 && cls == BC250_CPU_VIEW_GPU_ONLY);

    // LEGACY: no record (a standard allocation), a blob without E26R magic, a v1 record (shared, aperture). Their CPU
    // access was never stated: the create-time view stays, also where the placement is the aperture alone.
    CHECK(Decide(NULL, 0, &cls) == 0 && cls == BC250_CPU_VIEW_LEGACY);
    w[0] = 0x12345678ul; w[1] = 2; w[2] = 1; w[3] = 0;
    CHECK(Decide(w, 16, &cls) == 0 && cls == BC250_CPU_VIEW_LEGACY);
    w[0] = BC250_SURFACE_RESOURCE_MAGIC; w[1] = 1; w[2] = 1;
    CHECK(Decide(w, 12, &cls) == 0 && cls == BC250_CPU_VIEW_LEGACY);
    CHECK(Bc250CpuViewClassOfRecord(w, 12) == BC250_CPU_VIEW_LEGACY);

    // A record the parser refuses is never a deferrable class (the create refuses it, but the class must not lie).
    bytes = V2(w, 1, 0x8ul);                                      // the v4 bit in a v2 record
    CHECK(Decide(w, bytes, &cls) == -1 && Bc250CpuViewClassOfRecord(w, bytes) == BC250_CPU_VIEW_LEGACY);
    bytes = V3(&r, 1, BC250_SURFACE_RESOURCE_SCANOUT | BC250_SURFACE_RESOURCE_CPU_READ | BC250_SURFACE_RESOURCE_PRIMARY);
    CHECK(Decide(&r, bytes, &cls) == -1 && Bc250CpuViewClassOfRecord(&r, bytes) == BC250_CPU_VIEW_LEGACY);
    CHECK(Bc250CpuViewClassOfRecord(&r, bytes - 1) == BC250_CPU_VIEW_LEGACY);

    printf("cpu view policy: %s (%d failures)\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
