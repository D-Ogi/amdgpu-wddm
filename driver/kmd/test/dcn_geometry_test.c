#include "../dcn_geometry.h"
#include "../regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"
#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)
static BC250_DCN_GEOMETRY plan;
static struct {
    unsigned reads, writes, failRead, failWrite, after, stalls, lock;
    unsigned long addr[29], value[29], mode, twoTap;
} hw;
static long Read(void* c, unsigned long reg, unsigned long* value)
{
    (void)c;
    if (++hw.reads == hw.failRead) return BC250_DISP_STATUS_DEVICE;
    if (reg == BC250_REG_DMU_OTG0_OTG_CONTROL) *value = OTG0_OTG_CONTROL__OTG_MASTER_EN_MASK;
    else if (reg == BC250_REG_DMU_OTG0_OTG_GLOBAL_CONTROL0) *value = 0;
    else if (reg == BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK) *value = hw.lock ?
        OTG0_OTG_MASTER_UPDATE_LOCK__OTG_MASTER_UPDATE_LOCK_MASK | OTG0_OTG_MASTER_UPDATE_LOCK__UPDATE_LOCK_STATUS_MASK : 0;
    else if (reg == BC250_REG_DMU_DSCL0_SCL_MODE || reg == BC250_REG_DMU_DSCL1_SCL_MODE) *value = hw.mode;
    else if (reg == BC250_REG_DMU_DSCL0_DSCL_2TAP_CONTROL || reg == BC250_REG_DMU_DSCL1_DSCL_2TAP_CONTROL) *value = hw.twoTap;
    else { CHECK(0); return BC250_DISP_STATUS_INVALID; }
    return 0;
}
static long Write(void* c, unsigned long reg, unsigned long value)
{
    unsigned n = hw.writes++;
    (void)c;
    CHECK(n < 29);
    if (n >= 29) return BC250_DISP_STATUS_INVALID;
    if (hw.failWrite == n + 1 && !hw.after) return BC250_DISP_STATUS_DEVICE;
    hw.addr[n] = reg; hw.value[n] = value;
    return hw.failWrite == n + 1 ? BC250_DISP_STATUS_DEVICE : 0;
}
static void Stall(void* c, unsigned long us) { (void)c; (void)us; ++hw.stalls; }
static BC250_DISP_IO Setup(void)
{
    BC250_DISP_IO io = {0};
    memset(&hw, 0, sizeof(hw)); hw.lock = 1;
    hw.mode = 0xfffffff8ul; hw.twoTap = 0xfffffffful;
    io.Read = Read; io.Write = Write; io.Stall = Stall;
    return io;
}
static unsigned long Value(unsigned long reg)
{
    unsigned i;
    for (i = 0; i < hw.writes; ++i) if (hw.addr[i] == reg) return hw.value[i];
    CHECK(0); return 0;
}
static BC250_SCALER_REQUEST Request(unsigned sw, unsigned sh, unsigned dw, unsigned dh)
{
    BC250_SCALER_REQUEST r = {0};
    r.Geometry.Horizontal = (BC250_SCALER_AXIS_INPUT){sw,dw,0,(int)sw,0,(int)dw,0,(int)dw,0,0};
    r.Geometry.Vertical = (BC250_SCALER_AXIS_INPUT){sh,dh,0,(int)sh,0,(int)dh,0,(int)dh,0,0};
    r.Geometry.ChromaDivisor = 1;
    return r;
}
static int Zero(void)
{
    static const BC250_DCN_GEOMETRY zero = {0};
    return memcmp(&plan, &zero, sizeof(plan)) == 0;
}
static void Cases(void)
{
    unsigned colour, kind, scale, pipe, attempted;
    for (colour = 0; colour < 3; ++colour) for (kind = 0; kind < 2; ++kind)
    for (scale = 0; scale < 4; ++scale) for (pipe = 0; pipe < 2; ++pipe) {
        unsigned sw = 640, sh = 480, dw = scale == 0 ? 640 : scale == 1 ? 1280 : scale == 2 ? 320 : 960;
        unsigned dh = dw * 3 / 4;
        unsigned expectedMode = colour == 2 ? (scale == 0 ? 4 : scale == 2 ? 5 : 3) :
            scale == 0 ? 0 : colour == 0 ? 1 : 2;
        BC250_SCALER_REQUEST r = Request(sw,sh,dw,dh);
        BC250_DISP_IO io = Setup();
        unsigned long modeReg = pipe ? BC250_REG_DMU_DSCL1_SCL_MODE : BC250_REG_DMU_DSCL0_SCL_MODE;
        unsigned long ratioReg = pipe ? BC250_REG_DMU_DSCL1_SCL_HORZ_FILTER_SCALE_RATIO : BC250_REG_DMU_DSCL0_SCL_HORZ_FILTER_SCALE_RATIO;
        unsigned long ratioCReg = pipe ? BC250_REG_DMU_DSCL1_SCL_HORZ_FILTER_SCALE_RATIO_C : BC250_REG_DMU_DSCL0_SCL_HORZ_FILTER_SCALE_RATIO_C;
        unsigned long lbReg = pipe ? BC250_REG_DMU_DSCL1_LB_DATA_FORMAT : BC250_REG_DMU_DSCL0_LB_DATA_FORMAT;
        unsigned long blackReg = pipe ? BC250_REG_DMU_DSCL1_SCL_BLACK_OFFSET : BC250_REG_DMU_DSCL0_SCL_BLACK_OFFSET;
        unsigned long ratio = (unsigned long)(((unsigned long long)sw << 19) / dw);
        r.FilterKind = kind; r.Alpha = 1;
        r.Geometry.ChromaDivisor = colour == 2 ? 2 : 1;
        CHECK(Bc250DcnGeometryPrepare(&r,colour,&plan) == 0);
        CHECK(plan.ChromaMode == (colour != 0));
        CHECK(Bc250DcnGeometryApplyLocked(&io,pipe,&plan,&attempted) == 0);
        CHECK(attempted == 29 && hw.writes == 29 && hw.reads == 5 && !hw.stalls);
        CHECK(Value(modeReg) == ((hw.mode & ~DSCL0_SCL_MODE__DSCL_MODE_MASK) | expectedMode));
        CHECK(Bc250DispGet(Value(ratioReg),DSCL0_SCL_HORZ_FILTER_SCALE_RATIO__SCL_H_SCALE_RATIO_MASK) == ratio << 5);
        CHECK(Bc250DispGet(Value(ratioCReg),DSCL0_SCL_HORZ_FILTER_SCALE_RATIO_C__SCL_H_SCALE_RATIO_C_MASK) == (ratio / r.Geometry.ChromaDivisor) << 5);
        CHECK(Value(lbReg) == DSCL0_LB_DATA_FORMAT__ALPHA_EN_MASK);
        {
            unsigned long init = Value(pipe ? BC250_REG_DMU_DSCL1_SCL_VERT_FILTER_INIT_C : BC250_REG_DMU_DSCL0_SCL_VERT_FILTER_INIT_C);
            unsigned long decoded = Bc250DispGet(init,DSCL0_SCL_VERT_FILTER_INIT_C__SCL_V_INIT_FRAC_C_MASK) >> 5;
            decoded |= Bc250DispGet(init,DSCL0_SCL_VERT_FILTER_INIT_C__SCL_V_INIT_INT_C_MASK) << 19;
            CHECK(decoded == plan.Scaler.Geometry.ChromaV.Init19);
            CHECK((Value(pipe ? BC250_REG_DMU_DSCL1_DSCL_2TAP_CONTROL : BC250_REG_DMU_DSCL0_DSCL_2TAP_CONTROL) &
                (DSCL0_DSCL_2TAP_CONTROL__SCL_H_2TAP_HARDCODE_COEF_EN_MASK | DSCL0_DSCL_2TAP_CONTROL__SCL_V_2TAP_HARDCODE_COEF_EN_MASK)) == 0);
            CHECK(Bc250DispGet(Value(pipe ? BC250_REG_DMU_DSCL1_LB_MEMORY_CTRL : BC250_REG_DMU_DSCL0_LB_MEMORY_CTRL),DSCL0_LB_MEMORY_CTRL__LB_MAX_PARTITIONS_MASK) == 63);
        }
        CHECK(Bc250DispGet(Value(blackReg),DSCL0_SCL_BLACK_OFFSET__SCL_BLACK_OFFSET_CBCR_MASK) == (colour ? 0x8000u : 0));
        CHECK(hw.addr[0] == (pipe ? BC250_REG_DMU_DSCL1_DSCL_AUTOCAL : BC250_REG_DMU_DSCL0_DSCL_AUTOCAL));
        CHECK(hw.addr[28] == (pipe ? BC250_REG_DMU_HUBP1_DCSURF_SEC_VIEWPORT_START_C : BC250_REG_DMU_HUBP0_DCSURF_SEC_VIEWPORT_START_C));
    }
}
static void Boundaries(void)
{
    BC250_SCALER_REQUEST r = Request(1920,1080,1920,1200);
    BC250_DISP_IO io;
    unsigned i, pipe, after, attempted;
    r.Geometry.Horizontal.ClipBegin = 101; r.Geometry.Vertical.ClipBegin = 51;
    r.Geometry.Horizontal.Mirror = r.Geometry.Vertical.Mirror = 1;
    r.Geometry.ChromaDivisor = 2;
    CHECK(Bc250DcnGeometryPrepare(&r,2,&plan) == 0);
    CHECK(plan.MirrorH && plan.MirrorV);
    for (pipe = 0; pipe < 2; ++pipe) {
        io = Setup();
        CHECK(Bc250DcnGeometryApplyLocked(&io,pipe,&plan,&attempted) == 0);
        CHECK(Bc250DispGet(Value(pipe ? BC250_REG_DMU_DSCL1_RECOUT_START : BC250_REG_DMU_DSCL0_RECOUT_START),DSCL0_RECOUT_START__RECOUT_START_X_MASK) == 101);
        CHECK(Bc250DispGet(Value(pipe ? BC250_REG_DMU_DSCL1_RECOUT_START : BC250_REG_DMU_DSCL0_RECOUT_START),DSCL0_RECOUT_START__RECOUT_START_Y_MASK) == 51);
        for (i = 1; i <= 5; ++i) {
            io = Setup(); hw.failRead = i;
            CHECK(Bc250DcnGeometryApplyLocked(&io,pipe,&plan,&attempted) < 0);
            CHECK(!attempted && !hw.writes && !hw.stalls);
        }
        for (after = 0; after < 2; ++after) for (i = 1; i <= 29; ++i) {
            io = Setup(); hw.failWrite = i; hw.after = after;
            CHECK(Bc250DcnGeometryApplyLocked(&io,pipe,&plan,&attempted) < 0);
            CHECK(attempted == i && hw.writes == i && !hw.stalls);
        }
    }
    io = Setup(); hw.lock = 0;
    CHECK(Bc250DcnGeometryApplyLocked(&io,0,&plan,&attempted) < 0 && !attempted && !hw.writes);
    io = Setup(); CHECK(Bc250DcnGeometryApplyLocked(&io,2,&plan,&attempted) < 0 && !hw.reads);
    plan.Words[0] = 0xfffffffful;
    CHECK(Bc250DcnGeometryApplyLocked(&io,0,&plan,&attempted) < 0 && !hw.reads);
    CHECK(Bc250DcnGeometryPrepare(&r,0,&plan) != 0 && Zero());
    r.Geometry.Horizontal.SourceBegin = 1;
    CHECK(Bc250DcnGeometryPrepare(&r,2,&plan) != 0 && Zero());
    r = Request(4117,720,5000,1200);
    CHECK(Bc250DcnGeometryPrepare(&r,0,&plan) != 0 && Zero());
    r = Request(640,480,640,480); r.Geometry.Horizontal.ClipEnd = 0;
    CHECK(Bc250DcnGeometryPrepare(&r,0,&plan) == BC250_SCALER_GEOMETRY_EMPTY && Zero());
    CHECK(Bc250DcnGeometryPrepare(NULL,0,&plan) != 0 && Zero());
    r = Request(640,480,80,60);
    CHECK(Bc250DcnGeometryPrepare(&r,0,&plan) == 0);
    CHECK(plan.Scaler.LumaV.Taps == 8);
    CHECK(plan.Scaler.Geometry.LumaV.Ratio19 == 4194303);
    io = Setup(); CHECK(Bc250DcnGeometryApplyLocked(&io,1,&plan,&attempted) == 0);
    // Exact-8 adjusted ratio and truncated 8.5 init: bottom is 16.5 minus one
    // 3.19 unit. Only its inactive integer field wraps, not the top phase.
    CHECK(Bc250DispGet(Value(BC250_REG_DMU_DSCL1_SCL_VERT_FILTER_INIT_BOT),DSCL0_SCL_VERT_FILTER_INIT_BOT__SCL_V_INIT_INT_BOT_MASK) == 0);
    CHECK(Bc250DispGet(Value(BC250_REG_DMU_DSCL1_SCL_VERT_FILTER_INIT),DSCL0_SCL_VERT_FILTER_INIT__SCL_V_INIT_INT_MASK) == 8);
}
int main(void)
{
    Cases(); Boundaries();
    printf("DCN geometry: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
