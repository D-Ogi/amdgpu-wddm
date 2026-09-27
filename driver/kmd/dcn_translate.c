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

unsigned long DcnPrimaryPitch(unsigned long Width)
{
    unsigned long long pixels=((unsigned long long)Width+63ull)&~63ull;
    if (!Width || pixels>0xffffffffull/4ull) return 0;
    return (unsigned long)(pixels*4ull);
}
int DcnSurfaceBytes(unsigned long Width,unsigned long Height,unsigned long Pitch,
                    unsigned long long* Bytes)
{
    *Bytes=0;
    if (!Width || !Height || !Pitch || (Pitch&3ul) || (unsigned long long)Width*4ull>Pitch) return 0;
    *Bytes=(unsigned long long)Pitch*Height;
    return 1;
}

int DcnSharedTextureLayout(unsigned long Width,unsigned long Height,
    unsigned long* Pitch,unsigned long long* Bytes)
{
    unsigned long pitch;
    if (!Pitch || !Bytes) return 0;
    *Pitch=0;*Bytes=0;
    if (!Height || Height>0xfffffffcul) return 0;
    pitch=DcnPrimaryPitch(Width);
    if (!DcnSurfaceBytes(Width,(Height+3ul)&~3ul,pitch,Bytes)) return 0;
    *Pitch=pitch;return 1;
}

int DcnStagingLayout(unsigned long Width,unsigned long Height,
    unsigned long BytesPerPixel,unsigned long* Pitch,unsigned long long* Bytes)
{
    unsigned long long row;
    if (!Pitch || !Bytes) return 0;
    *Pitch=0;*Bytes=0;
    if (!Width || !Height || (BytesPerPixel!=1 && BytesPerPixel!=4)) return 0;
    row=((unsigned long long)Width*BytesPerPixel+3ull)&~3ull;
    if (row>0xffffffffull) return 0;
    *Pitch=(unsigned long)row;
    *Bytes=row*Height;
    return 1;
}
