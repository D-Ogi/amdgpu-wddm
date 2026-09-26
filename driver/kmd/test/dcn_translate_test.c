// Host controls for the supplied DCN address geometry. All windows are synthetic,
// including the retained 256 MiB fixture. Larger fixtures prove arithmetic only,
// not firmware support, installed RAM, usable application capacity or residency.
#include <stdio.h>
#include "../dcn_translate.h"

static int g_failures, g_checks;

#define CHECK(cond) \
    do { g_checks++; if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)

static void Geometry(unsigned long long mcBase, unsigned long long vramBase,
                     unsigned long long vramLength)
{
    const unsigned long long surfaceBytes = 1920ull * 1200ull * 4ull;    // BC250_DCNFLIP_SURFACE_BYTES's own shape
    unsigned long long physical;

    // 1. Offset zero translates to the supplied CPU-visible origin.
    CHECK(DcnTranslateCardAddress(mcBase, mcBase, vramBase, vramLength, &physical));
    CHECK(physical == vramBase);
    CHECK(DcnAddressFits(physical, vramBase, vramLength, surfaceBytes));

    // 2. An offset inside the supplied window.
    CHECK(DcnTranslateCardAddress(mcBase + 0x1000000ull, mcBase, vramBase, vramLength, &physical));
    CHECK(physical == vramBase + 0x1000000ull);
    CHECK(DcnAddressFits(physical, vramBase, vramLength, surfaceBytes));

    // 3. Below McBase: refused, *Physical zeroed rather than left whatever the caller had in it.
    physical = 0xDEADBEEFull;
    CHECK(!DcnTranslateCardAddress(mcBase - 0x1000ull, mcBase, vramBase, vramLength, &physical));
    CHECK(physical == 0);

    // 4. At, and past, the top of the carve-out: refused either way (a half-open range).
    CHECK(!DcnTranslateCardAddress(mcBase + vramLength, mcBase, vramBase, vramLength, &physical));
    CHECK(!DcnTranslateCardAddress(mcBase + vramLength + 0x1000ull, mcBase, vramBase, vramLength, &physical));

    // 5. Translates fine but is not 4 KiB aligned: DcnAddressFits refuses it, the escape's own rule.
    CHECK(DcnTranslateCardAddress(mcBase + 0x1004ull, mcBase, vramBase, vramLength, &physical));
    CHECK(!DcnAddressFits(physical, vramBase, vramLength, surfaceBytes));

    // 6. Aligned, inside the carve-out, but the fixed surface would run past its top.
    CHECK(DcnTranslateCardAddress(mcBase + vramLength - 0x1000ull, mcBase, vramBase, vramLength, &physical));
    CHECK(!DcnAddressFits(physical, vramBase, vramLength, surfaceBytes));

    // 7. A zero-length carve-out (VramStart's own zeroed state with EnableVram closed): every address refused.
    physical = 0xDEADBEEFull;
    CHECK(!DcnTranslateCardAddress(mcBase, mcBase, vramBase, 0, &physical));

    // Last complete pitched surface fits, including windows larger than 8 GiB.
    CHECK(DcnTranslateCardAddress(mcBase + vramLength - surfaceBytes,
                                 mcBase, vramBase, vramLength, &physical));
    CHECK(physical == vramBase + vramLength - surfaceBytes);
    CHECK(DcnAddressFits(physical, vramBase, vramLength, surfaceBytes));
    if (vramLength > (8ull << 30)) {
        CHECK(DcnTranslateCardAddress(mcBase + (8ull << 30), mcBase, vramBase, vramLength, &physical));
        CHECK(physical == vramBase + (8ull << 30));
        CHECK(DcnAddressFits(physical, vramBase, vramLength, surfaceBytes));
    }
}

int main(void)
{
    Geometry(0xF400000000ull, 0x270000000ull, 256ull << 20);
    Geometry(0xF400000000ull, 0x270000000ull, 8ull << 30);
    Geometry(0xE800000000ull, 0x670000000ull, 12ull << 30);
    Geometry(0xDC00000000ull, 0x1270000000ull, 16ull << 30);
    printf("dcn_translate_test: 4 synthetic geometries, %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
