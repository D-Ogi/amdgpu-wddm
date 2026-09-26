// Host test for driver/kmd/dcn_translate.c (ADR 0011 point 3 step 3): the VidPn flip's address conversion and
// its range check. The numbers below are a made-up carve-out of the same shape as facts M31/M85 (a 256 MB
// window starting where VramStart's own arithmetic would put it) - stand-ins, not a second copy of the fact,
// so this file never contradicts docs/facts.md. No WDK header, no driver code: run_dcn_translate.ps1 in this
// directory compiles and links this file with dcn_translate.c alone.
#include <stdio.h>
#include "../dcn_translate.h"

static int g_failures;

#define CHECK(cond) \
    do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)

int main(void)
{
    const unsigned long long mcBase = 0xF400000000ull;
    const unsigned long long vramBase = 0x270000000ull;
    const unsigned long long vramLength = 0x10000000ull;          // 256 MB
    const unsigned long long surfaceBytes = 1920ull * 1200ull * 4ull;    // BC250_DCNFLIP_SURFACE_BYTES's own shape
    unsigned long long physical;

    // 1. Offset 0: the firmware's own address (M85's 0x270000000 <- MC 0xF400000000).
    CHECK(DcnTranslateCardAddress(mcBase, mcBase, vramBase, vramLength, &physical));
    CHECK(physical == vramBase);
    CHECK(DcnAddressFits(physical, vramBase, vramLength, surfaceBytes));

    // 2. An offset inside the carve-out (the shape of M94's fill target, MC + 0x1000000).
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

    if (g_failures == 0) { printf("dcn_translate_test: all cases passed\n"); return 0; }
    fprintf(stderr, "dcn_translate_test: %d case(s) failed\n", g_failures);
    return 1;
}
