#include <stdio.h>
#include <string.h>
#include "dcn_latch_sample.h"
#include "regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"
static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)
typedef struct {
    unsigned reads, fail, change, restore;
    unsigned long frame, lock, select;
    unsigned long long address;
    unsigned field;
} MODEL;
static const unsigned long held = OTG0_OTG_MASTER_UPDATE_LOCK__OTG_MASTER_UPDATE_LOCK_MASK |
    OTG0_OTG_MASTER_UPDATE_LOCK__UPDATE_LOCK_STATUS_MASK;
static void change(MODEL* m)
{
    switch (m->field) {
    case 0: m->frame ^= 1; break;
    case 1: m->lock ^= held; break;
    case 2: m->select ^= Bc250DispField(OTG0_OTG_GLOBAL_CONTROL0__OTG_MASTER_UPDATE_LOCK_SEL_MASK, 1); break;
    case 3: m->address ^= 0x100001000ull; break;
    default: m->address ^= 0x1000; break;
    }
}
static long read_word(void* context, unsigned long offset, unsigned long* value)
{
    MODEL* m = context;
    ++m->reads;
    if (m->reads == m->fail) { *value = 0xdeadbeef; return BC250_DISP_STATUS_DEVICE; }
    if (m->reads == m->change || m->reads == m->restore) change(m);
    switch (offset) {
    case BC250_REG_DMU_OTG0_OTG_STATUS_FRAME_COUNT: *value = m->frame; break;
    case BC250_REG_DMU_OTG0_OTG_GLOBAL_CONTROL0: *value = m->select; break;
    case BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK: *value = m->lock; break;
    case BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH: *value = (unsigned long)(m->address >> 32); break;
    case BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE: *value = (unsigned long)m->address; break;
    case BC250_REG_DMU_HUBPREQ0_DCSURF_FLIP_CONTROL:
        *value = HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK; break;
    default: CHECK(0); return BC250_DISP_STATUS_INVALID;
    }
    return 0;
}
int main(void)
{
    MODEL m;
    BC250_DISP_IO io;
    BC250_LATCH_SAMPLE s;
    unsigned i, field;
    long status;
    for (i = 0; i <= Bc250LatchWordCount; ++i) {
        memset(&m, 0, sizeof(m)); memset(&io, 0, sizeof(io)); memset(&s, 0xcc, sizeof(s));
        io.Context = &m; io.Read = read_word; m.address = 0x1234567800ull; m.lock = held; m.fail = i;
        status = Bc250LatchSampleRead(&io, &s);
        CHECK(status == (i ? BC250_DISP_STATUS_DEVICE : 0));
        CHECK(m.reads == (i ? i : Bc250LatchWordCount));
        CHECK(s.ValidMask == (i ? (1ul << (i - 1)) - 1 : (1ul << Bc250LatchWordCount) - 1));
        CHECK(io.Writes == 0 && io.StalledUs == 0);
        CHECK(s.BracketsMatch == !i && s.LockHeld == !i);
        CHECK(!s.LockClear);
        CHECK(s.Physical == (i ? 0 : m.address));
        if (!i) CHECK(s.Word[Bc250LatchFlipControl] == HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK);
        if (i) CHECK(s.Word[i - 1] == 0);
    }
    // Inject one transition at each point. Changes outside a field's bracket
    // are intentionally not reported as torn observations.
    for (field = 0; field < 5; ++field) for (i = 1; i <= Bc250LatchWordCount+1; ++i) {
        int mismatch = (field == 0 && i > 1 && i <= Bc250LatchFrameAfter+1) ||
            (field == 1 && i > 3 && i <= Bc250LatchLockAfter+1) ||
            (field == 2 && i <= Bc250LatchSelectAfter+1) || (field == 3 && i > 4 && i <= 6);
        memset(&m, 0, sizeof(m)); memset(&io, 0, sizeof(io));
        io.Context = &m; io.Read = read_word; m.address = 0x1234567800ull; m.lock = held;
        m.field = field; m.change = i;
        status = Bc250LatchSampleRead(&io, &s);
        CHECK(status == (mismatch ? BC250_DISP_STATUS_BUSY : 0));
        CHECK(m.reads == Bc250LatchWordCount && s.ValidMask == (1ul<<Bc250LatchWordCount)-1);
        CHECK(s.BracketsMatch == (unsigned)!mismatch);
        if (mismatch) CHECK(s.Physical == 0 && !s.LockHeld);
        CHECK(!io.Writes && !io.StalledUs);
    }
    // Counterexample: frame and lock can change and return inside the bracket.
    // Matching endpoints MUST NOT be advertised as continuity or retirement.
    for (field = 0; field < 4; ++field) {
        memset(&m, 0, sizeof(m)); memset(&io, 0, sizeof(io));
        io.Context = &m; io.Read = read_word; m.address = 0x1234567800ull; m.lock = held;
        m.field = field; m.change = 5; m.restore = 6;
        CHECK(Bc250LatchSampleRead(&io, &s) == 0 && s.BracketsMatch && s.LockHeld);
        if (field == 3) {
            CHECK(s.Physical != m.address);
            CHECK(s.Physical != (m.address ^ 0x100001000ull));
        }
    }
    for (i = 0; i < 4; ++i) {
        memset(&m, 0, sizeof(m)); memset(&io, 0, sizeof(io));
        io.Context = &m; io.Read = read_word;
        m.lock = (i & 1 ? OTG0_OTG_MASTER_UPDATE_LOCK__OTG_MASTER_UPDATE_LOCK_MASK : 0) |
            (i & 2 ? OTG0_OTG_MASTER_UPDATE_LOCK__UPDATE_LOCK_STATUS_MASK : 0);
        CHECK(Bc250LatchSampleRead(&io, &s) == 0);
        CHECK(s.LockHeld == (i == 3) && s.LockClear == (i == 0));
    }
    CHECK(!Bc250DispWriteAllowed(BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE));
    CHECK(!Bc250DispWriteAllowed(BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH));
    CHECK(Bc250LatchSampleRead(&io, NULL) == BC250_DISP_STATUS_INVALID);
    memset(&s, 0xcc, sizeof(s));
    CHECK(Bc250LatchSampleRead(NULL, &s) == BC250_DISP_STATUS_INVALID && !s.ValidMask && !s.Physical);
    io.Read = NULL;
    CHECK(Bc250LatchSampleRead(&io, &s) == BC250_DISP_STATUS_INVALID);
    printf("DCN latch sample: %u checks, %u failures; no hardware proof\n", checks, failures);
    return failures ? 1 : 0;
}
