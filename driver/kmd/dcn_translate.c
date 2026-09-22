// See dcn_translate.h. No WDK header, no driver header: this file must stay compilable as ordinary C on the
// host (driver/kmd/test/run_dcn_translate.ps1) as well as with driver/kmd/build.ps1's kernel flags.
#include "dcn_translate.h"

int DcnTranslateCardAddress(unsigned long long CardAddress, unsigned long long McBase, unsigned long long VramBase,
                            unsigned long long VramLength, unsigned long long* Physical)
{
    unsigned long long offset;

    *Physical = 0;
    if (CardAddress < McBase) return 0;
    offset = CardAddress - McBase;
    if (offset >= VramLength) return 0;
    *Physical = VramBase + offset;
    return 1;
}

int DcnAddressFits(unsigned long long Physical, unsigned long long VramBase, unsigned long long VramLength,
                   unsigned long long SurfaceBytes)
{
    unsigned long long vramTop = VramBase + VramLength;

    if ((Physical & 0xFFFull) != 0) return 0;
    if (Physical < VramBase || Physical >= vramTop) return 0;
    return SurfaceBytes <= vramTop - Physical;
}
