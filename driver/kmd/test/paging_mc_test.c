// Host test for driver/kmd/paging_mc.c. The conversion has to be the inverse of the already-shipped
// DcnTranslateCardAddress (dcn_translate.c): that function is tested on its own, so a round trip
// through both is a check of this one against code that does not live in this file. The carve-out
// numbers are the same stand-ins dcn_translate_test.c uses, not a second copy of facts M31.
#include <stdio.h>
#include "../paging_mc.h"
#include "../dcn_translate.h"

static int g_failures, g_checks;

#define CHECK(cond) \
    do { g_checks++; if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)

static void RoundTrip(unsigned long long mc, unsigned long long mcBase, unsigned long long vramBase,
                      unsigned long long vramLength, unsigned long long bytes)
{
    unsigned long long physical = 0xDEADBEEFull;
    unsigned long long back = 0xDEADBEEFull;

    CHECK(DcnTranslateCardAddress(mc, mcBase, vramBase, vramLength, &physical));
    CHECK(PagingPhysicalToMc(physical, bytes, vramBase, mcBase, vramLength, &back));
    CHECK(back == mc);
}

static void Geometry(unsigned long long mcBase, unsigned long long vramBase,
                     unsigned long long vramLength)
{
    unsigned long long mc = 0xDEADBEEFull;

    // 1-3. Byte 0, a page inside, and the last byte of the window all come back as the MC they started from.
    RoundTrip(mcBase, mcBase, vramBase, vramLength, 1);
    RoundTrip(mcBase + 0x1C62000ull, mcBase, vramBase, vramLength, 4096);
    RoundTrip(mcBase + vramLength - 1ull, mcBase, vramBase, vramLength, 1);

    // 4. A run that starts inside and ends past the top is refused, and *mc is cleared.
    CHECK(!PagingPhysicalToMc(vramBase + vramLength - 0x1000ull, 0x1001ull, vramBase, mcBase, vramLength, &mc));
    CHECK(mc == 0);

    // 5. Below the carve-out, at the top, and a zero length: refused.
    mc = 0xDEADBEEFull;
    CHECK(!PagingPhysicalToMc(vramBase - 1ull, 1, vramBase, mcBase, vramLength, &mc));
    CHECK(mc == 0);
    CHECK(!PagingPhysicalToMc(vramBase + vramLength, 1, vramBase, mcBase, vramLength, &mc));
    CHECK(!PagingPhysicalToMc(vramBase, 0, vramBase, mcBase, vramLength, &mc));

    // 6. A zero-length window (VramStart not run) refuses everything.
    mc = 0xDEADBEEFull;
    CHECK(!PagingPhysicalToMc(vramBase, 1, vramBase, mcBase, 0, &mc));
    CHECK(mc == 0);

    // 7. The page offset survives: physical base+0x1004 is MC base+0x1004, not a truncated page.
    CHECK(PagingPhysicalToMc(vramBase + 0x1004ull, 4, vramBase, mcBase, vramLength, &mc));
    CHECK(mc == mcBase + 0x1004ull);

    // A whole page above the old 8 GiB ceiling must remain addressable.
    if (vramLength > (8ull << 30))
        RoundTrip(mcBase + (8ull << 30), mcBase, vramBase, vramLength, 4096);
}

int main(void)
{
    // Synthetic windows: varying both origins prevents the old base from becoming an assumption.
    Geometry(0xF400000000ull, 0x270000000ull, 8ull << 30);
    Geometry(0xE800000000ull, 0x670000000ull, 12ull << 30);
    Geometry(0xDC00000000ull, 0x1270000000ull, 16ull << 30);
    printf("paging_mc_test: 3 synthetic geometries, %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
