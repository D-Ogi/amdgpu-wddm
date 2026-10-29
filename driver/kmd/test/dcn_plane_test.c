// Host test of dcn_plane.c: the admission of multiplane-overlay plane 0 (BD-223), and its DML step with the real
// DML201 plan (bc250_dml201.c) of the lab's timing. Built with no WDK header by run_modeset.ps1.
//
// Positive controls: the lab case of BD-223 (a 1920x1080 chain on the Lenovo's 1920x1200 CVT-RB mode, the
// Cyberpunk 2077 session), the identity plane (the committed 1:1 pipe, which needs no DML), a letterboxed 720p
// plane, the 16x upscale limit exactly, and RGB10A2 under a 10-bit source mode. Negative controls: one request for
// every verdict, a DML failure, and a clock budget the scaled pipe does not fit in.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../dcn_plane.h"
#include "../plane_format.h"
#include "../../shim/bc250_dml201.h"

static int g_failures, g_checks;

#define CHECK(cond) \
    do { g_checks++; if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)

static void Rect(BC250_PLANE_RECT* R, long Left, long Top, long Right, long Bottom)
{
    R->Left = Left; R->Top = Top; R->Right = Right; R->Bottom = Bottom;
}

// The lab case: a 1920x1080 flip-model chain, stretched by DWM today onto the 1920x1200 desktop mode.
static BC250_PLANE_REQUEST LabRequest(void)
{
    BC250_PLANE_REQUEST r;
    memset(&r, 0, sizeof(r));
    r.PlaneCount = 1;
    r.Enabled = 1;
    r.SurfaceWidth = 1920; r.SurfaceHeight = 1080;
    r.SurfacePitch = r.RequiredPitch = 7680;
    r.PlaneFormat = BC250_PLANE_FORMAT_ARGB8888;
    r.SourceModeWidth = 1920; r.SourceModeHeight = 1200;
    r.TargetWidth = 1920; r.TargetHeight = 1200;
    Rect(&r.Src, 0, 0, 1920, 1080);
    Rect(&r.Dst, 0, 0, 1920, 1200);
    r.Clip = r.Dst;
    r.StretchQualityKnown = 1;
    return r;
}

static unsigned long Verdict(const BC250_PLANE_REQUEST* R)
{
    BC250_PLANE_PLAN p;
    unsigned long v = Bc250PlanePrepare(R, &p);
    if (v != BC250_PLANE_OK) {
        // A refusal leaves a zero plan.
        static BC250_PLANE_PLAN zero;
        CHECK(memcmp(&p, &zero, sizeof(p)) == 0);
    }
    return v;
}

static void TestVerdicts(void)
{
    BC250_PLANE_REQUEST r;
    BC250_PLANE_PLAN p;

    r = LabRequest(); CHECK(Verdict(&r) == BC250_PLANE_OK);
    CHECK(Bc250PlanePrepare(NULL, &p) == BC250_PLANE_ARGS);
    CHECK(Bc250PlanePrepare(&r, NULL) == BC250_PLANE_ARGS);
    r = LabRequest(); r.PlaneCount = 2; CHECK(Verdict(&r) == BC250_PLANE_COUNT);
    r = LabRequest(); r.PlaneCount = 0; CHECK(Verdict(&r) == BC250_PLANE_COUNT);
    r = LabRequest(); r.LayerIndex = 1; CHECK(Verdict(&r) == BC250_PLANE_LAYER);
    r = LabRequest(); r.Enabled = 0; CHECK(Verdict(&r) == BC250_PLANE_DISABLED);
    r = LabRequest(); Rect(&r.Dst, 0, 0, 0, 1200); CHECK(Verdict(&r) == BC250_PLANE_ARGS);
    r = LabRequest(); Rect(&r.Src, 10, 0, 5, 1080); CHECK(Verdict(&r) == BC250_PLANE_ARGS);
    r = LabRequest(); Rect(&r.Clip, -1, 0, 1920, 1200); CHECK(Verdict(&r) == BC250_PLANE_ARGS);
    // A 1920x1080 source mode that stage A scales to the 1920x1200 timing.
    r = LabRequest(); r.SourceModeHeight = 1080; Rect(&r.Dst, 0, 0, 1920, 1080); r.Clip = r.Dst;
    CHECK(Verdict(&r) == BC250_PLANE_SOURCE_MODE);
    r = LabRequest(); r.Mirror = 1; CHECK(Verdict(&r) == BC250_PLANE_MIRROR);
    r = LabRequest(); r.Rotated = 1; CHECK(Verdict(&r) == BC250_PLANE_ROTATION);
    r = LabRequest(); r.AlphaBlend = 1; CHECK(Verdict(&r) == BC250_PLANE_BLEND);
    r = LabRequest(); r.Interlaced = 1; CHECK(Verdict(&r) == BC250_PLANE_INTERLACED);
    r = LabRequest(); r.Stereo = 1; CHECK(Verdict(&r) == BC250_PLANE_STEREO);
    r = LabRequest(); r.NotSrgb = 1; CHECK(Verdict(&r) == BC250_PLANE_COLOR_SPACE);
    r = LabRequest(); r.StretchQualityKnown = 0; CHECK(Verdict(&r) == BC250_PLANE_STRETCH_QUALITY);
    r = LabRequest(); r.PlaneFormat = BC250_PLANE_FORMAT_NONE; CHECK(Verdict(&r) == BC250_PLANE_FORMAT);
    r = LabRequest(); r.PlaneFormat = BC250_PLANE_FORMATS; CHECK(Verdict(&r) == BC250_PLANE_FORMAT);
    r = LabRequest(); r.PlaneFormat = BC250_PLANE_FORMAT_ABGR2101010; CHECK(Verdict(&r) == BC250_PLANE_FORMAT);
    r.TenBitSource = 1; CHECK(Verdict(&r) == BC250_PLANE_OK);
    r = LabRequest(); r.PlaneFormat = BC250_PLANE_FORMAT_ABGR8888; CHECK(Verdict(&r) == BC250_PLANE_OK);
    r = LabRequest(); r.SurfaceWidth = 63; Rect(&r.Src, 0, 0, 63, 1080); CHECK(Verdict(&r) == BC250_PLANE_SURFACE);
    r = LabRequest(); r.SurfaceHeight = 0x4000; Rect(&r.Src, 0, 0, 1920, 0x4000); CHECK(Verdict(&r) == BC250_PLANE_SURFACE);
    r = LabRequest(); Rect(&r.Src, 0, 0, 1280, 720); CHECK(Verdict(&r) == BC250_PLANE_SRC_CROP);
    // The committed pipe's pitch: a wider row is refused, and 0 means no rule.
    r = LabRequest(); r.SurfacePitch = 8192; CHECK(Verdict(&r) == BC250_PLANE_PITCH);
    r.RequiredPitch = 0; CHECK(Verdict(&r) == BC250_PLANE_OK);
    r = LabRequest(); Rect(&r.Src, 1, 0, 1921, 1080); CHECK(Verdict(&r) == BC250_PLANE_SRC_CROP);
    r = LabRequest(); Rect(&r.Dst, 0, 0, 1920, 1201); r.Clip = r.Dst; CHECK(Verdict(&r) == BC250_PLANE_DST_BOUNDS);
    r = LabRequest(); Rect(&r.Dst, 1, 0, 1921, 1200); r.Clip = r.Dst; CHECK(Verdict(&r) == BC250_PLANE_DST_BOUNDS);
    r = LabRequest(); Rect(&r.Clip, 0, 0, 1920, 1199); CHECK(Verdict(&r) == BC250_PLANE_CLIP);
    r = LabRequest(); Rect(&r.Clip, 0, 1, 1920, 1200); CHECK(Verdict(&r) == BC250_PLANE_CLIP);
    // A clip larger than the plane leaves it whole.
    r = LabRequest(); Rect(&r.Dst, 0, 60, 1920, 1140); Rect(&r.Clip, 0, 0, 1920, 1200); CHECK(Verdict(&r) == BC250_PLANE_OK);
    r = LabRequest(); Rect(&r.Dst, 0, 0, 1280, 1200); r.Clip = r.Dst; CHECK(Verdict(&r) == BC250_PLANE_DOWNSCALE);
    r = LabRequest(); Rect(&r.Dst, 0, 0, 1920, 1000); r.Clip = r.Dst; CHECK(Verdict(&r) == BC250_PLANE_DOWNSCALE);
    // 16 times up is the limit: 120x75 fills 1920x1200 exactly; one pixel less in either direction is refused.
    r = LabRequest(); r.RequiredPitch = 0; r.SurfaceWidth = 120; r.SurfaceHeight = 75; Rect(&r.Src, 0, 0, 120, 75);
    CHECK(Verdict(&r) == BC250_PLANE_OK);
    r.SurfaceWidth = 119; Rect(&r.Src, 0, 0, 119, 75); CHECK(Verdict(&r) == BC250_PLANE_UPSCALE);
    r.SurfaceWidth = 120; r.SurfaceHeight = 74; Rect(&r.Src, 0, 0, 120, 74); CHECK(Verdict(&r) == BC250_PLANE_UPSCALE);
    // A target wider than the scaler's fields: dcn_scale.c refuses the plan.
    r = LabRequest(); r.SourceModeWidth = r.TargetWidth = 0x4000; CHECK(Verdict(&r) == BC250_PLANE_SCALER);
    // The vocabulary.
    CHECK(strcmp(Bc250PlaneVerdictText(BC250_PLANE_OK), "ok") == 0);
    CHECK(strcmp(Bc250PlaneVerdictText(BC250_PLANE_STRETCH_QUALITY), "stretch-quality") == 0);
    CHECK(strcmp(Bc250PlaneVerdictText(BC250_PLANE_PITCH), "pitch") == 0);
    CHECK(strcmp(Bc250PlaneVerdictText(BC250_PLANE_VERDICTS), "?") == 0);
}

static void TestPlans(void)
{
    BC250_PLANE_REQUEST r;
    BC250_PLANE_PLAN p, q;
    BC250_SCALER_PLAN stretched;

    // The lab case is the stretched plan of stage A for the same sizes, with the plane's own scaling tag.
    r = LabRequest();
    CHECK(Bc250PlanePrepare(&r, &p) == BC250_PLANE_OK);
    CHECK(Bc250ScalerPlan(1920, 1080, 1920, 1200, BC250_SCALING_STRETCHED, &stretched) == 0);
    stretched.Scaling = BC250_SCALING_PLANE;
    CHECK(memcmp(&p.Scaler, &stretched, sizeof(stretched)) == 0);
    CHECK(!p.Identity);
    CHECK(p.DmlViewportWidth == 1920 && p.DmlViewportHeight == 1080);
    CHECK(p.DmlRecoutWidth == 1920 && p.DmlRecoutHeight == 1200);
    CHECK(p.DmlHTaps == 1 && p.DmlVTaps == 4);
    // The identity plane.
    r.SurfaceHeight = 1200; Rect(&r.Src, 0, 0, 1920, 1200);
    CHECK(Bc250PlanePrepare(&r, &q) == BC250_PLANE_OK);
    CHECK(q.Identity && q.DmlHTaps == 1 && q.DmlVTaps == 1);
    CHECK(!Bc250PlaneSameShape(&p, &q));
    // A full-size plane away from the origin is not the identity: it has the position of a recout.
    r = LabRequest(); r.RequiredPitch = 0; r.SurfaceWidth = 1280; r.SurfaceHeight = 720; Rect(&r.Src, 0, 0, 1280, 720);
    Rect(&r.Dst, 0, 60, 1920, 1140); r.Clip = r.Dst;
    CHECK(Bc250PlanePrepare(&r, &q) == BC250_PLANE_OK);
    CHECK(!q.Identity && q.Scaler.Recout.Y == 60 && q.Scaler.Recout.Height == 1080);
    CHECK(q.DmlHTaps == 4 && q.DmlVTaps == 4);
    CHECK(q.Scaler.Viewport.Width == 1280 && q.Scaler.Viewport.Height == 720);
    // The same request twice: the same shape, so a second flip writes no scaler register.
    CHECK(Bc250PlanePrepare(&r, &p) == BC250_PLANE_OK);
    CHECK(Bc250PlaneSameShape(&p, &q));
    r.Dst.Top = 61; r.Dst.Bottom = 1141; r.Clip = r.Dst;
    CHECK(Bc250PlanePrepare(&r, &p) == BC250_PLANE_OK);
    CHECK(!Bc250PlaneSameShape(&p, &q));
}

// The lab's mode, as dml201_test.c gives it: the Lenovo's 1920x1200 CVT-RB timing over DP.
static BC250_DML201_INPUT LabTiming(void)
{
    BC250_DML201_INPUT i;
    memset(&i, 0, sizeof(i));
    i.HActive = 1920; i.HTotal = 2080; i.HFrontPorch = 48; i.HSyncWidth = 32;
    i.VActive = 1200; i.VTotal = 1235; i.VFrontPorch = 3; i.VSyncWidth = 6;
    i.PixelClockKhz = 154000; i.PitchPixels = 1920; i.Output = BC250_DML201_DP;
    i.DpLanes = 4; i.DchubRefClockKhz = 48000; i.DentistVcoKhz = 3000000;
    return i;
}

static int Dml(const BC250_PLANE_PLAN* P, unsigned long Pitch, void* W, size_t Bytes, BC250_DML201_PLAN* Out)
{
    BC250_DML201_INPUT i = LabTiming();
    i.PitchPixels = (unsigned int)Pitch;
    i.ViewportWidth = (unsigned int)P->DmlViewportWidth;
    i.ViewportHeight = (unsigned int)P->DmlViewportHeight;
    i.RecoutWidth = (unsigned int)P->DmlRecoutWidth;
    i.RecoutHeight = (unsigned int)P->DmlRecoutHeight;
    i.HTaps = (unsigned int)P->DmlHTaps;
    i.VTaps = (unsigned int)P->DmlVTaps;
    return Bc250Dml201Plan(&i, W, Bytes, Out);
}

static void TestDml(void)
{
    size_t bytes = Bc250Dml201WorkspaceSize();
    void* w = malloc(bytes);
    BC250_DML201_INPUT lab = LabTiming();
    BC250_DML201_PLAN native, scaled;
    BC250_PLANE_REQUEST r;
    BC250_PLANE_PLAN p;
    int s;

    CHECK(w != NULL);
    if (!w) return;
    // The committed pipe: its clocks are the budget a flip cannot change.
    CHECK(Bc250Dml201Plan(&lab, w, bytes, &native) == 0 && native.Valid == 1);
    // The lab case.
    r = LabRequest();
    CHECK(Bc250PlanePrepare(&r, &p) == BC250_PLANE_OK);
    s = Dml(&p, 1920, w, bytes, &scaled);
    printf("plane 1920x1080->1920x1200: dml=%d valid=%u dispclk=%u/%u dppclk=%u/%u\n", s, scaled.Valid,
           scaled.DispClockKhz, native.DispClockKhz, scaled.DppClockKhz, native.DppClockKhz);
    CHECK(s == 0 && scaled.Valid == 1);
    CHECK(Bc250PlaneDmlVerdict(&p, s, scaled.Valid, scaled.DispClockKhz, scaled.DppClockKhz,
                               native.DispClockKhz, native.DppClockKhz) == BC250_PLANE_OK);
    // The same answer from DML with one kHz less of either budget is a refusal, and so is no budget.
    CHECK(Bc250PlaneDmlVerdict(&p, s, scaled.Valid, scaled.DispClockKhz, scaled.DppClockKhz,
                               scaled.DispClockKhz - 1, native.DppClockKhz) == BC250_PLANE_CLOCKS);
    CHECK(Bc250PlaneDmlVerdict(&p, s, scaled.Valid, scaled.DispClockKhz, scaled.DppClockKhz,
                               native.DispClockKhz, scaled.DppClockKhz - 1) == BC250_PLANE_CLOCKS);
    CHECK(Bc250PlaneDmlVerdict(&p, s, scaled.Valid, scaled.DispClockKhz, scaled.DppClockKhz, 0, 0) == BC250_PLANE_CLOCKS);
    // A DML failure.
    CHECK(Bc250PlaneDmlVerdict(&p, BC250_DML201_UNSUPPORTED, 0, 0, 0, native.DispClockKhz, native.DppClockKhz) ==
          BC250_PLANE_DML);
    CHECK(Bc250PlaneDmlVerdict(&p, 0, 0, 0, 0, native.DispClockKhz, native.DppClockKhz) == BC250_PLANE_DML);
    // A pitch under the viewport: DML refuses the input, so the plane is refused.
    s = Dml(&p, 1280, w, bytes, &scaled);
    CHECK(s != 0);
    CHECK(Bc250PlaneDmlVerdict(&p, s, scaled.Valid, scaled.DispClockKhz, scaled.DppClockKhz,
                               native.DispClockKhz, native.DppClockKhz) == BC250_PLANE_DML);
    // The letterboxed 720p plane: 4 taps both ways.
    r = LabRequest(); r.RequiredPitch = 0; r.SurfaceWidth = 1280; r.SurfaceHeight = 720; Rect(&r.Src, 0, 0, 1280, 720);
    Rect(&r.Dst, 0, 60, 1920, 1140); r.Clip = r.Dst;
    CHECK(Bc250PlanePrepare(&r, &p) == BC250_PLANE_OK);
    s = Dml(&p, 1280, w, bytes, &scaled);
    printf("plane 1280x720->1920x1080 at y 60: dml=%d valid=%u dispclk=%u dppclk=%u\n", s, scaled.Valid,
           scaled.DispClockKhz, scaled.DppClockKhz);
    CHECK(s == 0 && scaled.Valid == 1);
    // The identity plane needs no DML answer: it is the committed pipe.
    r = LabRequest(); r.SurfaceHeight = 1200; Rect(&r.Src, 0, 0, 1920, 1200);
    CHECK(Bc250PlanePrepare(&r, &p) == BC250_PLANE_OK && p.Identity);
    CHECK(Bc250PlaneDmlVerdict(&p, BC250_DML201_INVALID, 0, 0, 0, 0, 0) == BC250_PLANE_OK);
    CHECK(Bc250PlaneDmlVerdict(NULL, 0, 1, 0, 0, 1, 1) == BC250_PLANE_ARGS);
    free(w);
}

int main(void)
{
    TestVerdicts();
    TestPlans();
    TestDml();
    printf("dcn_plane: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
