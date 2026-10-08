// Host test of dcn_scale.c: the scaler math of a smaller source mode on the native timing, the pipe precondition,
// and the register sequence against a register file of pipe 0 (display modes, stage A). Built with no WDK header
// by run_modeset.ps1.
//
// Positive controls: hand-computed layouts and fixed-point values for the lab's 1920x1200 (the examples of
// docs/design/display-modes.md), the 1:1 bypass shape amdgpu programs (E03: DSCL_MODE 0, LB_MEMORY_CTRL 0x3F01),
// every mode of the lab's mode list in every scaling the level offers, and the filter table (each phase sums to
// 1.0). Negative controls: sizes the scaler must refuse, every pipe shape the precondition must refuse, a lock that
// is never acknowledged, a stall budget of 0, and a register write that fails in the middle of the sequence.
#include <stdio.h>
#include <string.h>
#include "regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"
#include "../dcn_scale.h"
#include "../edid.h"
#include "edid_lab_redacted.h"

static int g_failures, g_checks;

#define CHECK(cond) \
    do { g_checks++; if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)

// ---- the register file -------------------------------------------------------------------------------------------

#define REGS (0x80000ul / 4ul)
#define LOG_MAX 1024

typedef struct {
    unsigned long r[REGS];
    unsigned short coef[8][33][2][2];      // [filter type][phase][pair][even, odd]
    unsigned long coefType, coefPhase, coefPair;
    long lockDelay;                         // reads of the lock register before UPDATE_LOCK_STATUS; < 0 = never
    long lockReads;
    unsigned long log[LOG_MAX][2];
    unsigned long logCount;
    unsigned long failOffset;
    unsigned long nowUs;
} FILE_MODEL;

static FILE_MODEL g_f;

#define REG(o) g_f.r[(o) / 4ul]

static void FirmwareState(void)
{
    memset(&g_f, 0, sizeof(g_f));
    // The shape the GOP leaves (E03, before amdgpu): 1920x1200 viewport, recout and MPC size, DSCL_MODE 1 at 1:1.
    REG(BC250_REG_DMU_HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION) = 0x04B00780ul;
    REG(BC250_REG_DMU_DSCL0_RECOUT_SIZE) = 0x04B00780ul;
    REG(BC250_REG_DMU_DSCL0_MPC_SIZE) = 0x04B00780ul;
    REG(BC250_REG_DMU_DSCL0_SCL_MODE) = 1;
    REG(BC250_REG_DMU_DSCL0_DSCL_2TAP_CONTROL) = 0x80000000ul;     // a bit outside the six fields: must survive
}

static long FRead(void* C, unsigned long Offset, unsigned long* Value)
{
    (void)C;
    if (Offset == BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK) {
        unsigned long v = REG(Offset) & OTG0_OTG_MASTER_UPDATE_LOCK__OTG_MASTER_UPDATE_LOCK_MASK;
        if (v && g_f.lockDelay >= 0 && g_f.lockReads++ >= g_f.lockDelay) v |= OTG0_OTG_MASTER_UPDATE_LOCK__UPDATE_LOCK_STATUS_MASK;
        *Value = v;
        return 0;
    }
    *Value = REG(Offset);
    return 0;
}

static long FWrite(void* C, unsigned long Offset, unsigned long Value)
{
    (void)C;
    if (g_f.failOffset == Offset) return (long)0xC0000185L;
    if (g_f.logCount < LOG_MAX) { g_f.log[g_f.logCount][0] = Offset; g_f.log[g_f.logCount][1] = Value; g_f.logCount++; }
    if (Offset == BC250_REG_DMU_DSCL0_SCL_COEF_RAM_TAP_SELECT) {
        g_f.coefType = (Value & DSCL0_SCL_COEF_RAM_TAP_SELECT__SCL_COEF_RAM_FILTER_TYPE_MASK) >> 16;
        g_f.coefPhase = (Value & DSCL0_SCL_COEF_RAM_TAP_SELECT__SCL_COEF_RAM_PHASE_MASK) >> 8;
        g_f.coefPair = Value & DSCL0_SCL_COEF_RAM_TAP_SELECT__SCL_COEF_RAM_TAP_PAIR_IDX_MASK;
    } else if (Offset == BC250_REG_DMU_DSCL0_SCL_COEF_RAM_TAP_DATA) {
        // The coefficient port advances by itself: pair, then phase (4 taps = 2 pairs).
        if (g_f.coefPhase < 33) {
            g_f.coef[g_f.coefType][g_f.coefPhase][g_f.coefPair][0] = (unsigned short)(Value & 0x3FFFu);
            g_f.coef[g_f.coefType][g_f.coefPhase][g_f.coefPair][1] = (unsigned short)((Value >> 16) & 0x3FFFu);
            CHECK((Value & DSCL0_SCL_COEF_RAM_TAP_DATA__SCL_COEF_RAM_EVEN_TAP_COEF_EN_MASK) &&
                  (Value & DSCL0_SCL_COEF_RAM_TAP_DATA__SCL_COEF_RAM_ODD_TAP_COEF_EN_MASK));
        }
        if (++g_f.coefPair == 2) { g_f.coefPair = 0; g_f.coefPhase++; }
    } else if (Offset == BC250_REG_DMU_OTG0_OTG_TRIGA_MANUAL_TRIG) {
        // The update the trigger releases: the coefficient bank software selected becomes the current one.
        unsigned long m = REG(BC250_REG_DMU_DSCL0_SCL_MODE);
        m &= ~DSCL0_SCL_MODE__SCL_COEF_RAM_SELECT_CURRENT_MASK;
        if (m & DSCL0_SCL_MODE__SCL_COEF_RAM_SELECT_MASK) m |= DSCL0_SCL_MODE__SCL_COEF_RAM_SELECT_CURRENT_MASK;
        REG(BC250_REG_DMU_DSCL0_SCL_MODE) = m;
    } else if (Offset == BC250_REG_DMU_DSCL0_SCL_MODE) {
        // SCL_COEF_RAM_SELECT_CURRENT is read only.
        REG(Offset) = (Value & ~DSCL0_SCL_MODE__SCL_COEF_RAM_SELECT_CURRENT_MASK) |
                      (REG(Offset) & DSCL0_SCL_MODE__SCL_COEF_RAM_SELECT_CURRENT_MASK);
        return 0;
    }
    if (Offset == BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK) g_f.lockReads = 0;
    if (Offset != BC250_REG_DMU_OTG0_OTG_TRIGA_MANUAL_TRIG) REG(Offset) = Value;
    return 0;
}

static void FStall(void* C, unsigned long Us) { (void)C; g_f.nowUs += Us; }

static void IoInit(BC250_DISP_IO* Io, unsigned long BudgetUs)
{
    memset(Io, 0, sizeof(*Io));
    Io->Read = FRead;
    Io->Write = FWrite;
    Io->Stall = FStall;
    Io->BudgetUs = BudgetUs;
}

static unsigned long CountWrites(unsigned long Offset)
{
    unsigned long i, n = 0;
    for (i = 0; i < g_f.logCount; i++) if (g_f.log[i][0] == Offset) n++;
    return n;
}

static long IndexOf(unsigned long Offset)
{
    unsigned long i;
    for (i = 0; i < g_f.logCount; i++) if (g_f.log[i][0] == Offset) return (long)i;
    return -1;
}

static int Sign14(unsigned long V) { return (V & 0x2000u) ? (int)V - 0x4000 : (int)V; }

// ---- tests -------------------------------------------------------------------------------------------------------

static void TestFilterTable(void)
{
    unsigned long p, t;
    for (p = 0; p < BC250_SCL_PHASES; p++) {
        int sum = 0;
        for (t = 0; t < BC250_SCL_TAPS; t++) sum += Sign14(g_Bc250SclFilter4tapUpscale[p * BC250_SCL_TAPS + t]);
        CHECK(sum == 4096);                             // S1.12: each phase sums to 1.0
    }
    CHECK(g_Bc250SclFilter4tapUpscale[1] == 0x1000);    // phase 0 is the identity tap
    CHECK(g_Bc250SclFilter4tapUpscale[131] == 0x3F04);  // the last entry of dce_scl_filters.c line 483
}

static void TestLayouts(void)
{
    BC250_RECT r;
    BC250_SCALER_PLAN p;

    // Aspect ratio kept: 1280x1024 (5:4) is pillarboxed to 1500x1200 at x 210; 1600x900 (16:9) letterboxed to
    // 1920x1080 at y 60; 640x480 (4:3) to 1600x1200 at x 160.
    CHECK(Bc250ScalingRecout(1280, 1024, 1920, 1200, BC250_SCALING_ASPECT, &r) == 0);
    CHECK(r.X == 210 && r.Y == 0 && r.Width == 1500 && r.Height == 1200);
    CHECK(Bc250ScalingRecout(1600, 900, 1920, 1200, BC250_SCALING_ASPECT, &r) == 0);
    CHECK(r.X == 0 && r.Y == 60 && r.Width == 1920 && r.Height == 1080);
    CHECK(Bc250ScalingRecout(640, 480, 1920, 1200, BC250_SCALING_ASPECT, &r) == 0);
    CHECK(r.X == 160 && r.Y == 0 && r.Width == 1600 && r.Height == 1200);
    CHECK(Bc250ScalingRecout(1280, 720, 1920, 1200, BC250_SCALING_CENTERED, &r) == 0);
    CHECK(r.X == 320 && r.Y == 240 && r.Width == 1280 && r.Height == 720);
    CHECK(Bc250ScalingRecout(1280, 720, 1920, 1200, BC250_SCALING_STRETCHED, &r) == 0);
    CHECK(r.X == 0 && r.Y == 0 && r.Width == 1920 && r.Height == 1200);

    // 1920x1080 stretched: horizontal 1:1, vertical 1080/1200 = 0.9.
    CHECK(Bc250ScalerPlan(1920, 1080, 1920, 1200, BC250_SCALING_STRETCHED, &p) == 0);
    CHECK(p.RatioH19 == (1ul << 19) && p.RatioV19 == 471859ul);
    CHECK(p.TapsH == 1 && p.TapsV == 4 && p.TapsHC == 1 && p.TapsVC == 2);
    CHECK(p.InitH19 == (3ul << 18));                    // (1 + 1 + 1) / 2 = 1.5
    CHECK(p.InitV19 == 1546649ul);                      // (0.9 + 5) / 2, 19 bits, truncated
    CHECK(p.DsclMode == 1 && !p.FilterH && p.FilterV);
    CHECK(p.Viewport.X == 0 && p.Viewport.Width == 1920 && p.Viewport.Height == 1080);
    CHECK(p.LbConfig == 2);                             // 1920 / 6 = 320 per line: config 1 holds 3 lines, config 2 four
    // 1920x1080 aspect: the same size as its recout, so the 1:1 bypass with black bars.
    CHECK(Bc250ScalerPlan(1920, 1080, 1920, 1200, BC250_SCALING_ASPECT, &p) == 0);
    CHECK(p.DsclMode == 0 && p.TapsV == 1 && p.Recout.Y == 60 && p.LbConfig == 1 && !p.FilterV);
    // 1280x1024 aspect: both directions 1280/1500 = 1024/1200.
    CHECK(Bc250ScalerPlan(1280, 1024, 1920, 1200, BC250_SCALING_ASPECT, &p) == 0);
    CHECK(p.RatioH19 == 447392ul && p.RatioV19 == 447392ul);
    CHECK(p.InitH19 == (447392ul + (5ul << 19)) / 2ul);
    CHECK(p.FilterH && p.FilterV && p.LbConfig == 1);   // min(1280, 1500) / 6 = 214: config 1 holds 4 lines
    // The native size: the shape amdgpu programs for 1:1 (E03).
    CHECK(Bc250ScalerPlan(1920, 1200, 1920, 1200, BC250_SCALING_IDENTITY, &p) == 0);
    CHECK(p.DsclMode == 0 && p.LbConfig == 1 && p.TapsH == 1 && p.TapsV == 1);
    CHECK(p.Recout.X == 0 && p.Recout.Width == 1920 && p.Viewport.Height == 1200);

    // Negative: larger than the target, zero, too large for the fields, Identity for another size, unknown scaling.
    CHECK(Bc250ScalingRecout(2560, 1440, 1920, 1200, BC250_SCALING_ASPECT, &r) == BC250_DISP_STATUS_NOT_SUPPORTED);
    CHECK(Bc250ScalingRecout(1920, 1201, 1920, 1200, BC250_SCALING_CENTERED, &r) == BC250_DISP_STATUS_NOT_SUPPORTED);
    CHECK(Bc250ScalingRecout(0, 1080, 1920, 1200, BC250_SCALING_ASPECT, &r) == BC250_DISP_STATUS_INVALID);
    CHECK(Bc250ScalingRecout(1280, 720, 16384, 1200, BC250_SCALING_ASPECT, &r) == BC250_DISP_STATUS_INVALID);
    CHECK(Bc250ScalingRecout(1280, 720, 1920, 1200, BC250_SCALING_IDENTITY, &r) == BC250_DISP_STATUS_NOT_SUPPORTED);
    CHECK(Bc250ScalingRecout(1280, 720, 1920, 1200, BC250_SCALING_COUNT, &r) == BC250_DISP_STATUS_INVALID);
    CHECK(r.Width == 0 && r.Height == 0);
    CHECK(Bc250ScalerPlan(2560, 1440, 1920, 1200, BC250_SCALING_STRETCHED, &p) == BC250_DISP_STATUS_NOT_SUPPORTED);
}

// Every mode of the lab's list, in every scaling its level offers, gives a plan the hardware fields can hold.
static void TestEveryLabMode(void)
{
    static BC250_EDID_INFO info;
    static BC250_MODE_LIST list;
    unsigned long i, s, level, plans = 0;

    CHECK(Bc250EdidParse(g_LabEdid, sizeof(g_LabEdid), &info) == BC250_EDID_OK);
    for (level = BC250_DISPLAY_MODES_OFF; level <= BC250_DISPLAY_MODES_SCALED; level++) {
        Bc250ModeListBuild(1920, 1200, &info, level, &list);
        for (i = 0; i < list.Count; i++) {
            unsigned long w = list.Modes[i].Width, h = list.Modes[i].Height;
            unsigned long support = Bc250ScalingSupport(level, w, h, 1920, 1200);
            CHECK(support != 0);
            CHECK(support & BC250_SCALING_BIT(Bc250ScalingDefault(level, w, h, 1920, 1200)));
            for (s = 0; s < BC250_SCALING_COUNT; s++) {
                BC250_SCALER_PLAN p;
                if (!(support & BC250_SCALING_BIT(s))) continue;
                CHECK(Bc250ScalerPlan(w, h, 1920, 1200, s, &p) == 0);
                CHECK(p.Viewport.Width == w && p.Viewport.Height == h);
                CHECK(p.Recout.X + p.Recout.Width <= 1920 && p.Recout.Y + p.Recout.Height <= 1200);
                CHECK(p.Recout.Width >= w && p.Recout.Height >= h);
                CHECK(p.RatioH19 <= (1ul << 19) && p.RatioV19 <= (1ul << 19));
                CHECK((p.InitH19 >> 19) < 16 && ((p.InitV19 + p.RatioV19) >> 19) < 16);
                CHECK(p.LbConfig <= 2);
                if (level == BC250_DISPLAY_MODES_CENTERED) CHECK(p.DsclMode == 0);
                plans++;
            }
        }
    }
    // Level 0: the native mode (identity). Level 1: native (identity, centered) + 15 centered. Level 2: native
    // (identity and the three scalings, each the 1:1 shape) and fifteen others (centered, stretched, aspect ratio).
    CHECK(plans == 1 + (2 + 15) + (4 + 15 * 3));
}

static void TestSupport(void)
{
    unsigned long I = BC250_SCALING_BIT(BC250_SCALING_IDENTITY), C = BC250_SCALING_BIT(BC250_SCALING_CENTERED);
    unsigned long S = BC250_SCALING_BIT(BC250_SCALING_STRETCHED), A = BC250_SCALING_BIT(BC250_SCALING_ASPECT);

    CHECK(Bc250ScalingSupport(BC250_DISPLAY_MODES_SCALED, 1920, 1200, 1920, 1200) == (I | C | S | A));
    CHECK(Bc250ScalingSupport(BC250_DISPLAY_MODES_CENTERED, 1920, 1200, 1920, 1200) == (I | C));
    CHECK(Bc250ScalingSupport(BC250_DISPLAY_MODES_SCALED, 1920, 1080, 1920, 1200) == (C | S | A));
    CHECK(Bc250ScalingSupport(BC250_DISPLAY_MODES_SCALED, 1680, 1050, 1920, 1200) == (C | S | A)); // same 16:10
    CHECK(Bc250ScalingSupport(BC250_DISPLAY_MODES_CENTERED, 1920, 1080, 1920, 1200) == C);
    CHECK(Bc250ScalingSupport(BC250_DISPLAY_MODES_OFF, 1920, 1200, 1920, 1200) == I);
    CHECK(Bc250ScalingSupport(BC250_DISPLAY_MODES_OFF, 1920, 1080, 1920, 1200) == 0);
    CHECK(Bc250ScalingSupport(BC250_DISPLAY_MODES_SCALED, 2560, 1440, 1920, 1200) == 0);
    CHECK(Bc250ScalingDefault(BC250_DISPLAY_MODES_SCALED, 1600, 900, 1920, 1200) == BC250_SCALING_ASPECT);
    CHECK(Bc250ScalingDefault(BC250_DISPLAY_MODES_SCALED, 1680, 1050, 1920, 1200) == BC250_SCALING_ASPECT);
    CHECK(Bc250ScalingDefault(BC250_DISPLAY_MODES_SCALED, 1920, 1200, 1920, 1200) == BC250_SCALING_IDENTITY);
    CHECK(Bc250ScalingDefault(BC250_DISPLAY_MODES_CENTERED, 1600, 900, 1920, 1200) == BC250_SCALING_CENTERED);
    CHECK(strcmp(Bc250ScalingName(BC250_SCALING_ASPECT), "aspect-ratio") == 0);
}

static void TestShape(void)
{
    BC250_DISP_IO io;
    BC250_PIPE_SHAPE s;
    unsigned long level;

    FirmwareState();
    IoInit(&io, 100);
    CHECK(Bc250PipeShapeRead(&io, &s) == 0 && io.Writes == 0 && io.Refusals == 0);
    CHECK(Bc250PipeShapeCheck(&s, 1920, 1200, &level) == BC250_PIPE_OK && level == BC250_DISPLAY_MODES_SCALED);
    // The other native size the shape does not describe.
    CHECK(Bc250PipeShapeCheck(&s, 1920, 1080, &level) == BC250_PIPE_VIEWPORT && level == BC250_DISPLAY_MODES_OFF);
    s.ViewportStart = 0x00000010ul;
    CHECK(Bc250PipeShapeCheck(&s, 1920, 1200, &level) == BC250_PIPE_VIEWPORT && level == 0);
    Bc250PipeShapeRead(&io, &s); s.RecoutStart = 0x00100000ul;
    CHECK(Bc250PipeShapeCheck(&s, 1920, 1200, &level) == BC250_PIPE_RECOUT);
    Bc250PipeShapeRead(&io, &s); s.MpcSize = 0x04380780ul;
    CHECK(Bc250PipeShapeCheck(&s, 1920, 1200, &level) == BC250_PIPE_MPC_SIZE);
    Bc250PipeShapeRead(&io, &s); s.SclMode = 3;
    CHECK(Bc250PipeShapeCheck(&s, 1920, 1200, &level) == BC250_PIPE_DSCL_MODE);
    Bc250PipeShapeRead(&io, &s); s.MpccTopSel = 1;
    CHECK(Bc250PipeShapeCheck(&s, 1920, 1200, &level) == BC250_PIPE_MPCC_TOP);
    Bc250PipeShapeRead(&io, &s); s.OdmSource = 0x100;
    CHECK(Bc250PipeShapeCheck(&s, 1920, 1200, &level) == BC250_PIPE_ODM);
    Bc250PipeShapeRead(&io, &s); s.MemPwrCtrl = 3;
    CHECK(Bc250PipeShapeCheck(&s, 1920, 1200, &level) == BC250_PIPE_COEF_POWER && level == BC250_DISPLAY_MODES_CENTERED);
    CHECK(strcmp(Bc250PipeVerdictText(BC250_PIPE_ODM), "OPTC segment 0 is not fed by OPP 0") == 0);
}

static void TestProgram(void)
{
    BC250_DISP_IO io;
    BC250_SCALER_PLAN p;
    BC250_SCALER_RESULT res;
    unsigned long i, ph;

    // 1920x1080 stretched on 1920x1200.
    FirmwareState();
    g_f.lockDelay = 2;
    IoInit(&io, 100);
    CHECK(Bc250ScalerPlan(1920, 1080, 1920, 1200, BC250_SCALING_STRETCHED, &p) == 0);
    CHECK(Bc250ScalerProgram(&io, &p, &res) == 0);
    CHECK(io.Refusals == 0 && res.LockWaitUs == 2);
    // The lock first, the unlock and the trigger last.
    CHECK(g_f.log[0][0] == BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK && g_f.log[0][1] == 1);
    CHECK(g_f.log[g_f.logCount - 2][0] == BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK && g_f.log[g_f.logCount - 2][1] == 0);
    CHECK(g_f.log[g_f.logCount - 1][0] == BC250_REG_DMU_OTG0_OTG_TRIGA_MANUAL_TRIG);
    CHECK(CountWrites(BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK) == 2);
    // The values.
    CHECK(REG(BC250_REG_DMU_DSCL0_RECOUT_START) == 0 && REG(BC250_REG_DMU_DSCL0_RECOUT_SIZE) == 0x04B00780ul);
    CHECK(REG(BC250_REG_DMU_DSCL0_MPC_SIZE) == 0x04B00780ul);
    CHECK((REG(BC250_REG_DMU_DSCL0_SCL_MODE) & DSCL0_SCL_MODE__DSCL_MODE_MASK) == 1);
    CHECK(REG(BC250_REG_DMU_DSCL0_LB_MEMORY_CTRL) == 0x3F02ul && REG(BC250_REG_DMU_DSCL0_LB_DATA_FORMAT) == 0);
    CHECK(REG(BC250_REG_DMU_DSCL0_SCL_HORZ_FILTER_SCALE_RATIO) == (1ul << 24));
    CHECK(REG(BC250_REG_DMU_DSCL0_SCL_VERT_FILTER_SCALE_RATIO) == (471859ul << 5));
    CHECK(REG(BC250_REG_DMU_DSCL0_SCL_VERT_FILTER_SCALE_RATIO_C) == (471859ul << 5));
    // INIT: 1546649 = 2 + 0.95 -> INT 2, FRAC (1546649 & 0x7FFFF) << 5.
    CHECK(REG(BC250_REG_DMU_DSCL0_SCL_VERT_FILTER_INIT) == ((2ul << 24) | ((1546649ul & 0x7FFFFul) << 5)));
    CHECK(REG(BC250_REG_DMU_DSCL0_SCL_VERT_FILTER_INIT_BOT) ==
          ((((1546649ul + 471859ul) >> 19) << 24) | (((1546649ul + 471859ul) & 0x7FFFFul) << 5)));
    CHECK(REG(BC250_REG_DMU_DSCL0_SCL_HORZ_FILTER_INIT) == ((1ul << 24) | (0x40000ul << 5)));
    CHECK(REG(BC250_REG_DMU_DSCL0_SCL_TAP_CONTROL) == 0x103ul);          // V 4, H 1, V_C 2, H_C 1 (minus one)
    CHECK(REG(BC250_REG_DMU_DSCL0_DSCL_2TAP_CONTROL) == 0x80000000ul);  // six fields 0, the rest kept
    CHECK(REG(BC250_REG_DMU_HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION) == 0x04380780ul);
    CHECK(REG(BC250_REG_DMU_HUBP0_DCSURF_SEC_VIEWPORT_DIMENSION) == 0x04380780ul);
    CHECK(REG(BC250_REG_DMU_HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION_C) == 0x04380780ul);
    CHECK(REG(BC250_REG_DMU_HUBP0_DCSURF_SEC_VIEWPORT_DIMENSION_C) == 0x04380780ul);
    // Only the vertical filter is loaded (type 0), every phase, then the bank swap.
    CHECK(CountWrites(BC250_REG_DMU_DSCL0_SCL_COEF_RAM_TAP_SELECT) == 1 && res.CoefWrites == 66);
    CHECK(g_f.log[IndexOf(BC250_REG_DMU_DSCL0_SCL_COEF_RAM_TAP_SELECT)][1] == 0);
    for (ph = 0; ph < 33; ph++)
        for (i = 0; i < 4; i++) CHECK(g_f.coef[0][ph][i / 2][i % 2] == g_Bc250SclFilter4tapUpscale[ph * 4 + i]);
    CHECK((res.SclModeAfter & DSCL0_SCL_MODE__SCL_COEF_RAM_SELECT_MASK) != 0);           // !CURRENT (was 0)
    CHECK((res.SclModeAfter & DSCL0_SCL_MODE__SCL_COEF_RAM_SELECT_CURRENT_MASK) != 0);   // latched by the trigger
    CHECK(IndexOf(BC250_REG_DMU_DSCL0_SCL_COEF_RAM_TAP_SELECT) > IndexOf(BC250_REG_DMU_DSCL0_SCL_TAP_CONTROL));

    // 1280x1024 aspect: both filters, horizontal (type 1) first; the bank flips back to 0.
    g_f.logCount = 0;
    memset(g_f.coef, 0, sizeof(g_f.coef));
    IoInit(&io, 100);
    CHECK(Bc250ScalerPlan(1280, 1024, 1920, 1200, BC250_SCALING_ASPECT, &p) == 0);
    CHECK(Bc250ScalerProgram(&io, &p, &res) == 0 && res.CoefWrites == 132);
    CHECK(CountWrites(BC250_REG_DMU_DSCL0_SCL_COEF_RAM_TAP_SELECT) == 2);
    CHECK(g_f.log[IndexOf(BC250_REG_DMU_DSCL0_SCL_COEF_RAM_TAP_SELECT)][1] == (1ul << 16));
    CHECK(g_f.coef[1][32][1][1] == 0x3F04 && g_f.coef[0][32][1][1] == 0x3F04);
    CHECK((res.SclModeAfter & DSCL0_SCL_MODE__SCL_COEF_RAM_SELECT_MASK) == 0);
    CHECK(REG(BC250_REG_DMU_DSCL0_RECOUT_START) == 210ul && REG(BC250_REG_DMU_DSCL0_RECOUT_SIZE) == 0x04B005DCul);
    CHECK(REG(BC250_REG_DMU_HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION) == 0x04000500ul);
    CHECK(REG(BC250_REG_DMU_DSCL0_LB_MEMORY_CTRL) == 0x3F01ul);

    // 1280x720 centered: bypass, no ratio, no filter; the ratio registers keep the previous values.
    g_f.logCount = 0;
    IoInit(&io, 100);
    CHECK(Bc250ScalerPlan(1280, 720, 1920, 1200, BC250_SCALING_CENTERED, &p) == 0);
    CHECK(Bc250ScalerProgram(&io, &p, &res) == 0 && res.CoefWrites == 0);
    CHECK(CountWrites(BC250_REG_DMU_DSCL0_SCL_HORZ_FILTER_SCALE_RATIO) == 0);
    CHECK(CountWrites(BC250_REG_DMU_DSCL0_SCL_COEF_RAM_TAP_SELECT) == 0);
    CHECK((REG(BC250_REG_DMU_DSCL0_SCL_MODE) & DSCL0_SCL_MODE__DSCL_MODE_MASK) == 0);
    CHECK(REG(BC250_REG_DMU_DSCL0_RECOUT_START) == ((240ul << 16) | 320ul));
    CHECK(REG(BC250_REG_DMU_DSCL0_RECOUT_SIZE) == ((720ul << 16) | 1280ul));
    CHECK(REG(BC250_REG_DMU_MPCC0_MPCC_BG_R_CR) == 0 && REG(BC250_REG_DMU_MPCC0_MPCC_BG_G_Y) == 0);

    // Back to native: amdgpu's 1:1 shape, which the precondition accepts again.
    IoInit(&io, 100);
    CHECK(Bc250ScalerRestoreNative(&io, 1920, 1200, &res) == 0);
    CHECK(REG(BC250_REG_DMU_HUBP0_DCSURF_PRI_VIEWPORT_START) == 0);
    CHECK(REG(BC250_REG_DMU_HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION) == 0x04B00780ul);
    CHECK(REG(BC250_REG_DMU_DSCL0_RECOUT_START) == 0 && REG(BC250_REG_DMU_DSCL0_RECOUT_SIZE) == 0x04B00780ul);
    CHECK((REG(BC250_REG_DMU_DSCL0_SCL_MODE) & DSCL0_SCL_MODE__DSCL_MODE_MASK) == 0);
    CHECK(REG(BC250_REG_DMU_DSCL0_LB_MEMORY_CTRL) == 0x3F01ul);
    {
        BC250_PIPE_SHAPE s;
        unsigned long level;
        CHECK(Bc250PipeShapeRead(&io, &s) == 0);
        CHECK(Bc250PipeShapeCheck(&s, 1920, 1200, &level) == BC250_PIPE_OK);
    }
}

static void TestProgramRefusals(void)
{
    BC250_DISP_IO io;
    BC250_SCALER_PLAN p;
    BC250_SCALER_RESULT res;

    CHECK(Bc250ScalerPlan(1600, 900, 1920, 1200, BC250_SCALING_ASPECT, &p) == 0);
    // The lock is never acknowledged: lock, unlock, nothing else, no trigger.
    FirmwareState();
    g_f.lockDelay = -1;
    IoInit(&io, 100);
    CHECK(Bc250ScalerProgram(&io, &p, &res) == BC250_DISP_STATUS_TIMEOUT);
    CHECK(g_f.logCount == 2 && g_f.log[1][0] == BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK && g_f.log[1][1] == 0);
    CHECK(res.LockWaitUs == 10 && io.StalledUs == 10);
    CHECK(REG(BC250_REG_DMU_DSCL0_RECOUT_SIZE) == 0x04B00780ul);
    // A budget of 0 and a lock that needs one poll more: the same, without a single stall.
    FirmwareState();
    g_f.lockDelay = 1;
    IoInit(&io, 0);
    CHECK(Bc250ScalerProgram(&io, &p, &res) == BC250_DISP_STATUS_BUDGET && g_f.logCount == 2 && io.StalledUs == 0);
    // A write that fails in the middle: the sequence stops, unlocks, no trigger.
    FirmwareState();
    g_f.failOffset = BC250_REG_DMU_DSCL0_SCL_TAP_CONTROL;
    IoInit(&io, 100);
    CHECK(Bc250ScalerProgram(&io, &p, &res) == (long)0xC0000185L);
    CHECK(g_f.log[g_f.logCount - 1][0] == BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK && g_f.log[g_f.logCount - 1][1] == 0);
    CHECK(CountWrites(BC250_REG_DMU_OTG0_OTG_TRIGA_MANUAL_TRIG) == 0);
    CHECK(CountWrites(BC250_REG_DMU_DSCL0_SCL_COEF_RAM_TAP_DATA) == 0);
    // A plan that was refused is never programmed.
    FirmwareState();
    IoInit(&io, 100);
    CHECK(Bc250ScalerPlan(2560, 1440, 1920, 1200, BC250_SCALING_ASPECT, &p) < 0);
    CHECK(Bc250ScalerProgram(&io, &p, &res) == BC250_DISP_STATUS_INVALID && g_f.logCount == 0);
    CHECK(Bc250ScalerRestoreNative(&io, 0, 1200, &res) == BC250_DISP_STATUS_INVALID && g_f.logCount == 0);
}

int main(void)
{
    TestFilterTable();
    TestLayouts();
    TestEveryLabMode();
    TestSupport();
    TestShape();
    TestProgram();
    TestProgramRefusals();
    printf("dcn_scale_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
