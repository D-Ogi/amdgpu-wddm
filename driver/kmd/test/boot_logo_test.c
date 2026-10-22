/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* Host test of the boot-screen mascot (driver/kmd/boot_logo.c). An independent pixel oracle redraws every case
 * from the asset by itself, so the decoder, the placement and the blend are each checked against something that
 * is not the code under test. It also checks that nothing outside the placed rectangle is touched, that a
 * refused call writes no byte at all, and that the shipped asset verifies and fits every mode the lab uses. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../boot_logo.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c); exit(1); } } while(0)

/* A small asset built by hand, so the test does not depend on what the generator produced today. */
static const BC250_LOGO_COLOR TinyPalette[4] = {
    {0,0,0,0},          /* transparent */
    {10,20,30,255},     /* opaque */
    {200,210,220,128},  /* half */
    {1,2,3,64}          /* quarter */
};
/* 4 x 3: row 0 "1 transparent, 2 opaque, 1 half", row 1 "4 quarter", row 2 "2 opaque, 2 transparent". */
static const unsigned char TinyData[] = {
    1,0, 2,1, 1,2, 0,
    4,3, 0,
    2,1, 2,0, 0
};
static const BC250_BOOT_LOGO Tiny = { 4, 3, 4, sizeof(TinyData), TinyPalette, TinyData };

static unsigned long Blend(unsigned long s, unsigned long d, unsigned long a)
{
    return (s * a + d * (255ul - a) + 127ul) / 255ul;
}

/* The oracle: decode the asset into (x,y,colour) by itself and paint it with the placement the test computed. */
static void Paint(unsigned char* dst, const BC250_BOOT_LOGO* logo, const BC250_BOOT_LOGO_TARGET* t,
                  const BC250_BOOT_LOGO_PLACEMENT* p, unsigned long* written)
{
    unsigned long at = 0, y, sx, sy, i;
    *written = 0;
    for (y = 0; y < logo->Height; y++) {
        unsigned long x = 0;
        for (;;) {
            unsigned long count = logo->Data[at++], index, a;
            const BC250_LOGO_COLOR* c;
            if (count == 0) break;
            index = logo->Data[at++];
            c = &logo->Palette[index];
            a = c->A;
            for (i = 0; i < count; i++, x++) {
                if (a == 0) continue;
                for (sy = 0; sy < p->Scale; sy++) for (sx = 0; sx < p->Scale; sx++) {
                    unsigned char* q = dst + (unsigned long long)(p->Top + y * p->Scale + sy) * t->Pitch +
                                             (unsigned long long)(p->Left + x * p->Scale + sx) * 4ull;
                    q[0] = (unsigned char)(a == 255 ? c->B : Blend(c->B, q[0], a));
                    q[1] = (unsigned char)(a == 255 ? c->G : Blend(c->G, q[1], a));
                    q[2] = (unsigned char)(a == 255 ? c->R : Blend(c->R, q[2], a));
                    q[3] = 0xFF;
                    (*written)++;
                }
            }
        }
    }
}

/* One case: draw with the module, paint with the oracle, compare the whole surface. */
static void Case(const BC250_BOOT_LOGO* logo, unsigned long w, unsigned long h, unsigned long pitch,
                 unsigned long anchor, unsigned char fill, unsigned long* cases)
{
    BC250_BOOT_LOGO_TARGET target;
    BC250_BOOT_LOGO_PLACEMENT place;
    unsigned long bytes = pitch * h, drawn = 0, expected = 0;
    unsigned char *actual = malloc(bytes), *oracle = malloc(bytes);
    CHECK(actual && oracle);
    memset(actual, fill, bytes); memset(oracle, fill, bytes);
    target.Width = w; target.Height = h; target.Pitch = pitch; target.AnchorPermille = anchor;

    CHECK(Bc250BootLogoPlace(logo, &target, &place) == Bc250LogoDrawn);
    CHECK(place.Scale >= 1 && place.Scale <= 3);
    CHECK(place.DrawWidth == logo->Width * place.Scale && place.DrawHeight == logo->Height * place.Scale);
    CHECK(place.Left + place.DrawWidth <= w && place.Top + place.DrawHeight <= h);
    CHECK(place.Left == (w - place.DrawWidth) / 2);         /* centred across */
    /* The ring Windows draws lives below the band, and the picture must never reach it.
     * This holds for every anchor the caller may ask for, including the ones that ask for the bottom. */
    CHECK(place.Top + place.DrawHeight <= h * BC250_LOGO_BOTTOM_PERMILLE / 1000);

    CHECK(Bc250BootLogoDraw(actual, logo, &target, &drawn) == Bc250LogoDrawn);
    Paint(oracle, logo, &target, &place, &expected);
    CHECK(drawn == expected);
    CHECK(memcmp(actual, oracle, bytes) == 0);
    free(actual); free(oracle);
    (*cases)++;
}

int main(void)
{
    const BC250_BOOT_LOGO* nagi = Bc250BootLogoAsset();
    BC250_BOOT_LOGO_TARGET target = { 1920, 1200, 7680, 350 };
    BC250_BOOT_LOGO_PLACEMENT place;
    BC250_BOOT_LOGO broken;
    unsigned char small[64 * 4 * 48], before[sizeof(small)];
    unsigned long cases = 0, drawn, anchor, i;
    unsigned long heights[] = { 480, 600, 720, 768, 1080, 1200, 1440, 2160 };

    /* ---- the hand-built asset, against the oracle, over several surfaces and fills ---- */
    Case(&Tiny, 64, 48, 256, 350, 0x00, &cases);
    Case(&Tiny, 64, 48, 300, 350, 0x7f, &cases);    /* a pitch wider than the width: padding must stay */
    Case(&Tiny, 63, 49, 256, 100, 0xff, &cases);    /* odd sizes, the lowest anchor */
    Case(&Tiny, 63, 49, 256, 900, 0xff, &cases);    /* and the highest */
    for (anchor = 0; anchor <= 1000; anchor += 25) Case(&Tiny, 100, 90, 420, anchor, 0x40, &cases);

    /* ---- the shipped asset ---- */
    CHECK(Bc250BootLogoVerify(nagi) == Bc250LogoDrawn);
    CHECK(nagi->Width > 0 && nagi->Height > 0 && nagi->PaletteCount <= 256);
    /* It must stay small in the driver image: the generator's budget, checked again here. */
    CHECK(nagi->DataBytes + nagi->PaletteCount * sizeof(BC250_LOGO_COLOR) <= 48u * 1024u);
    CHECK(nagi->Palette[0].A == 0);                 /* index 0 is the transparent colour */
    for (i = 0; i < sizeof(heights) / sizeof(heights[0]); i++) {
        unsigned long h = heights[i], w = h * 16 / 9;
        Case(nagi, w, h, w * 4, 350, 0x00, &cases);
        Case(nagi, w, h, w * 4 + 64, 350, 0x00, &cases);
    }
    /* The firmware surfaces the lab actually hands over, including the BD-155 scaled one (800x600 on a
     * 1920x1200 panel: the picture is placed in the 800x600 surface and the display pipe scales it up). */
    Case(nagi, 1920, 1200, 7680, 350, 0x00, &cases);
    Case(nagi, 800, 600, 3200, 350, 0x00, &cases);
    Case(nagi, 1024, 768, 4096, 350, 0x00, &cases);

    /* ---- the enlargement rule ---- */
    target.Width = 1920; target.Height = 1200; target.Pitch = 7680; target.AnchorPermille = 350;
    CHECK(Bc250BootLogoPlace(nagi, &target, &place) == Bc250LogoDrawn && place.Scale == 2);
    target.Width = 800; target.Height = 600; target.Pitch = 3200;
    CHECK(Bc250BootLogoPlace(nagi, &target, &place) == Bc250LogoDrawn && place.Scale == 1);
    target.Width = 3840; target.Height = 2160; target.Pitch = 15360;
    CHECK(Bc250BootLogoPlace(nagi, &target, &place) == Bc250LogoDrawn && place.Scale == 3);

    /* ---- refusals write nothing ---- */
    memset(small, 0xA5, sizeof(small)); memcpy(before, small, sizeof(small));
    target.Width = 16; target.Height = 16; target.Pitch = 64; target.AnchorPermille = 350;
    CHECK(Bc250BootLogoDraw(small, nagi, &target, &drawn) == Bc250LogoNoRoom && drawn == 0);
    target.Width = 64; target.Height = 48; target.Pitch = 100;       /* pitch under width * 4 */
    CHECK(Bc250BootLogoDraw(small, &Tiny, &target, &drawn) == Bc250LogoBadArgument && drawn == 0);
    target.Pitch = 256;
    CHECK(Bc250BootLogoDraw(NULL, &Tiny, &target, &drawn) == Bc250LogoBadArgument);
    CHECK(Bc250BootLogoDraw(small, NULL, &target, &drawn) == Bc250LogoBadArgument && drawn == 0);
    CHECK(Bc250BootLogoDraw(small, &Tiny, NULL, &drawn) == Bc250LogoBadArgument && drawn == 0);
    CHECK(Bc250BootLogoPlace(&Tiny, &target, NULL) == Bc250LogoBadArgument);
    CHECK(memcmp(small, before, sizeof(small)) == 0);

    /* ---- a damaged asset is refused, and is refused before a single pixel ---- */
    broken = Tiny; broken.PaletteCount = 2;                  /* index 2 and 3 now out of range */
    CHECK(Bc250BootLogoVerify(&broken) == Bc250LogoBadAsset);
    CHECK(Bc250BootLogoDraw(small, &broken, &target, &drawn) == Bc250LogoBadAsset && drawn == 0);
    broken = Tiny; broken.Width = 5;                         /* rows no longer add up */
    CHECK(Bc250BootLogoVerify(&broken) == Bc250LogoBadAsset);
    broken = Tiny; broken.Height = 2;                        /* data left over */
    CHECK(Bc250BootLogoVerify(&broken) == Bc250LogoBadAsset);
    broken = Tiny; broken.DataBytes = sizeof(TinyData) - 1;  /* data cut short */
    CHECK(Bc250BootLogoVerify(&broken) == Bc250LogoBadAsset);
    broken = Tiny; broken.Width = 0;
    CHECK(Bc250BootLogoVerify(&broken) == Bc250LogoBadArgument);
    broken = Tiny; broken.PaletteCount = 0;
    CHECK(Bc250BootLogoVerify(&broken) == Bc250LogoBadArgument);
    broken = Tiny; broken.Data = NULL;
    CHECK(Bc250BootLogoVerify(&broken) == Bc250LogoBadArgument);
    CHECK(Bc250BootLogoVerify(NULL) == Bc250LogoBadArgument);
    CHECK(memcmp(small, before, sizeof(small)) == 0);

    /* ---- the result texts exist ---- */
    for (i = 0; i <= 4; i++) CHECK(Bc250BootLogoResultText((BC250_BOOT_LOGO_RESULT)i)[0] != '\0');

    /* What one full redraw of the picture costs, for the record: the pixels it writes at the lab's own mode and
     * how many of them need a read of the framebuffer (the anti-aliased edge). */
    {
        unsigned long at2 = 0, y2, opaque = 0, edge = 0;
        for (y2 = 0; y2 < nagi->Height; y2++)
            for (;;) {
                unsigned long c2 = nagi->Data[at2++], i2, a2;
                if (c2 == 0) break;
                i2 = nagi->Data[at2++];
                a2 = nagi->Palette[i2].A;
                if (a2 == 255) opaque += c2; else if (a2 != 0) edge += c2;
            }
        target.Width = 1920; target.Height = 1200; target.Pitch = 7680; target.AnchorPermille = 350;
        CHECK(Bc250BootLogoPlace(nagi, &target, &place) == Bc250LogoDrawn);
        printf("one redraw at 1920x1200: %lu pixels written (%lu opaque, %lu blended), %lu bytes, at %lux%lu+%lu+%lu\n",
               (opaque + edge) * place.Scale * place.Scale, opaque * place.Scale * place.Scale,
               edge * place.Scale * place.Scale, (opaque + edge) * place.Scale * place.Scale * 4u,
               place.DrawWidth, place.DrawHeight, place.Left, place.Top);
    }

    printf("PASS %lu pixel-oracle cases, the %lux%lu shipped asset (%lu bytes), placement, refusals\n",
           cases, nagi->Width, nagi->Height, nagi->DataBytes + nagi->PaletteCount * 4u);
    return 0;
}
