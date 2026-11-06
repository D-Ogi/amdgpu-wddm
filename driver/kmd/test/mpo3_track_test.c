/* Host test for driver\kmd\mpo3_track.h: the PresentId of plane 0 across MPO3 flips, kept requests, legacy flips and
 * VSync reports, and the plane rule of Check and the flip. The header is included as it ships; run_mpo3_track.ps1
 * also compiles this test against copies of it with one rule broken, and requires a FAIL line from each. */
#include <stdio.h>
#include <string.h>
#include "mpo3_track.h"

static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

#define A1 0x1000000ll
#define A2 0x2000000ll

/* One report: its kind, its count and its id, against what the case expects. */
static void Expect(BC250_MPO3_TRACK* t, long long shown, unsigned kind, unsigned count, unsigned long long id)
{
    unsigned c = 99u;
    unsigned long long p = 99u;
    unsigned k = Bc250Mpo3Report(t, shown, &c, &p);
    CHECK(k == kind);
    CHECK(c == count);
    CHECK(p == id);
}

static void Track(void)
{
    BC250_MPO3_TRACK t;

    /* A fresh tracker is the address contract: CRTC_VSYNC. */
    memset(&t, 0, sizeof(t));
    Expect(&t, A1, 0u, 0u, 0u);

    /* PlaneCount 0 (no plane at all): MPO3 reports from now on, with no plane to name. */
    memset(&t, 0, sizeof(t));
    Bc250Mpo3Enter(&t);
    Expect(&t, A1, 1u, 0u, 0u);

    /* The disable-planes flip of CddDisable with nothing pending: complete at once, named by the next report. */
    memset(&t, 0, sizeof(t));
    Bc250Mpo3Kept(&t, 7u);
    Expect(&t, A1, 1u, 1u, 7u);
    Expect(&t, A2, 1u, 1u, 7u);          /* the id stays the plane's last one, whatever the address */

    /* An immediate flip is current on return: the next report names it even before its address is read back. */
    memset(&t, 0, sizeof(t));
    Bc250Mpo3Flipped(&t, A1, 10u, 1u);
    Expect(&t, A2, 1u, 1u, 10u);

    /* A next-VSync flip retires only on the report that shows its surface. */
    memset(&t, 0, sizeof(t));
    Bc250Mpo3Flipped(&t, A1, 10u, 1u);
    Bc250Mpo3Flipped(&t, A2, 11u, 0u);
    Expect(&t, A1, 1u, 1u, 10u);         /* the plane still shows A1: 11 not yet */
    Expect(&t, A2, 1u, 1u, 11u);
    Expect(&t, A1, 1u, 1u, 11u);         /* retired once; a later report does not take it back */

    /* A next-VSync flip with nothing current yet: a report with no plane until the surface is shown. */
    memset(&t, 0, sizeof(t));
    Bc250Mpo3Flipped(&t, A2, 3u, 0u);
    Expect(&t, A1, 1u, 0u, 0u);
    Expect(&t, A2, 1u, 1u, 3u);

    /* An immediate flip supersedes a pending one. */
    memset(&t, 0, sizeof(t));
    Bc250Mpo3Flipped(&t, A2, 4u, 0u);
    Bc250Mpo3Flipped(&t, A1, 5u, 1u);
    Expect(&t, A2, 1u, 1u, 5u);
    Expect(&t, A1, 1u, 1u, 5u);

    /* A kept request while a flip is pending waits for that flip's surface, and the later id is the one named. */
    memset(&t, 0, sizeof(t));
    Bc250Mpo3Flipped(&t, A1, 20u, 1u);
    Bc250Mpo3Flipped(&t, A2, 21u, 0u);
    Bc250Mpo3Kept(&t, 22u);
    Expect(&t, A1, 1u, 1u, 20u);         /* the plane still reads A1: neither 21 nor 22 */
    Expect(&t, A2, 1u, 1u, 22u);

    /* A kept request with nothing pending supersedes the current id. */
    memset(&t, 0, sizeof(t));
    Bc250Mpo3Flipped(&t, A1, 30u, 1u);
    Bc250Mpo3Kept(&t, 31u);
    Expect(&t, A1, 1u, 1u, 31u);

    /* A legacy flip ends the mode, with everything it held. */
    memset(&t, 0, sizeof(t));
    Bc250Mpo3Flipped(&t, A1, 40u, 1u);
    Bc250Mpo3Flipped(&t, A2, 41u, 0u);
    Bc250Mpo3Legacy(&t);
    Expect(&t, A2, 0u, 0u, 0u);
    /* and a later MPO3 request starts clean: no stale pending or current id. */
    Bc250Mpo3Enter(&t);
    Expect(&t, A2, 1u, 0u, 0u);
}

static BC250_MPO3_PLANE Plane(void)
{
    BC250_MPO3_PLANE p;
    memset(&p, 0, sizeof(p));
    p.PlaneCount = 1u;
    p.ModeWidth = 1920u;
    p.ModeHeight = 1080u;
    p.Src.Right = p.Dst.Right = p.Clip.Right = p.PostSrc.Right = p.PostDst.Right = 1920;
    p.Src.Bottom = p.Dst.Bottom = p.Clip.Bottom = p.PostSrc.Bottom = p.PostDst.Bottom = 1080;
    p.Rotation = p.PostRotation = BC250_MPO3_ROTATION_IDENTITY;
    p.ColorSpace = BC250_MPO3_COLOR_SRGB;
    return p;
}

static void Verdicts(void)
{
    BC250_MPO3_PLANE p;
    unsigned v;

    p = Plane(); CHECK(Bc250Mpo3PlaneVerdict(&p) == BC250_MPO3_OK);
    p = Plane(); p.PostPresent = 1u; CHECK(Bc250Mpo3PlaneVerdict(&p) == BC250_MPO3_OK);
    p = Plane(); p.PlaneCount = 0u; CHECK(Bc250Mpo3PlaneVerdict(&p) == BC250_MPO3_COUNT);
    p = Plane(); p.PlaneCount = 2u; CHECK(Bc250Mpo3PlaneVerdict(&p) == BC250_MPO3_COUNT);
    p = Plane(); p.SourceId = 1u; CHECK(Bc250Mpo3PlaneVerdict(&p) == BC250_MPO3_SOURCE);
    p = Plane(); p.LayerIndex = 1u; CHECK(Bc250Mpo3PlaneVerdict(&p) == BC250_MPO3_LAYER);
    /* Each rectangle, each edge: a scaled, placed or clipped plane is not this rule's. */
    p = Plane(); p.Src.Right = 1280; CHECK(Bc250Mpo3PlaneVerdict(&p) == BC250_MPO3_GEOMETRY);
    p = Plane(); p.Src.Left = 1; CHECK(Bc250Mpo3PlaneVerdict(&p) == BC250_MPO3_GEOMETRY);
    p = Plane(); p.Dst.Top = 8; CHECK(Bc250Mpo3PlaneVerdict(&p) == BC250_MPO3_GEOMETRY);
    p = Plane(); p.Dst.Bottom = 720; CHECK(Bc250Mpo3PlaneVerdict(&p) == BC250_MPO3_GEOMETRY);
    p = Plane(); p.Clip.Right = 1919; CHECK(Bc250Mpo3PlaneVerdict(&p) == BC250_MPO3_GEOMETRY);
    p = Plane(); p.Clip.Top = 1; CHECK(Bc250Mpo3PlaneVerdict(&p) == BC250_MPO3_GEOMETRY);
    p = Plane(); p.Rotation = 2u; CHECK(Bc250Mpo3PlaneVerdict(&p) == BC250_MPO3_ROTATION);
    p = Plane(); p.Rotation = 0u; CHECK(Bc250Mpo3PlaneVerdict(&p) == BC250_MPO3_ROTATION);
    p = Plane(); p.Flags = 1u; CHECK(Bc250Mpo3PlaneVerdict(&p) == BC250_MPO3_FLAGS);
    p = Plane(); p.Flags = 4u; CHECK(Bc250Mpo3PlaneVerdict(&p) == BC250_MPO3_FLAGS);
    p = Plane(); p.AlphaBlend = 1u; CHECK(Bc250Mpo3PlaneVerdict(&p) == BC250_MPO3_BLEND);
    p = Plane(); p.ColorSpace = 12u; CHECK(Bc250Mpo3PlaneVerdict(&p) == BC250_MPO3_COLOR);
    p = Plane(); p.PostPresent = 1u; p.PostFlags = 2u; CHECK(Bc250Mpo3PlaneVerdict(&p) == BC250_MPO3_POST);
    p = Plane(); p.PostPresent = 1u; p.PostRotation = 3u; CHECK(Bc250Mpo3PlaneVerdict(&p) == BC250_MPO3_POST);
    p = Plane(); p.PostPresent = 1u; p.PostDst.Right = 1280; CHECK(Bc250Mpo3PlaneVerdict(&p) == BC250_MPO3_POST);
    p = Plane(); p.PostPresent = 1u; p.PostSrc.Top = 4; CHECK(Bc250Mpo3PlaneVerdict(&p) == BC250_MPO3_POST);
    /* Without a post-composition its fields are not read. */
    p = Plane(); p.PostFlags = 2u; p.PostRotation = 0u; CHECK(Bc250Mpo3PlaneVerdict(&p) == BC250_MPO3_OK);
    /* BD-223: the scaled rule leaves the rectangles to dcn_plane.c and keeps every other test. */
    p = Plane(); p.Src.Right = 1280; p.Src.Bottom = 720; p.Clip.Right = 1919;
    CHECK(Bc250Mpo3PlaneRule(&p, 1u) == BC250_MPO3_OK && Bc250Mpo3PlaneRule(&p, 0u) == BC250_MPO3_GEOMETRY);
    p.Dst.Left = 100; p.Dst.Top = 50; CHECK(Bc250Mpo3PlaneRule(&p, 1u) == BC250_MPO3_OK);
    p.PlaneCount = 2u; CHECK(Bc250Mpo3PlaneRule(&p, 1u) == BC250_MPO3_COUNT); p.PlaneCount = 1u;
    p.SourceId = 1u; CHECK(Bc250Mpo3PlaneRule(&p, 1u) == BC250_MPO3_SOURCE); p.SourceId = 0u;
    p.LayerIndex = 1u; CHECK(Bc250Mpo3PlaneRule(&p, 1u) == BC250_MPO3_LAYER); p.LayerIndex = 0u;
    p.Rotation = 2u; CHECK(Bc250Mpo3PlaneRule(&p, 1u) == BC250_MPO3_ROTATION); p.Rotation = BC250_MPO3_ROTATION_IDENTITY;
    p.Flags = 2u; CHECK(Bc250Mpo3PlaneRule(&p, 1u) == BC250_MPO3_FLAGS); p.Flags = 0u;
    p.AlphaBlend = 1u; CHECK(Bc250Mpo3PlaneRule(&p, 1u) == BC250_MPO3_BLEND); p.AlphaBlend = 0u;
    p.ColorSpace = 12u; CHECK(Bc250Mpo3PlaneRule(&p, 1u) == BC250_MPO3_COLOR); p.ColorSpace = BC250_MPO3_COLOR_SRGB;
    /* A post-composition is the mode's own in both rules: the plane scales, the post-composition does not. */
    p.PostPresent = 1u; p.PostDst.Right = 1280; CHECK(Bc250Mpo3PlaneRule(&p, 1u) == BC250_MPO3_POST);
    p.PostDst.Right = 1920; CHECK(Bc250Mpo3PlaneRule(&p, 1u) == BC250_MPO3_OK);
    /* The full-size rule is the rule with Scaled 0. */
    p = Plane(); CHECK(Bc250Mpo3PlaneRule(&p, 0u) == Bc250Mpo3PlaneVerdict(&p) && Bc250Mpo3PlaneRule(&p, 1u) == BC250_MPO3_OK);
    CHECK(strcmp(Bc250Mpo3VerdictText(BC250_MPO3_SCALED_RULE), "scaled-rule") == 0);
    /* Every verdict has a name, and an unknown one is "?". */
    for (v = 0; v < BC250_MPO3_VERDICTS; v++) CHECK(Bc250Mpo3VerdictText(v)[0] != '?');
    CHECK(Bc250Mpo3VerdictText(BC250_MPO3_VERDICTS)[0] == '?');
}

int main(void)
{
    Track();
    Verdicts();
    if (failures) { printf("mpo3 track: %u of %u checks failed\n", failures, checks); return 1; }
    printf("mpo3 track: %u checks passed\n", checks);
    return 0;
}
