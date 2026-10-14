/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The boot-screen mascot: see boot_logo.h for the contract and docs/design/boot-logo.md for the design.
 * Plain C, no WDK header, no allocation, no floating point, no division by a value this file did not check. */
#include "boot_logo.h"
#include "boot_logo_asset.inc"      /* generated; the only place it may be included */

const BC250_BOOT_LOGO* Bc250BootLogoAsset(void) { return &Bc250BootLogoNagi; }

#define BC250_LOGO_MAX_SCALE 3ul
#define BC250_LOGO_SCALE_STEP 600ul             /* one more enlargement for every 600 rows of the surface */
#define BC250_LOGO_ANCHOR_MIN 100ul
#define BC250_LOGO_ANCHOR_MAX 900ul
/* The surface sizes this code is willing to address. 16384 squared times four bytes stays far inside the
 * 32-bit arithmetic below, and no display timing of this part comes near it. */
#define BC250_LOGO_MAX_DIMENSION 16384ul

static unsigned long Blend(unsigned long Source, unsigned long Destination, unsigned long Alpha)
{
    /* Source over destination with straight alpha, rounded to nearest. */
    return (Source * Alpha + Destination * (255ul - Alpha) + 127ul) / 255ul;
}

const char* Bc250BootLogoResultText(BC250_BOOT_LOGO_RESULT Result)
{
    switch (Result) {
    case Bc250LogoDrawn:      return "drawn";
    case Bc250LogoBadArgument: return "bad argument";
    case Bc250LogoBadAsset:   return "bad asset";
    case Bc250LogoNoRoom:     return "no room";
    default:                  return "unknown";
    }
}

BC250_BOOT_LOGO_RESULT Bc250BootLogoVerify(const BC250_BOOT_LOGO* Logo)
{
    unsigned long at = 0, y;

    if (Logo == 0 || Logo->Palette == 0 || Logo->Data == 0) return Bc250LogoBadArgument;
    if (Logo->Width == 0 || Logo->Height == 0 ||
        Logo->Width > BC250_LOGO_MAX_DIMENSION || Logo->Height > BC250_LOGO_MAX_DIMENSION) return Bc250LogoBadArgument;
    if (Logo->PaletteCount == 0 || Logo->PaletteCount > 256ul) return Bc250LogoBadArgument;

    for (y = 0; y < Logo->Height; y++) {
        unsigned long x = 0;
        for (;;) {
            unsigned long count, index;
            if (at >= Logo->DataBytes) return Bc250LogoBadAsset;
            count = Logo->Data[at++];
            if (count == 0) break;                          /* end of this row */
            if (at >= Logo->DataBytes) return Bc250LogoBadAsset;
            index = Logo->Data[at++];
            if (index >= Logo->PaletteCount) return Bc250LogoBadAsset;
            if (count > Logo->Width - x) return Bc250LogoBadAsset;   /* x <= Width holds below */
            x += count;
        }
        if (x != Logo->Width) return Bc250LogoBadAsset;
    }
    /* Trailing bytes would mean the asset and this decoder disagree about the picture. */
    return at == Logo->DataBytes ? Bc250LogoDrawn : Bc250LogoBadAsset;
}

BC250_BOOT_LOGO_RESULT Bc250BootLogoPlace(const BC250_BOOT_LOGO* Logo, const BC250_BOOT_LOGO_TARGET* Target,
                                          BC250_BOOT_LOGO_PLACEMENT* Placement)
{
    unsigned long scale, anchor, centre, top, band;

    if (Placement != 0) {
        Placement->Scale = Placement->Left = Placement->Top = 0;
        Placement->DrawWidth = Placement->DrawHeight = 0;
    }
    if (Logo == 0 || Target == 0 || Placement == 0) return Bc250LogoBadArgument;
    if (Logo->Width == 0 || Logo->Height == 0 ||
        Logo->Width > BC250_LOGO_MAX_DIMENSION || Logo->Height > BC250_LOGO_MAX_DIMENSION) return Bc250LogoBadArgument;
    if (Target->Width == 0 || Target->Height == 0 ||
        Target->Width > BC250_LOGO_MAX_DIMENSION || Target->Height > BC250_LOGO_MAX_DIMENSION) return Bc250LogoBadArgument;
    if (Target->Pitch < Target->Width * 4ul) return Bc250LogoBadArgument;

    anchor = Target->AnchorPermille;
    if (anchor < BC250_LOGO_ANCHOR_MIN) anchor = BC250_LOGO_ANCHOR_MIN;
    if (anchor > BC250_LOGO_ANCHOR_MAX) anchor = BC250_LOGO_ANCHOR_MAX;

    /* The band the picture may use: the top of the surface down to the line the ring starts below. */
    band = Target->Height * BC250_LOGO_BOTTOM_PERMILLE / 1000ul;
    if (band == 0ul) return Bc250LogoNoRoom;

    scale = Target->Height / BC250_LOGO_SCALE_STEP;
    if (scale < 1ul) scale = 1ul;
    if (scale > BC250_LOGO_MAX_SCALE) scale = BC250_LOGO_MAX_SCALE;
    while (scale > 1ul &&
           (Logo->Width * scale > Target->Width || Logo->Height * scale > band)) scale--;
    if (Logo->Width * scale > Target->Width || Logo->Height * scale > band) return Bc250LogoNoRoom;

    Placement->Scale = scale;
    Placement->DrawWidth = Logo->Width * scale;
    Placement->DrawHeight = Logo->Height * scale;
    Placement->Left = (Target->Width - Placement->DrawWidth) / 2ul;
    centre = Target->Height * anchor / 1000ul;
    top = centre > Placement->DrawHeight / 2ul ? centre - Placement->DrawHeight / 2ul : 0ul;
    /* The anchor may ask for a place the ring owns. The band wins: the picture only ever moves up. */
    if (top > band - Placement->DrawHeight) top = band - Placement->DrawHeight;
    Placement->Top = top;
    return Bc250LogoDrawn;
}

BC250_BOOT_LOGO_RESULT Bc250BootLogoDraw(void* Pixels, const BC250_BOOT_LOGO* Logo,
                                         const BC250_BOOT_LOGO_TARGET* Target, unsigned long* Written)
{
    BC250_BOOT_LOGO_PLACEMENT place;
    BC250_BOOT_LOGO_RESULT result;
    unsigned long at = 0, y, written = 0;
    unsigned char* base;

    if (Written != 0) *Written = 0;
    if (Pixels == 0) return Bc250LogoBadArgument;
    /* Verify before place: a sound asset is what makes the single decoding pass below safe. */
    result = Bc250BootLogoVerify(Logo);
    if (result != Bc250LogoDrawn) return result;
    result = Bc250BootLogoPlace(Logo, Target, &place);
    if (result != Bc250LogoDrawn) return result;

    base = (unsigned char*)Pixels;
    for (y = 0; y < Logo->Height; y++) {
        unsigned long x = 0;
        for (;;) {
            unsigned long count, index, a, run, line;
            const BC250_LOGO_COLOR* colour;
            count = Logo->Data[at++];
            if (count == 0) break;
            index = Logo->Data[at++];
            colour = &Logo->Palette[index];
            a = colour->A;
            if (a == 0) { x += count; continue; }           /* transparent: the destination keeps its pixel */
            for (line = 0; line < place.Scale; line++) {
                unsigned char* row = base +
                    (unsigned long long)(place.Top + y * place.Scale + line) * Target->Pitch +
                    (unsigned long long)(place.Left + x * place.Scale) * 4ull;
                for (run = 0; run < count * place.Scale; run++) {
                    if (a == 255ul) {
                        row[0] = colour->B; row[1] = colour->G; row[2] = colour->R; row[3] = 0xFF;
                    } else {
                        row[0] = (unsigned char)Blend(colour->B, row[0], a);
                        row[1] = (unsigned char)Blend(colour->G, row[1], a);
                        row[2] = (unsigned char)Blend(colour->R, row[2], a);
                        row[3] = 0xFF;
                    }
                    row += 4;
                }
            }
            written += count * place.Scale * place.Scale;
            x += count;
        }
    }
    if (Written != 0) *Written = written;
    return Bc250LogoDrawn;
}
