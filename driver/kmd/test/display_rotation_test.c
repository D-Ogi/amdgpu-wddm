/* Host test for driver\kmd\display_rotation.h (BD-247, docs/design/display-rotation.md): which path rotations the
 * driver shows, the rotation support it states, and the rotated bugcheck copy. The test includes the production
 * header, so there is nothing to extract. run_display_rotation.ps1 builds it, and with -Mutation all it also
 * builds each negative control, which must fail. */
#include <stdio.h>
#include <string.h>
#include "display_rotation.h"

static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

/* The WDK bit field, with the same member names. */
typedef struct {
    unsigned Identity : 1, Rotate90 : 1, Rotate180 : 1, Rotate270 : 1;
    unsigned Offset0 : 1, Offset90 : 1, Offset180 : 1, Offset270 : 1;
} TEST_ROTATION_SUPPORT;

/* The turn, as the contract states it: content turns counter-clockwise on the panel. At 90 degrees the logical
 * top-left corner shows at the physical bottom-left, the logical top-right at the physical top-left. */
static void Corners(void)
{
    unsigned px, py;
    Bc250RotationMapPixel(90u, 1920u, 1080u, 0u, 0u, &px, &py); CHECK(px == 0u && py == 1079u);
    Bc250RotationMapPixel(90u, 1920u, 1080u, 1079u, 0u, &px, &py); CHECK(px == 0u && py == 0u);
    Bc250RotationMapPixel(90u, 1920u, 1080u, 0u, 1919u, &px, &py); CHECK(px == 1919u && py == 1079u);
    Bc250RotationMapPixel(180u, 1920u, 1080u, 0u, 0u, &px, &py); CHECK(px == 1919u && py == 1079u);
    Bc250RotationMapPixel(270u, 1920u, 1080u, 0u, 0u, &px, &py); CHECK(px == 1919u && py == 0u);
    Bc250RotationMapPixel(270u, 1920u, 1080u, 1079u, 1919u, &px, &py); CHECK(px == 0u && py == 1079u);
    Bc250RotationMapPixel(0u, 1920u, 1080u, 7u, 9u, &px, &py); CHECK(px == 7u && py == 9u);
}

/* Each angle is a bijection of the logical size onto the physical one, and 90 then 270 is the identity. */
static void Bijection(unsigned angle, unsigned w, unsigned h)
{
    static unsigned char seen[7 * 5];
    unsigned lw, lh, x, y, px, py, qx, qy, hits = 0;
    memset(seen, 0, sizeof(seen));
    Bc250RotationLogicalSize(angle, w, h, &lw, &lh);
    CHECK(lw * lh == w * h);
    for (y = 0; y < lh; y++)
        for (x = 0; x < lw; x++) {
            Bc250RotationMapPixel(angle, w, h, x, y, &px, &py);
            CHECK(px < w && py < h);
            if (px < w && py < h && !seen[py * w + px]++) hits++;
            /* The inverse turn of the inverse geometry gives the pixel back. */
            Bc250RotationMapPixel((360u - angle) % 360u, lw, lh, px, py, &qx, &qy);
            CHECK(qx == x && qy == y);
        }
    CHECK(hits == w * h);
}

static void Angles(void)
{
    unsigned v;
    CHECK(Bc250RotationAngle(BC250_VPPR_IDENTITY, 1) == 0u);
    CHECK(Bc250RotationAngle(BC250_VPPR_UNINITIALIZED, 1) == 0u);
    CHECK(Bc250RotationAngle(BC250_VPPR_UNPINNED, 1) == 0u);
    CHECK(Bc250RotationAngle(BC250_VPPR_NOTSPECIFIED, 1) == 0u);
    CHECK(Bc250RotationAngle(BC250_VPPR_ROTATE90, 1) == 90u);
    CHECK(Bc250RotationAngle(BC250_VPPR_ROTATE180, 1) == 180u);
    CHECK(Bc250RotationAngle(BC250_VPPR_ROTATE270, 1) == 270u);
    /* Every offset form is refused: one path, no clone. */
    for (v = BC250_VPPR_IDENTITY_OFFSET90; v <= BC250_VPPR_ROTATE270_OFFSET270; v++)
        CHECK(Bc250RotationAngle(v, 1) == BC250_ROTATION_REFUSED);
    CHECK(Bc250RotationAngle(17u, 1) == BC250_ROTATION_REFUSED);
    /* The display-only table keeps the identity alone. */
    CHECK(Bc250RotationAngle(BC250_VPPR_IDENTITY, 0) == 0u);
    CHECK(Bc250RotationAngle(BC250_VPPR_ROTATE90, 0) == BC250_ROTATION_REFUSED);
    CHECK(Bc250RotationAngle(BC250_VPPR_ROTATE180, 0) == BC250_ROTATION_REFUSED);
    CHECK(Bc250RotationAngle(BC250_VPPR_ROTATE270, 0) == BC250_ROTATION_REFUSED);
    CHECK(Bc250RotationValue(0u) == BC250_VPPR_IDENTITY && Bc250RotationValue(90u) == BC250_VPPR_ROTATE90 &&
          Bc250RotationValue(180u) == BC250_VPPR_ROTATE180 && Bc250RotationValue(270u) == BC250_VPPR_ROTATE270);
    CHECK(Bc250RotationValue(BC250_ROTATION_REFUSED) == BC250_VPPR_IDENTITY);
    for (v = 0; v < 360u; v += 90u) CHECK(Bc250RotationAngle(Bc250RotationValue(v), 1) == v);
}

static void Support(void)
{
    TEST_ROTATION_SUPPORT s;
    memset(&s, 0xFF, sizeof(s));
    BC250_ROTATION_SUPPORT_FILL(s, 1);
    CHECK(s.Identity && s.Rotate90 && s.Rotate180 && s.Rotate270 && s.Offset0);
    /* E16 run 007: an offset other than 0 on the primary path bugchecks a test-signed system. */
    CHECK(!s.Offset90 && !s.Offset180 && !s.Offset270);
    memset(&s, 0xFF, sizeof(s));
    BC250_ROTATION_SUPPORT_FILL(s, 0);
    CHECK(s.Identity && !s.Rotate90 && !s.Rotate180 && !s.Rotate270 && !s.Offset0 && !s.Offset90);
}

static void Sizes(void)
{
    unsigned w, h;
    Bc250RotationLogicalSize(0u, 1920u, 1080u, &w, &h); CHECK(w == 1920u && h == 1080u);
    Bc250RotationLogicalSize(90u, 1920u, 1080u, &w, &h); CHECK(w == 1080u && h == 1920u);
    Bc250RotationLogicalSize(180u, 1920u, 1080u, &w, &h); CHECK(w == 1920u && h == 1080u);
    Bc250RotationLogicalSize(270u, 1920u, 1080u, &w, &h); CHECK(w == 1080u && h == 1920u);
    Bc250RotationLogicalSize(BC250_ROTATION_REFUSED, 1920u, 1080u, &w, &h); CHECK(w == 1920u && h == 1080u);
}

/* The bugcheck copy against the pixel map: every written physical pixel holds the logical pixel that maps to it,
 * nothing outside the clipped block is touched, and the pitch padding stays as it was. */
static void Write(unsigned angle)
{
    enum { W = 7, H = 5, PITCH = 40 };   /* 7 pixels of 4 bytes and 12 bytes of padding a row */
    static unsigned char frame[PITCH * H];
    static unsigned source[6 * 8];
    unsigned lw, lh, x, y, px, py, i, sx = 2, sy = 1, sw = 6, sh = 8, stride = 6 * 4, expected;
    for (i = 0; i < 6 * 8; i++) source[i] = 0x01000000u + i;
    memset(frame, 0xAB, sizeof(frame));
    Bc250RotationLogicalSize(angle, W, H, &lw, &lh);
    Bc250RotationWriteBlock(angle, frame, PITCH, W, H, (const unsigned char*)source, sw, sh, stride, sx, sy);
    for (py = 0; py < H; py++)
        for (px = 0; px < PITCH / 4u; px++) {
            unsigned word;
            memcpy(&word, frame + py * PITCH + 4u * px, 4);
            expected = 0xABABABABu;
            if (px < W)
                for (y = sy; y < lh && y < sy + sh; y++)
                    for (x = sx; x < lw && x < sx + sw; x++) {
                        unsigned qx, qy;
                        Bc250RotationMapPixel(angle, W, H, x, y, &qx, &qy);
                        if (qx == px && qy == py) expected = source[(y - sy) * 6u + (x - sx)];
                    }
            CHECK(word == expected);
        }
    /* A block at or past the logical edge writes nothing. */
    memset(frame, 0xAB, sizeof(frame));
    Bc250RotationWriteBlock(angle, frame, PITCH, W, H, (const unsigned char*)source, sw, sh, stride, lw, 0);
    Bc250RotationWriteBlock(angle, frame, PITCH, W, H, (const unsigned char*)source, sw, sh, stride, 0, lh);
    Bc250RotationWriteBlock(angle, frame, PITCH, W, H, NULL, sw, sh, stride, 0, 0);
    for (i = 0; i < sizeof(frame); i++) if (frame[i] != 0xAB) break;
    CHECK(i == sizeof(frame));
    /* A stride shorter than the width clips the block to the stride. */
    memset(frame, 0xAB, sizeof(frame));
    Bc250RotationWriteBlock(angle, frame, PITCH, W, H, (const unsigned char*)source, sw, 1, 4, 0, 0);
    Bc250RotationMapPixel(angle, W, H, 1, 0, &px, &py);
    CHECK(frame[py * PITCH + 4u * px] == 0xAB);
    Bc250RotationMapPixel(angle, W, H, 0, 0, &px, &py);
    CHECK(frame[py * PITCH + 4u * px] == 0x00);
}

int main(void)
{
    unsigned a;
    Angles();
    Support();
    Sizes();
    Corners();
    for (a = 0; a < 360u; a += 90u) { Bijection(a, 7, 5); Bijection(a, 5, 5); Bijection(a, 1, 3); Write(a); }
    printf("display rotation: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
