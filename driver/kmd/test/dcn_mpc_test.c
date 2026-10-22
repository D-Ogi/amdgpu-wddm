#include "../dcn_mpc.h"
#include "../regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"
#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)
#define FIELD(v, f) Bc250DispGet((v), MPCC0_MPCC_CONTROL__##f##_MASK)
static struct {
    unsigned reads, writes, failRead, failWrite, after, stalls, lock, selector;
    unsigned long addr[23], value[23];
    unsigned liveCount;
    unsigned long liveAddr[23], liveValue[23];
} hw;
static long Read(void* c, unsigned long reg, unsigned long* value)
{
    unsigned i;
    (void)c;
    if (++hw.reads == hw.failRead) return BC250_DISP_STATUS_DEVICE;
    if (reg == BC250_REG_DMU_OTG0_OTG_CONTROL) *value = OTG0_OTG_CONTROL__OTG_MASTER_EN_MASK;
    else if (reg == BC250_REG_DMU_OTG0_OTG_GLOBAL_CONTROL0) *value = 0;
    else if (reg == BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK) *value = hw.lock ?
        OTG0_OTG_MASTER_UPDATE_LOCK__OTG_MASTER_UPDATE_LOCK_MASK | OTG0_OTG_MASTER_UPDATE_LOCK__UPDATE_LOCK_STATUS_MASK : 0;
    else if (reg == BC250_REG_DMU_MPCC0_MPCC_UPDATE_LOCK_SEL || reg == BC250_REG_DMU_MPCC1_MPCC_UPDATE_LOCK_SEL) *value = hw.selector;
    else if (reg == BC250_REG_DMU_MPCC0_MPCC_CONTROL || reg == BC250_REG_DMU_MPCC1_MPCC_CONTROL ||
             reg == BC250_REG_DMU_MPCC0_MPCC_SM_CONTROL || reg == BC250_REG_DMU_MPCC1_MPCC_SM_CONTROL ||
             reg == BC250_REG_DMU_MPC_OUT0_MUX) *value = 0xfffffffful;
    else { CHECK(0); return BC250_DISP_STATUS_INVALID; }
    for (i = 0; i < hw.liveCount; ++i) if (hw.liveAddr[i] == reg) *value = hw.liveValue[i];
    return 0;
}
static long Write(void* c, unsigned long reg, unsigned long value)
{
    unsigned n = hw.writes++;
    (void)c;
    CHECK(n < 23);
    if (n >= 23) return BC250_DISP_STATUS_INVALID;
    CHECK(reg != BC250_REG_DMU_MPCC0_MPCC_UPDATE_LOCK_SEL && reg != BC250_REG_DMU_MPCC1_MPCC_UPDATE_LOCK_SEL);
    CHECK(reg != BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK);
    if (hw.failWrite == n + 1 && !hw.after) return BC250_DISP_STATUS_DEVICE;
    hw.addr[n] = reg; hw.value[n] = value;
    {
        unsigned i;
        for (i = 0; i < hw.liveCount; ++i) if (hw.liveAddr[i] == reg) break;
        CHECK(i < 23);
        if (i < 23) {
            hw.liveAddr[i] = reg; hw.liveValue[i] = value;
            if (i == hw.liveCount) ++hw.liveCount;
        }
    }
    return hw.failWrite == n + 1 ? BC250_DISP_STATUS_DEVICE : 0;
}
static void Stall(void* c, unsigned long us) { (void)c; (void)us; ++hw.stalls; }
static BC250_DISP_IO Setup(void)
{
    BC250_DISP_IO io = {0};
    memset(&hw, 0, sizeof(hw)); hw.lock = 1;
    io.Read = Read; io.Write = Write; io.Stall = Stall;
    return io;
}
static BC250_MPC_REQUEST Request(unsigned mask, unsigned order)
{
    BC250_MPC_REQUEST r = {0};
    r.Plane[0].Enabled = mask & 1; r.Plane[1].Enabled = (mask >> 1) & 1;
    r.Plane[0].ZOrder = order; r.Plane[1].ZOrder = order ^ 1u;
    return r;
}
static void Graph(const BC250_MPC_PLAN* plan)
{
    unsigned seen = 0, node = plan->Output, count = 0;
    while (node != 15 && count++ < 3) {
        unsigned base;
        CHECK(node < 2);
        if (node >= 2) break;
        CHECK(!(seen & (1u << node)));
        seen |= 1u << node; base = node * BC250_MPC_WORDS_PER_PLANE;
        CHECK(plan->Words[base + 7] == node); // input DPP
        CHECK(plan->Words[base + 9] == 0); // common OPP
        node = (unsigned)plan->Words[base + 8];
    }
    CHECK(count <= 2 && seen == plan->EnabledMask);
}
static void Combinations(void)
{
    unsigned mask, order, pixel, global, premult, a, p, attempted;
    const unsigned alpha[] = {0,1,127,254,255};
    for (mask = 0; mask < 4; ++mask) for (order = 0; order < 2; ++order)
    for (pixel = 0; pixel < 4; ++pixel) for (global = 0; global < 4; ++global)
    for (premult = 0; premult < 4; ++premult) for (a = 0; a < 5; ++a) {
        BC250_MPC_REQUEST r = Request(mask, order);
        BC250_MPC_PLAN plan;
        BC250_DISP_IO io = Setup();
        for (p = 0; p < 2; ++p) {
            r.Plane[p].PerPixelAlpha = (pixel >> p) & 1;
            r.Plane[p].GlobalAlphaEnabled = (global >> p) & 1;
            r.Plane[p].PremultipliedAlpha = (premult >> p) & 1;
            r.Plane[p].GlobalAlpha = alpha[(a + p) % 5];
        }
        CHECK(Bc250MpcPrepare(&r,&plan));
        CHECK(plan.Ready == 1 && plan.EnabledMask == mask);
        CHECK(plan.Output == (mask == 0 ? 15u : mask == 1 ? 0u : mask == 2 ? 1u : order ^ 1u));
        Graph(&plan);
        for (p = 0; p < 2; ++p) {
            const BC250_MPC_PLANE* q = &r.Plane[p];
            unsigned base = p * BC250_MPC_WORDS_PER_PLANE;
            unsigned long v = plan.Words[base];
            CHECK(FIELD(v,MPCC_MODE) == (!q->Enabled ? 0u : mask == 3 && p == plan.Output ? 3u : 2u));
            CHECK(FIELD(v,MPCC_ALPHA_BLND_MODE) == (q->PerPixelAlpha ? q->GlobalAlphaEnabled : 2u));
            CHECK(FIELD(v,MPCC_ALPHA_MULTIPLIED_MODE) == (q->PerPixelAlpha && q->PremultipliedAlpha));
            CHECK(FIELD(v,MPCC_GLOBAL_ALPHA) == (q->GlobalAlphaEnabled ? q->GlobalAlpha : 255));
            CHECK(FIELD(v,MPCC_GLOBAL_GAIN) == (q->PerPixelAlpha && q->GlobalAlphaEnabled ? q->GlobalAlpha : 255));
            CHECK(FIELD(v,MPCC_BG_BPC) == 4 && FIELD(v,MPCC_BOT_GAIN_MODE) == 0);
            CHECK(FIELD(v,MPCC_BLND_ACTIVE_OVERLAP_ONLY) == 0);
            CHECK(plan.Words[base+1] == 0x1f000 && plan.Words[base+2] == 0x1f000 && plan.Words[base+3] == 0x1f000);
            CHECK(plan.Words[base+4] == 0 && plan.Words[base+5] == 0 && plan.Words[base+6] == 0);
            CHECK(plan.Words[base+10] == 0);
        }
        CHECK(Bc250MpcApplyLocked(&io,&plan,1,&attempted) == 0);
        CHECK(attempted == 23 && hw.reads == 10 && hw.writes == 23 && !hw.stalls);
        CHECK(hw.addr[22] == BC250_REG_DMU_MPC_OUT0_MUX);
        CHECK(hw.value[22] == ((~MPC_OUT0_MUX__MPC_OUT_MUX_MASK) | plan.Output));
        // Reset unrelated control/stereo bits to ones: writes must preserve them.
        for (p = 0; p < 2; ++p) {
            unsigned base = p * BC250_MPC_WORDS_PER_PLANE;
            const unsigned long controlMask = MPCC0_MPCC_CONTROL__MPCC_MODE_MASK |
                MPCC0_MPCC_CONTROL__MPCC_ALPHA_BLND_MODE_MASK | MPCC0_MPCC_CONTROL__MPCC_ALPHA_MULTIPLIED_MODE_MASK |
                MPCC0_MPCC_CONTROL__MPCC_BLND_ACTIVE_OVERLAP_ONLY_MASK | MPCC0_MPCC_CONTROL__MPCC_BG_BPC_MASK |
                MPCC0_MPCC_CONTROL__MPCC_BOT_GAIN_MODE_MASK | MPCC0_MPCC_CONTROL__MPCC_GLOBAL_ALPHA_MASK |
                MPCC0_MPCC_CONTROL__MPCC_GLOBAL_GAIN_MASK;
            const unsigned long stereoMask = MPCC0_MPCC_SM_CONTROL__MPCC_SM_EN_MASK |
                MPCC0_MPCC_SM_CONTROL__MPCC_SM_MODE_MASK | MPCC0_MPCC_SM_CONTROL__MPCC_SM_FRAME_ALT_MASK |
                MPCC0_MPCC_SM_CONTROL__MPCC_SM_FIELD_ALT_MASK | MPCC0_MPCC_SM_CONTROL__MPCC_SM_FORCE_NEXT_FRAME_POL_MASK |
                MPCC0_MPCC_SM_CONTROL__MPCC_SM_FORCE_NEXT_TOP_POL_MASK;
            CHECK(hw.value[base] == (plan.Words[base] | ~controlMask));
            CHECK(hw.value[base+10] == ~stereoMask);
        }
    }
}
static void Faults(void)
{
    BC250_MPC_REQUEST r = Request(3,0);
    BC250_MPC_PLAN plan, zero = {0};
    BC250_DISP_IO io;
    unsigned n, after, attempted;
    CHECK(Bc250MpcPrepare(&r,&plan));
    for (n = 1; n <= 10; ++n) {
        io = Setup(); hw.failRead = n;
        CHECK(Bc250MpcApplyLocked(&io,&plan,0,&attempted) == BC250_DISP_STATUS_DEVICE);
        CHECK(hw.reads == n && !hw.writes && !attempted && !hw.stalls);
    }
    for (after = 0; after < 2; ++after) for (n = 1; n <= 23; ++n) {
        io = Setup(); hw.failWrite = n; hw.after = after;
        CHECK(Bc250MpcApplyLocked(&io,&plan,0,&attempted) == BC250_DISP_STATUS_DEVICE);
        CHECK(hw.writes == n && attempted == n && !hw.stalls);
        CHECK((hw.addr[n-1] != 0) == (after != 0));
    }
    io = Setup(); hw.lock = 0;
    CHECK(Bc250MpcApplyLocked(&io,&plan,0,&attempted) == BC250_DISP_STATUS_MISMATCH && !attempted);
    io = Setup(); hw.selector = 15;
    CHECK(Bc250MpcApplyLocked(&io,&plan,0,&attempted) == BC250_DISP_STATUS_MISMATCH && !attempted);
    r = Request(0,0); CHECK(Bc250MpcPrepare(&r,&plan)); io = Setup();
    CHECK(Bc250MpcApplyLocked(&io,&plan,0,&attempted) == BC250_DISP_STATUS_MISMATCH);
    CHECK(!hw.reads && !hw.writes && !attempted);
    CHECK(Bc250MpcApplyLocked(&io,&plan,1,&attempted) == 0 && attempted == 23);
    r = Request(3,0); r.Plane[1].ZOrder = 0;
    CHECK(!Bc250MpcPrepare(&r,&plan) && !memcmp(&plan,&zero,sizeof(plan)));
    r = Request(0,0); r.Plane[0].GlobalAlpha = 256;
    CHECK(!Bc250MpcPrepare(&r,&plan) && !memcmp(&plan,&zero,sizeof(plan)));
    CHECK(!Bc250MpcPrepare(NULL,&plan) && !memcmp(&plan,&zero,sizeof(plan)));
    CHECK(!Bc250MpcPrepare(&r,NULL));
    r = Request(3,0); CHECK(Bc250MpcPrepare(&r,&plan)); plan.Words[7] = 16; io = Setup();
    CHECK(Bc250MpcApplyLocked(&io,&plan,0,&attempted) == BC250_DISP_STATUS_INVALID && !hw.reads && !attempted);
}
static void Transitions(void)
{
    unsigned from, to, z, attempted;
    for (from = 0; from < 4; ++from) for (to = 0; to < 4; ++to) for (z = 0; z < 4; ++z) {
        BC250_MPC_REQUEST r = Request(from,z & 1);
        BC250_MPC_PLAN plan;
        BC250_DISP_IO io = Setup();
        CHECK(Bc250MpcPrepare(&r,&plan));
        CHECK(Bc250MpcApplyLocked(&io,&plan,1,&attempted) == 0);
        // Keep all register state across a topology change, including all-off.
        hw.reads = hw.writes = 0;
        r = Request(to,z >> 1);
        r.Plane[0].PerPixelAlpha = r.Plane[1].PerPixelAlpha = 1;
        CHECK(Bc250MpcPrepare(&r,&plan));
        CHECK(Bc250MpcApplyLocked(&io,&plan,1,&attempted) == 0);
        CHECK(hw.liveCount == 23 && hw.writes == 23 && attempted == 23);
        CHECK(Bc250DispGet(hw.liveValue[22],MPC_OUT0_MUX__MPC_OUT_MUX_MASK) == plan.Output);
        CHECK(hw.liveValue[7] == plan.Words[7] && hw.liveValue[18] == plan.Words[18]);
        CHECK(hw.liveValue[8] == plan.Words[8] && hw.liveValue[19] == plan.Words[19]);
        if (to == 1 || to == 2) {
            unsigned base = to == 1 ? 0 : BC250_MPC_WORDS_PER_PLANE;
            CHECK(FIELD(hw.liveValue[base],MPCC_MODE) == 2); // never bypass the remaining alpha
            CHECK(FIELD(hw.liveValue[base],MPCC_ALPHA_BLND_MODE) == 0);
        }
        Graph(&plan);
    }
}
int main(void)
{
    Combinations(); Faults(); Transitions();
    printf("MPC topology, alpha, black background and fault boundaries: %u checks, %u failures\n",checks,failures);
    return failures ? 1 : 0;
}
