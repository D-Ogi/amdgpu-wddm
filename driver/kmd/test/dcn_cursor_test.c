/* Host test for driver\kmd\dcn_cursor_shape.h (docs/design/hardware-pointer.md): the conversion of the three WDDM
 * pointer formats into the DCN cursor surface, the hot spot rule, the placement with its negative clamp and its
 * edges, the destination offset, and the rotation, checked against display_rotation.h's pixel map. The test includes
 * the production headers, so there is nothing to extract. run_dcn_cursor.ps1 builds it, and with -Mutation all it
 * also builds each negative control, which must fail. */
#include <stdio.h>
#include <string.h>
#include "display_rotation.h"
#include "dcn_cursor_shape.h"

static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

#define CANARY 0x5A5A5A5Au
static unsigned int surface[BC250_CURSOR_PITCH_PIXELS * BC250_CURSOR_MAX_SIZE];
static unsigned char input[BC250_CURSOR_RAW_MAX_BYTES + 64];

static void Fill(void) { unsigned i; for (i = 0; i < sizeof(surface) / 4; i++) surface[i] = CANARY; }

static BC250_CURSOR_SHAPE Shape(unsigned flags, unsigned w, unsigned h, unsigned pitch)
{
    BC250_CURSOR_SHAPE s;
    s.Flags = flags; s.Width = w; s.Height = h; s.Pitch = pitch; s.XHot = 0; s.YHot = 0; s.Pixels = input;
    return s;
}

/* Only the image rectangle is written: every word outside it keeps the canary. */
static unsigned Outside(const BC250_CURSOR_IMAGE* image)
{
    unsigned u, v, bad = 0;
    for (v = 0; v < BC250_CURSOR_MAX_SIZE; v++)
        for (u = 0; u < BC250_CURSOR_PITCH_PIXELS; u++)
            if ((u >= image->Width || v >= image->Height) && surface[v * BC250_CURSOR_PITCH_PIXELS + u] != CANARY) bad++;
    return bad;
}

static void Verdicts(void)
{
    BC250_CURSOR_SHAPE s;
    s = Shape(BC250_POINTER_COLOR, 32, 32, 128); CHECK(Bc250CursorShapeCheck(&s) == BC250_CURSOR_SHAPE_OK);
    s.Pixels = NULL; CHECK(Bc250CursorShapeCheck(&s) == BC250_CURSOR_SHAPE_ARGS);
    s = Shape(0, 32, 32, 128); CHECK(Bc250CursorShapeCheck(&s) == BC250_CURSOR_SHAPE_FLAGS);
    s = Shape(BC250_POINTER_COLOR | BC250_POINTER_MONOCHROME, 32, 32, 128);
    CHECK(Bc250CursorShapeCheck(&s) == BC250_CURSOR_SHAPE_FLAGS);
    s = Shape(0x8u, 32, 32, 128); CHECK(Bc250CursorShapeCheck(&s) == BC250_CURSOR_SHAPE_FLAGS);
    s = Shape(BC250_POINTER_COLOR, 0, 32, 128); CHECK(Bc250CursorShapeCheck(&s) == BC250_CURSOR_SHAPE_SIZE);
    s = Shape(BC250_POINTER_COLOR, 32, 0, 128); CHECK(Bc250CursorShapeCheck(&s) == BC250_CURSOR_SHAPE_SIZE);
    s = Shape(BC250_POINTER_COLOR, 257, 1, 1028); CHECK(Bc250CursorShapeCheck(&s) == BC250_CURSOR_SHAPE_SIZE);
    s = Shape(BC250_POINTER_MONOCHROME, 8, 257, 1); CHECK(Bc250CursorShapeCheck(&s) == BC250_CURSOR_SHAPE_SIZE);
    s = Shape(BC250_POINTER_COLOR, 7, 3, 27); CHECK(Bc250CursorShapeCheck(&s) == BC250_CURSOR_SHAPE_PITCH);
    s = Shape(BC250_POINTER_MONOCHROME, 13, 3, 1); CHECK(Bc250CursorShapeCheck(&s) == BC250_CURSOR_SHAPE_PITCH);
    s = Shape(BC250_POINTER_MONOCHROME, 13, 3, 2); CHECK(Bc250CursorShapeCheck(&s) == BC250_CURSOR_SHAPE_OK);
    /* The largest shapes fit the copy; one more byte of pitch does not. */
    s = Shape(BC250_POINTER_COLOR, 256, 256, 1024); CHECK(Bc250CursorShapeCheck(&s) == BC250_CURSOR_SHAPE_OK);
    CHECK(Bc250CursorShapeBytes(&s) == 256u * 1024u);
    s.Pitch = 1028; CHECK(Bc250CursorShapeCheck(&s) == BC250_CURSOR_SHAPE_BYTES);
    s = Shape(BC250_POINTER_MONOCHROME, 256, 256, 32); CHECK(Bc250CursorShapeBytes(&s) == 2u * 256u * 32u);
    s = Shape(BC250_POINTER_MONOCHROME, 256, 256, 512); CHECK(Bc250CursorShapeCheck(&s) == BC250_CURSOR_SHAPE_OK);
    s.Pitch = 513; CHECK(Bc250CursorShapeCheck(&s) == BC250_CURSOR_SHAPE_BYTES);
    /* The hot spot is a pixel of the image: the last one passes, one past it on either axis is refused. For
     * Monochrome the height is that of one mask. */
    s = Shape(BC250_POINTER_COLOR, 32, 16, 128); s.XHot = 31; s.YHot = 15; CHECK(Bc250CursorShapeCheck(&s) == BC250_CURSOR_SHAPE_OK);
    s.XHot = 32; CHECK(Bc250CursorShapeCheck(&s) == BC250_CURSOR_SHAPE_HOTSPOT);
    s.XHot = 0; s.YHot = 16; CHECK(Bc250CursorShapeCheck(&s) == BC250_CURSOR_SHAPE_HOTSPOT);
    s = Shape(BC250_POINTER_MONOCHROME, 32, 32, 4); s.YHot = 32; CHECK(Bc250CursorShapeCheck(&s) == BC250_CURSOR_SHAPE_HOTSPOT);
    s = Shape(BC250_POINTER_COLOR, 32, 32, 128);
    { BC250_CURSOR_IMAGE image; Fill(); CHECK(Bc250CursorRender(&s, 45u, 0, surface, &image) == BC250_CURSOR_SHAPE_ANGLE); }
    CHECK(Bc250CursorRender(&s, 0u, 0, NULL, NULL) == BC250_CURSOR_SHAPE_ARGS);
    CHECK(Bc250CursorAngleValid(0u) && Bc250CursorAngleValid(90u) && Bc250CursorAngleValid(180u) &&
          Bc250CursorAngleValid(270u) && !Bc250CursorAngleValid(360u) && !Bc250CursorAngleValid(1u));
}

/* The 1 bpp masks. Rows Pitch bytes apart, the XOR mask Height rows after the AND mask, the most significant bit
 * the leftmost pixel. Padding bits past the width and padding bytes past the row are set and must not matter. */
static void MonoSet(unsigned pitch, unsigned h, unsigned mask, unsigned i, unsigned j, unsigned bit)
{
    unsigned char* row = input + (mask * h + j) * pitch;
    if (bit) row[i >> 3] |= (unsigned char)(0x80u >> (i & 7u));
    else row[i >> 3] &= (unsigned char)~(0x80u >> (i & 7u));
}

static void Monochrome(void)
{
    const unsigned w = 13, h = 5, pitch = 4;
    BC250_CURSOR_SHAPE s = Shape(BC250_POINTER_MONOCHROME, w, h, pitch);
    BC250_CURSOR_IMAGE image;
    unsigned i, j, andBit, xorBit, word, expect;
    memset(input, 0xFF, 2 * h * pitch);
    /* AND 1 XOR 0 (transparent) by default; a pattern of AND 0 XOR 0 (black) and AND 0 XOR 1 (white). */
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++) {
            MonoSet(pitch, h, 1, i, j, 0);
            if ((i + 2 * j) % 3 == 1) MonoSet(pitch, h, 0, i, j, 0);
            if ((i + 2 * j) % 3 == 2) { MonoSet(pitch, h, 0, i, j, 0); MonoSet(pitch, h, 1, i, j, 1); }
        }
    Fill();
    CHECK(Bc250CursorRender(&s, 0u, 0, surface, &image) == BC250_CURSOR_SHAPE_OK);
    CHECK(image.Width == w && image.Height == h && image.XorPixels == 0 && image.Mode == BC250_CURSOR_MODE_STRAIGHT);
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++) {
            word = surface[j * BC250_CURSOR_PITCH_PIXELS + i];
            expect = (i + 2 * j) % 3 == 0 ? 0u : (i + 2 * j) % 3 == 1 ? 0xFF000000u : 0xFFFFFFFFu;
            CHECK(word == expect);
        }
    CHECK(Outside(&image) == 0);

    /* One inverting pixel (AND 1 XOR 1): the whole image goes out as COLOR_1BIT_AND, AND in the alpha byte. */
    MonoSet(pitch, h, 1, 12, 4, 1);
    MonoSet(pitch, h, 0, 12, 4, 1);
    Fill();
    CHECK(Bc250CursorRender(&s, 0u, 0, surface, &image) == BC250_CURSOR_SHAPE_OK);
    CHECK(image.XorPixels == 1 && image.Mode == BC250_CURSOR_MODE_COLOR_1BIT_AND);
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++) {
            andBit = (input[j * pitch + (i >> 3)] >> (7u - (i & 7u))) & 1u;
            xorBit = (input[(h + j) * pitch + (i >> 3)] >> (7u - (i & 7u))) & 1u;
            CHECK(surface[j * BC250_CURSOR_PITCH_PIXELS + i] == ((andBit ? 0xFF000000u : 0u) | (xorBit ? 0xFFFFFFu : 0u)));
        }
    CHECK(surface[4 * BC250_CURSOR_PITCH_PIXELS + 12] == 0xFFFFFFFFu);
    CHECK(Outside(&image) == 0);
    CHECK(Bc250CursorLinesPerChunk(w, BC250_CURSOR_MODE_MONO) == 4u);
}

static void PutWord(unsigned pitch, unsigned i, unsigned j, unsigned word)
{
    unsigned char* p = input + j * pitch + 4 * i;
    p[0] = (unsigned char)word; p[1] = (unsigned char)(word >> 8); p[2] = (unsigned char)(word >> 16);
    p[3] = (unsigned char)(word >> 24);
}

static void Color(void)
{
    const unsigned w = 7, h = 3, pitch = 32;
    BC250_CURSOR_SHAPE s = Shape(BC250_POINTER_COLOR, w, h, pitch);
    BC250_CURSOR_IMAGE image;
    unsigned i, j, ok = 1;
    memset(input, 0xAB, h * pitch);
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++) PutWord(pitch, i, j, 0x01000000u * (i * 37 + j) + 0x00010203u * (j * 7 + i + 1));
    Fill();
    CHECK(Bc250CursorRender(&s, 0u, 0, surface, &image) == BC250_CURSOR_SHAPE_OK);
    CHECK(image.Width == w && image.Height == h && image.Mode == BC250_CURSOR_MODE_STRAIGHT && image.XorPixels == 0);
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            if (surface[j * BC250_CURSOR_PITCH_PIXELS + i] != 0x01000000u * (i * 37 + j) + 0x00010203u * (j * 7 + i + 1)) ok = 0;
    CHECK(ok);
    CHECK(Outside(&image) == 0);
    Fill();
    CHECK(Bc250CursorRender(&s, 0u, 1, surface, &image) == BC250_CURSOR_SHAPE_OK);
    CHECK(image.Mode == BC250_CURSOR_MODE_PREMULTIPLIED);
    CHECK(Bc250CursorLinesPerChunk(32, BC250_CURSOR_MODE_STRAIGHT) == 4u);
    CHECK(Bc250CursorLinesPerChunk(33, BC250_CURSOR_MODE_STRAIGHT) == 3u);
    CHECK(Bc250CursorLinesPerChunk(64, BC250_CURSOR_MODE_STRAIGHT) == 3u);
    CHECK(Bc250CursorLinesPerChunk(128, BC250_CURSOR_MODE_STRAIGHT) == 2u);
    CHECK(Bc250CursorLinesPerChunk(256, BC250_CURSOR_MODE_STRAIGHT) == 1u);
}

static void Masked(void)
{
    const unsigned w = 5, h = 2, pitch = 24;
    BC250_CURSOR_SHAPE s = Shape(BC250_POINTER_MASKED_COLOR, w, h, pitch);
    BC250_CURSOR_IMAGE image;
    memset(input, 0xCD, h * pitch);
    PutWord(pitch, 0, 0, 0x00123456u);     /* replace: opaque */
    PutWord(pitch, 1, 0, 0xFF000000u);     /* XOR with 0: transparent */
    PutWord(pitch, 2, 0, 0x7F654321u);     /* read by the high bit: replace */
    PutWord(pitch, 3, 0, 0x80000000u);     /* read by the high bit: transparent */
    PutWord(pitch, 4, 0, 0x00000000u);     /* replace with black */
    PutWord(pitch, 0, 1, 0xFF000000u); PutWord(pitch, 1, 1, 0xFF000000u); PutWord(pitch, 2, 1, 0x00FFFFFFu);
    PutWord(pitch, 3, 1, 0x00FF0000u); PutWord(pitch, 4, 1, 0xFF000000u);
    Fill();
    CHECK(Bc250CursorRender(&s, 0u, 0, surface, &image) == BC250_CURSOR_SHAPE_OK);
    CHECK(image.Mode == BC250_CURSOR_MODE_STRAIGHT && image.XorPixels == 0);
    CHECK(surface[0] == 0xFF123456u && surface[1] == 0u && surface[2] == 0xFF654321u && surface[3] == 0u);
    CHECK(surface[4] == 0xFF000000u);
    CHECK(surface[BC250_CURSOR_PITCH_PIXELS + 2] == 0xFFFFFFFFu && surface[BC250_CURSOR_PITCH_PIXELS + 3] == 0xFFFF0000u);
    CHECK(Outside(&image) == 0);
    /* A pixel that XORs a colour into the screen: COLOR_1BIT_AND, the words as AND and XOR. */
    PutWord(pitch, 4, 1, 0xFF00FF00u);
    Fill();
    CHECK(Bc250CursorRender(&s, 0u, 1, surface, &image) == BC250_CURSOR_SHAPE_OK);
    CHECK(image.Mode == BC250_CURSOR_MODE_COLOR_1BIT_AND && image.XorPixels == 1);
    CHECK(surface[0] == 0x00123456u && surface[1] == 0xFF000000u && surface[3] == 0xFF000000u);
    CHECK(surface[BC250_CURSOR_PITCH_PIXELS + 4] == 0xFF00FF00u);
}

static void Hotspot(void)
{
    int x, y;
    Bc250CursorTopLeft(BC250_CURSOR_SETTING_ON, 100, 50, 7, 9, &x, &y); CHECK(x == 100 && y == 50);
    Bc250CursorTopLeft(BC250_CURSOR_SETTING_ON | BC250_CURSOR_SETTING_HOTSPOT_POSITION, 100, 50, 7, 9, &x, &y);
    CHECK(x == 93 && y == 41);
    Bc250CursorTopLeft(BC250_CURSOR_SETTING_ON | BC250_CURSOR_SETTING_HOTSPOT_POSITION, 3, 0, 7, 9, &x, &y);
    CHECK(x == -4 && y == -9);
}

static BC250_CURSOR_PLANE Plane(unsigned pw, unsigned ph)
{
    BC250_CURSOR_PLANE p;
    memset(&p, 0, sizeof(p));
    p.SourceWidth = p.ViewportWidth = pw; p.SourceHeight = p.ViewportHeight = ph;
    p.RefClockKhz = 100000u; p.PixelClockKhz = 148500u;
    return p;
}

static void Place(void)
{
    BC250_CURSOR_PLANE p = Plane(1920, 1080);
    BC250_CURSOR_IMAGE image = { 32, 32, BC250_CURSOR_MODE_STRAIGHT, 0 };
    BC250_CURSOR_PLACE out;
    Bc250CursorPlace(&p, 0u, 32, 32, &image, 1, 100, 50, &out);
    CHECK(out.Enable == 1 && out.X == 100 && out.Y == 50 && out.HotX == 0 && out.HotY == 0);
    CHECK(out.DstX == 100u * 100000u / 148500u);
    /* Negative: position 0 and the part outside in the hot spot (amdgpu_dm's rule). */
    Bc250CursorPlace(&p, 0u, 32, 32, &image, 1, -10, -3, &out);
    CHECK(out.Enable == 1 && out.X == 0 && out.Y == 0 && out.HotX == 10 && out.HotY == 3 && out.DstX == 0);
    Bc250CursorPlace(&p, 0u, 32, 32, &image, 1, -31, 0, &out); CHECK(out.Enable == 1 && out.HotX == 31);
    Bc250CursorPlace(&p, 0u, 32, 32, &image, 1, -32, 0, &out); CHECK(out.Enable == 0);
    Bc250CursorPlace(&p, 0u, 32, 32, &image, 1, 0, -32, &out); CHECK(out.Enable == 0);
    Bc250CursorPlace(&p, 0u, 32, 32, &image, 1, 0, -31, &out); CHECK(out.Enable == 1 && out.HotY == 31);
    Bc250CursorPlace(&p, 0u, 32, 32, &image, 1, 1919, 1079, &out); CHECK(out.Enable == 1 && out.X == 1919);
    Bc250CursorPlace(&p, 0u, 32, 32, &image, 1, 1920, 0, &out); CHECK(out.Enable == 0);
    Bc250CursorPlace(&p, 0u, 32, 32, &image, 1, 0, 1080, &out); CHECK(out.Enable == 0);
    Bc250CursorPlace(&p, 0u, 32, 32, &image, 0, 100, 50, &out); CHECK(out.Enable == 0 && out.X == 0);
    /* The largest image: a hot spot of 255 still fits the 8-bit field. */
    image.Width = image.Height = 256;
    Bc250CursorPlace(&p, 0u, 256, 256, &image, 1, -255, -255, &out);
    CHECK(out.Enable == 1 && out.HotX == 255 && out.HotY == 255);
    Bc250CursorPlace(&p, 0u, 256, 256, &image, 1, -256, 0, &out); CHECK(out.Enable == 0);
    /* The destination offset: 13 bits at most, and scaled by recout over viewport. */
    p.PixelClockKhz = 1000u;
    Bc250CursorPlace(&p, 0u, 256, 256, &image, 1, 1900, 0, &out); CHECK(out.Enable == 1 && out.DstX == 0x1FFFu);
    p.PixelClockKhz = 100000u; p.DstNumerator = 2; p.DstDenominator = 1;
    Bc250CursorPlace(&p, 0u, 256, 256, &image, 1, 300, 0, &out); CHECK(out.DstX == 600u);
    p.RefClockKhz = 0;
    Bc250CursorPlace(&p, 0u, 256, 256, &image, 1, 300, 0, &out); CHECK(out.Enable == 1 && out.DstX == 0u);
}

/* A multiplane overlay plane: viewport (200, 100, 960 x 540) of the surface shown at recout (0, 0, 1920 x 1080). The
 * register position is in the surface, the edges and the destination offset are those of the viewport. */
static void Scaled(void)
{
    BC250_CURSOR_PLANE p = Plane(1920, 1080);
    BC250_CURSOR_IMAGE image = { 32, 32, BC250_CURSOR_MODE_STRAIGHT, 0 };
    BC250_CURSOR_PLACE out;
    p.Map = 1; p.ViewportX = 200; p.ViewportY = 100; p.ViewportWidth = 960; p.ViewportHeight = 540;
    p.RecoutWidth = 1920; p.RecoutHeight = 1080; p.DstNumerator = 1920; p.DstDenominator = 960;
    p.RefClockKhz = p.PixelClockKhz = 100000u;
    Bc250CursorPlace(&p, 0u, 32, 32, &image, 1, 1000, 500, &out);
    CHECK(out.Enable == 1 && out.X == 700 && out.Y == 350 && out.HotX == 0 && out.DstX == 1000u);
    /* Left of the viewport but inside the surface: no clamp, no destination offset. */
    Bc250CursorPlace(&p, 0u, 32, 32, &image, 1, -20, 0, &out);
    CHECK(out.Enable == 1 && out.X == 190 && out.HotX == 0 && out.DstX == 0);
    Bc250CursorPlace(&p, 0u, 32, 32, &image, 1, -64, 0, &out); CHECK(out.Enable == 0);
    Bc250CursorPlace(&p, 0u, 32, 32, &image, 1, 1919, 1079, &out); CHECK(out.Enable == 1 && out.X == 1159);
    p.ViewportX = 0; p.ViewportY = 0;
    Bc250CursorPlace(&p, 0u, 32, 32, &image, 1, -20, -2, &out);
    CHECK(out.Enable == 1 && out.X == 0 && out.HotX == 10 && out.Y == 0 && out.HotY == 1);
}

/* The rotated pointer against display_rotation.h: each logical pixel of a 5 x 3 image at (X, Y) of the desktop must
 * show, on the physical frame, where Bc250RotationMapPixel puts the desktop pixel under it. */
static void Rotation(unsigned angle)
{
    const unsigned w = 5, h = 3, pitch = 20, pw = 1920, ph = 1080;
    const int x = 300, y = 200;
    BC250_CURSOR_SHAPE s = Shape(BC250_POINTER_COLOR, w, h, pitch);
    BC250_CURSOR_PLANE p = Plane(pw, ph);
    BC250_CURSOR_IMAGE image;
    BC250_CURSOR_PLACE out;
    unsigned i, j, px, py, u, v, ok = 1;
    for (j = 0; j < h; j++) for (i = 0; i < w; i++) PutWord(pitch, i, j, 0xFF000000u | (j << 8) | i);
    Fill();
    CHECK(Bc250CursorRender(&s, angle, 0, surface, &image) == BC250_CURSOR_SHAPE_OK);
    CHECK(image.Width == ((angle == 90u || angle == 270u) ? h : w));
    CHECK(Outside(&image) == 0);
    Bc250CursorPlace(&p, angle, w, h, &image, 1, x, y, &out);
    CHECK(out.Enable == 1 && out.HotX == 0 && out.HotY == 0);
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++) {
            Bc250RotationMapPixel(angle, pw, ph, (unsigned)x + i, (unsigned)y + j, &px, &py);
            u = px - out.X; v = py - out.Y;
            if (u >= image.Width || v >= image.Height ||
                surface[v * BC250_CURSOR_PITCH_PIXELS + u] != (0xFF000000u | (j << 8) | i)) ok = 0;
        }
    CHECK(ok);
}

/* At an edge of a rotated desktop the clamp works in the physical frame: the logical left edge at 90 degrees is the
 * physical bottom, so a pointer left of the desktop loses its physical bottom rows, never gets a negative position. */
static void RotatedEdges(void)
{
    BC250_CURSOR_PLANE p = Plane(1920, 1080);
    BC250_CURSOR_IMAGE image = { 32, 32, BC250_CURSOR_MODE_STRAIGHT, 0 };
    BC250_CURSOR_PLACE out;
    /* 90: the logical top (y = -5) is the physical left. */
    Bc250CursorPlace(&p, 90u, 32, 32, &image, 1, 0, -5, &out);
    CHECK(out.Enable == 1 && out.X == 0 && out.HotX == 5 && out.Y == 1080 - 32 && out.HotY == 0);
    /* 270: the logical top is the physical right; the logical left is the physical top. */
    Bc250CursorPlace(&p, 270u, 32, 32, &image, 1, -6, 0, &out);
    CHECK(out.Enable == 1 && out.Y == 0 && out.HotY == 6 && out.X == 1920 - 32);
    /* 180: the logical top left corner is the physical bottom right. */
    Bc250CursorPlace(&p, 180u, 32, 32, &image, 1, -3, -4, &out);
    CHECK(out.Enable == 1 && out.X == 1920 - 29 && out.Y == 1080 - 28);
    Bc250CursorPlace(&p, 180u, 32, 32, &image, 1, 1919, 1079, &out);
    CHECK(out.Enable == 1 && out.X == 0 && out.Y == 0 && out.HotX == 31 && out.HotY == 31);
}

int main(void)
{
    Verdicts();
    Monochrome();
    Color();
    Masked();
    Hotspot();
    Place();
    Scaled();
    Rotation(0u); Rotation(90u); Rotation(180u); Rotation(270u);
    RotatedEdges();
    printf("dcn cursor: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
