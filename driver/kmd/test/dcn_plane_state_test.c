/* BD-223 stage S2: the plane state of dcn.c (DcnPlaneCheck, DcnPlaneApproved, DcnPlaneShapeAdmit, DcnPlaneShapeLocked,
   DcnPlaneShapeModeset), compiled from the driver's own source (generate_dcn_plane_state_test.py) against models of
   the register steps. dcn201_pipe_test.c and optc_test.c test those steps; this test holds the order of the steps,
   the save of the mode's registers at the first plane flip, the way back, the approvals and the mode stamp. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"
#include "dcn_plane.h"
#include "plane_format.h"
#include "../shim/bc250_optc.h"
#include "../shim/bc250_dcn201_pipe.h"

typedef unsigned long ULONG;
typedef long LONG;
typedef unsigned long long ULONGLONG;
typedef int32_t NTSTATUS;
typedef unsigned char BOOLEAN, UCHAR, KIRQL;
typedef uintptr_t ULONG_PTR, KSPIN_LOCK;
#define TRUE 1
#define FALSE 0
#define C_ASSERT_JOIN2(a, b) a##b
#define C_ASSERT_JOIN(a, b) C_ASSERT_JOIN2(a, b)
#define C_ASSERT(e) typedef char C_ASSERT_JOIN(c_assert_, __LINE__)[(e) ? 1 : -1]
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_DEVICE_NOT_READY ((NTSTATUS)0xC00000A3u)
#define STATUS_INVALID_DEVICE_STATE ((NTSTATUS)0xC0000184u)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xC000009Au)
#define STATUS_IO_TIMEOUT ((NTSTATUS)0xC00000B5u)
#define STATUS_DEVICE_DATA_ERROR ((NTSTATUS)0xC000009Cu)
#define NT_SUCCESS(s) ((NTSTATUS)(s) >= 0)
#define POOL_FLAG_NON_PAGED 0x40u
#define RtlZeroMemory(d, n) memset((d), 0, (n))
#define RtlCopyMemory(d, s, n) memcpy((d), (s), (n))
#define RtlEqualMemory(a, b, n) (memcmp((a), (b), (n)) == 0)
#define KeMemoryBarrier() ((void)0)

static unsigned g_checks, g_failures;
#define CHECK(x) do { ++g_checks; if (!(x)) { ++g_failures; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

static LONG InterlockedCompareExchange(volatile LONG* p, LONG x, LONG c) { LONG o = *p; if (o == c) *p = x; return o; }
static LONG InterlockedIncrement(volatile LONG* p) { return ++*p; }
static void KeInitializeSpinLock(KSPIN_LOCK* l) { *l = 0; }
static void KeAcquireSpinLock(KSPIN_LOCK* l, KIRQL* irql) { CHECK(*l == 0); *l = 1; *irql = 2; }
static void KeReleaseSpinLock(KSPIN_LOCK* l, KIRQL irql) { CHECK(*l == 1 && irql == 2); *l = 0; }
static void KeStallExecutionProcessor(ULONG us) { (void)us; }

/* EXTRACTED_TYPES */

typedef struct _BC250_DEVICE {
    void* Mmio;
    ULONG SourceWidth, SourceHeight;
    BOOLEAN DcnPlaneScaled;
    BC250_SCALER_PLAN DcnPlaneShape;
    BC250_SCALER_PLAN DcnPlaneNative;
    volatile LONG DcnPlaneShapeWrites, DcnPlaneShapeRestores, DcnPlaneShapeFailures;
    volatile LONG DcnPlaneModeStamp;
    ULONGLONG DcnPlaneState[BC250_DCN_PLANE_STATE_BYTES / sizeof(ULONGLONG)];
    volatile LONG DcnPlaneRequestorWrites, DcnPlaneRequestorRestores, DcnPlaneRequestorDropped;
    volatile LONG DcnLockTimeouts;
} BC250_DEVICE;

static ULONG DisplaySourceWidth(BC250_DEVICE* d) { return d->SourceWidth; }
static ULONG DisplaySourceHeight(BC250_DEVICE* d) { return d->SourceHeight; }

/* The model. events is the order of the register steps: L lock, U unlock, g sync read, S sync write, R requestor
   write, r requestor restore, C scaler write. requestor is the identity of the requestor on the pipe: the mode's
   (MODE_REQUESTOR) or a plan's DppClockKhz. */
#define MODE_REQUESTOR 1000u
static struct {
    char events[64];
    unsigned n;
    BC250_OPTC_SYNC otg;
    unsigned long requestor;
    BC250_SCALER_PLAN scaler;
    int lock, lockAck;
    long failSyncRead, failSyncWrite, failRequestor, failCapture, failRestore, failScaler, validate;
    int poolFail;
    unsigned pools;
    /* ConnectorLinkPlaneDml */
    NTSTATUS dmlStatus;
    int dmlResult;
    BC250_DML201_PLAN dml;
    ULONG budgetDisp, budgetDpp, refKhz, pitchPixels;
} m;

static void Event(char e) { if (m.n + 1 < sizeof(m.events)) { m.events[m.n++] = e; m.events[m.n] = 0; } }
static void Clear(void) { m.n = 0; m.events[0] = 0; }
static int Events(const char* expected)
{
    int same = strcmp(m.events, expected) == 0;
    if (!same) printf("  events \"%s\", expected \"%s\"\n", m.events, expected);
    m.n = 0; m.events[0] = 0;
    return same;
}

static void* ExAllocatePool2(unsigned flags, size_t bytes, ULONG tag)
{
    (void)tag;
    CHECK(flags == POOL_FLAG_NON_PAGED);
    if (m.poolFail) return NULL;
    m.pools++;
    return malloc(bytes);
}
static void ExFreePoolWithTag(void* p, ULONG tag) { (void)tag; m.pools--; free(p); }

static NTSTATUS MmioDcnWriteEx(BC250_DEVICE* d, ULONG reg, ULONG value, BOOLEAN quiet)
{
    (void)d; CHECK(quiet);
    CHECK(reg == BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK);
    m.lock = value != 0;
    Event(value ? 'L' : 'U');
    return STATUS_SUCCESS;
}
static NTSTATUS MmioDcnRead(BC250_DEVICE* d, ULONG reg, ULONG* value)
{
    (void)d;
    CHECK(reg == BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK);
    *value = (m.lock ? 1u : 0u) | (m.lock && m.lockAck ? OTG0_OTG_MASTER_UPDATE_LOCK__UPDATE_LOCK_STATUS_MASK : 0u);
    return STATUS_SUCCESS;
}
static long RuntimeRead(void* c, unsigned long o, unsigned long* v) { (void)c; (void)o; *v = 0; return -1; }
static long RuntimeWrite(void* c, unsigned long o, unsigned long v) { (void)c; (void)o; (void)v; return -1; }
static void RuntimeStall(void* c, unsigned long us) { (void)c; (void)us; }

long Bc250OptcReadGlobalSync(BC250_DISP_IO* io, BC250_OPTC_SYNC* s)
{
    CHECK(io && io->Read == RuntimeRead && io->Write == RuntimeWrite);
    Event('g');
    if (m.failSyncRead) { memset(s, 0, sizeof(*s)); return m.failSyncRead; }
    *s = m.otg;
    return 0;
}
long Bc250OptcGlobalSyncLocked(BC250_DISP_IO* io, const BC250_OPTC_SYNC* s)
{
    (void)io;
    if (s->VStartup != m.otg.VStartup || s->VUpdateOffset != m.otg.VUpdateOffset) return BC250_DISP_STATUS_NOT_SUPPORTED;
    if (m.failSyncWrite) return m.failSyncWrite;
    Event('S');
    m.otg = *s;
    return 0;
}
long Bc250Dcn201RequestorValidate(const BC250_DML201_PLAN* p, unsigned int ref) { (void)p; (void)ref; return m.validate; }
long Bc250Dcn201RequestorLocked(BC250_DISP_IO* io, const BC250_DML201_PLAN* p, unsigned int ref,
                                BC250_DCN201_REQUESTOR_SAVE* save)
{
    (void)io;
    CHECK(ref == 50000u);
    if (!save->Count) {
        if (m.failCapture) return m.failCapture;
        save->Count = 1; save->Register[0] = 0x1000u; save->Mask[0] = ~0u; save->Value[0] = (unsigned)m.requestor;
    }
    Event('R');
    if (m.failRequestor) { m.requestor = 0; return m.failRequestor; }
    m.requestor = p->DppClockKhz;
    return 0;
}
long Bc250Dcn201RequestorRestoreLocked(BC250_DISP_IO* io, const BC250_DCN201_REQUESTOR_SAVE* s)
{
    (void)io;
    CHECK(s->Count == 1);
    Event('r');
    if (m.failRestore) return m.failRestore;
    m.requestor = s->Value[0];
    return 0;
}
static long ModelScalerProgramLocked(BC250_DISP_IO* io, const BC250_SCALER_PLAN* plan, BC250_SCALER_RESULT* result)
{
    (void)io;
    memset(result, 0, sizeof(*result));
    Event('C');
    if (m.failScaler) return m.failScaler;
    m.scaler = *plan;
    return 0;
}
#define Bc250ScalerProgramLocked ModelScalerProgramLocked
static NTSTATUS ConnectorLinkPlaneDml(BC250_DEVICE* d, const BC250_LINK_PLANE* plane, BC250_LINK_PLANE_DML* out,
                                      struct BC250_DML201_PLAN* plan)
{
    (void)d;
    memset(out, 0, sizeof(*out)); memset(plan, 0, sizeof(*plan));
    m.pitchPixels = plane->PitchPixels;
    if (m.dmlStatus < 0) return m.dmlStatus;
    *plan = m.dml;
    out->Result = m.dmlResult; out->Valid = plan->Valid;
    out->DispClockKhz = plan->DispClockKhz; out->DppClockKhz = plan->DppClockKhz;
    out->BudgetDispKhz = m.budgetDisp; out->BudgetDppKhz = m.budgetDpp; out->DchubRefClockKhz = m.refKhz;
    return STATUS_SUCCESS;
}

/* ACTUAL_SOURCE */

static BC250_DEVICE dev;
static const BC250_OPTC_SYNC modeSync = { 340, 22, 520, 255 };

static void Fresh(void)
{
    memset(&m, 0, sizeof(m));
    memset(&dev, 0, sizeof(dev));
    dev.Mmio = &m; dev.SourceWidth = 1920; dev.SourceHeight = 1200;
    DcnPlaneStateReset(&dev);
    m.otg = modeSync; m.requestor = MODE_REQUESTOR; m.lockAck = 1;
    m.budgetDisp = 400000; m.budgetDpp = 400000; m.refKhz = 50000;
    m.dml.Valid = 1; m.dml.VStartup = 22; m.dml.VUpdateOffset = 520; m.dml.VReady = 369; m.dml.VUpdateWidth = 275;
    m.dml.DispClockKhz = 154462; m.dml.DppClockKhz = 154462;
}

/* A plane of Width x Height scaled into the 1920x1200 mode at y Top, Pitch bytes per row. */
static BC250_PLANE_PLAN Plane(unsigned long w, unsigned long h, unsigned long dstW, unsigned long dstH, long top,
                              unsigned long pitch)
{
    BC250_PLANE_REQUEST r;
    BC250_PLANE_PLAN p;
    memset(&r, 0, sizeof(r));
    r.PlaneCount = 1; r.Enabled = 1; r.SurfaceWidth = w; r.SurfaceHeight = h; r.SurfacePitch = pitch;
    r.RequiredPitch = 1920 * 4; r.PlaneFormat = BC250_PLANE_FORMAT_ARGB8888;
    r.SourceModeWidth = r.TargetWidth = 1920; r.SourceModeHeight = r.TargetHeight = 1200;
    r.Src.Right = (long)w; r.Src.Bottom = (long)h;
    r.Dst.Top = top; r.Dst.Right = (long)dstW; r.Dst.Bottom = top + (long)dstH; r.Clip = r.Dst;
    r.StretchQualityKnown = 1;
    CHECK(Bc250PlanePrepare(&r, &p) == BC250_PLANE_OK);
    return p;
}

static ULONG Check(const BC250_PLANE_PLAN* p, ULONG pitch, unsigned dpp)
{
    BC250_DCN_PLANE_CHECK out;
    ULONG verdict;
    m.dml.DppClockKhz = dpp;
    verdict = DcnPlaneCheck(&dev, p, pitch, 4, &out);
    CHECK(m.pools == 0);
    return verdict;
}

/* One flip of the transaction: the admission and, when it admits, the locked step. */
static NTSTATUS Flip(const BC250_SCALER_PLAN* shape, ULONG pitch)
{
    if (shape && !DcnPlaneShapeAdmit(&dev, shape, pitch)) return -1;
    return DcnPlaneShapeLocked(&dev, shape, pitch);
}

static void CheckAndApprovals(void)
{
    BC250_PLANE_PLAN a = Plane(1280, 720, 1920, 1080, 60, 5120), id;
    BC250_DCN_PLANE_CHECK out;
    int i;
    Fresh();
    CHECK(Check(&a, 5120, 2000) == BC250_PLANE_OK);
    CHECK(m.pitchPixels == 1280 && Events("g"));
    CHECK(DcnPlaneApproved(&dev, &a.Scaler, 5120) && !DcnPlaneApproved(&dev, &a.Scaler, 6144));
    /* The verdict and the log fields. */
    m.dml.DppClockKhz = 2000;
    CHECK(DcnPlaneCheck(&dev, &a, 5120, 4, &out) == BC250_PLANE_OK && Events("g"));
    CHECK(out.Status == STATUS_SUCCESS && out.VStartup == 22 && out.OtgVStartup == 22 && out.VReady == 369 &&
          out.VUpdateOffset == 520 && out.OtgVUpdateOffset == 520 && out.BudgetDppKhz == 400000);
    /* Refusals approve nothing. */
    Fresh(); m.dmlStatus = STATUS_DEVICE_NOT_READY; CHECK(Check(&a, 5120, 2000) == BC250_PLANE_DML && Events(""));
    Fresh(); m.dmlResult = -2; CHECK(Check(&a, 5120, 2000) == BC250_PLANE_DML);
    Fresh(); m.dml.Valid = 0; CHECK(Check(&a, 5120, 2000) == BC250_PLANE_DML);
    Fresh(); CHECK(Check(&a, 5120, 400001) == BC250_PLANE_CLOCKS && Events(""));
    Fresh(); m.dml.VStartup = 23; CHECK(Check(&a, 5120, 2000) == BC250_PLANE_GLOBAL_SYNC && Events("g"));
    Fresh(); m.dml.VUpdateOffset = 519; CHECK(Check(&a, 5120, 2000) == BC250_PLANE_GLOBAL_SYNC);
    Fresh(); m.failSyncRead = BC250_DISP_STATUS_DEVICE; CHECK(Check(&a, 5120, 2000) == BC250_PLANE_GLOBAL_SYNC);
    Fresh(); dev.Mmio = NULL; CHECK(Check(&a, 5120, 2000) == BC250_PLANE_GLOBAL_SYNC && Events(""));
    Fresh(); m.validate = BC250_DISP_STATUS_INVALID; CHECK(Check(&a, 5120, 2000) == BC250_PLANE_DML);
    Fresh(); m.poolFail = 1; CHECK(Check(&a, 5120, 2000) == BC250_PLANE_DML && Events(""));
    Fresh(); CHECK(Check(&a, 5121, 2000) == BC250_PLANE_ARGS && Events(""));
    CHECK(!DcnPlaneApproved(&dev, &a.Scaler, 5120));
    /* The identity plane is the committed pipe: no DML, no approval. */
    Fresh(); id = Plane(1920, 1200, 1920, 1200, 0, 7680); CHECK(id.Identity);
    CHECK(Check(&id, 7680, 2000) == BC250_PLANE_OK && Events("") && !DcnPlaneApproved(&dev, &id.Scaler, 7680));
    /* A mode program makes every approval stale. */
    Fresh(); CHECK(Check(&a, 5120, 2000) == BC250_PLANE_OK);
    DcnPlaneShapeModeset(&dev, TRUE);
    CHECK(!DcnPlaneApproved(&dev, &a.Scaler, 5120) && !DcnPlaneShapeAdmit(&dev, &a.Scaler, 5120));
    /* Four entries; a fifth plane replaces the oldest, the same plane again replaces nothing. */
    Fresh();
    CHECK(Check(&a, 5120, 2000) == BC250_PLANE_OK && Check(&a, 5184, 2001) == BC250_PLANE_OK &&
          Check(&a, 5248, 2002) == BC250_PLANE_OK && Check(&a, 5312, 2003) == BC250_PLANE_OK);
    CHECK(Check(&a, 5120, 2004) == BC250_PLANE_OK);
    CHECK(DcnPlaneApproved(&dev, &a.Scaler, 5184));
    CHECK(Check(&a, 5376, 2005) == BC250_PLANE_OK);
    CHECK(!DcnPlaneApproved(&dev, &a.Scaler, 5120) && DcnPlaneApproved(&dev, &a.Scaler, 5184) &&
          DcnPlaneApproved(&dev, &a.Scaler, 5376));
    /* An entry that Check writes now (odd sequence) does not count. */
    {
        DCN_PLANE_STATE* s = DcnPlaneStateOf(&dev);
        for (i = 0; i < (int)DCN_PLANE_APPROVALS; i++)
            if (s->Approved[i].Pitch == 5376) s->Approved[i].Sequence++;
        CHECK(!DcnPlaneApproved(&dev, &a.Scaler, 5376));
    }
    Clear();
}

static void FlipsAndWayBack(void)
{
    BC250_PLANE_PLAN a = Plane(1280, 720, 1920, 1080, 60, 5120), b = Plane(1600, 900, 1920, 1080, 60, 6400);
    BC250_SCALER_PLAN other;
    Fresh();
    CHECK(Check(&a, 5120, 2000) == BC250_PLANE_OK && Check(&a, 6144, 2001) == BC250_PLANE_OK &&
          Check(&b, 6400, 3000) == BC250_PLANE_OK);
    Clear();
    /* The first plane flip: the mode's sync read and saved, the plane's sync, its requestor, its scaler. */
    CHECK(Flip(&a.Scaler, 5120) == STATUS_SUCCESS && Events("gSRC"));
    CHECK(m.requestor == 2000 && m.otg.VReady == 369 && m.otg.VUpdateWidth == 275 && m.otg.VStartup == 22);
    CHECK(memcmp(&m.scaler, &a.Scaler, sizeof(a.Scaler)) == 0);
    CHECK(DcnPlaneShapeIs(&dev, &a.Scaler) && !DcnPlaneShapeIs(&dev, NULL));
    CHECK(dev.DcnPlaneRequestorWrites == 1 && dev.DcnPlaneShapeWrites == 1);
    /* The same plane again writes nothing. */
    CHECK(Flip(&a.Scaler, 5120) == STATUS_SUCCESS && Events(""));
    /* The same shape at another pitch: its own sync and requestor, not the scaler. */
    CHECK(Flip(&a.Scaler, 6144) == STATUS_SUCCESS && Events("SR") && m.requestor == 2001);
    /* Another plane: all three again, and the mode's registers stay saved. */
    CHECK(Flip(&b.Scaler, 6400) == STATUS_SUCCESS && Events("SRC") && m.requestor == 3000);
    /* The full-size flip: the mode's sync, its requestor, then the 1:1 shape. A second one writes nothing. */
    CHECK(Flip(NULL, 7680) == STATUS_SUCCESS && Events("SrC"));
    CHECK(m.requestor == MODE_REQUESTOR && memcmp(&m.otg, &modeSync, sizeof(modeSync)) == 0);
    CHECK(m.scaler.Scaling == BC250_SCALING_IDENTITY && m.scaler.SourceWidth == 1920 && m.scaler.SourceHeight == 1200);
    CHECK(DcnPlaneShapeIs(&dev, NULL) && dev.DcnPlaneRequestorRestores == 1 && dev.DcnPlaneShapeRestores == 1);
    CHECK(Flip(NULL, 7680) == STATUS_SUCCESS && Events(""));
    /* The admission: no approval, another mode size, not a plane's scaling. Nothing is written. */
    CHECK(Flip(&a.Scaler, 4096) == -1 && Events(""));
    other = a.Scaler; other.TargetHeight = 1080; CHECK(Flip(&other, 5120) == -1);
    other = a.Scaler; other.Scaling = BC250_SCALING_IDENTITY; CHECK(Flip(&other, 5120) == -1);
    CHECK(Events(""));
    /* The locked step without the admission of the same transaction is refused before any write. */
    CHECK(DcnPlaneShapeLocked(&dev, &a.Scaler, 5120) == STATUS_INVALID_DEVICE_STATE && Events(""));
    CHECK(DcnPlaneShapeAdmit(&dev, &a.Scaler, 5120));
    CHECK(DcnPlaneShapeLocked(&dev, &a.Scaler, 6144) == STATUS_INVALID_DEVICE_STATE && Events(""));
    CHECK(DcnPlaneShapeLocked(&dev, &a.Scaler, 5120) == STATUS_INVALID_DEVICE_STATE && Events(""));
    /* A plane on the pipe stays admitted when Check has replaced its approval since. */
    CHECK(Flip(&a.Scaler, 5120) == STATUS_SUCCESS && Events("gSRC"));
    {
        unsigned i;
        for (i = 0; i < DCN_PLANE_APPROVALS; i++) DcnPlaneStateOf(&dev)->Approved[i].Sequence = 0;
    }
    CHECK(DcnPlaneApproved(&dev, &a.Scaler, 5120) && Flip(&a.Scaler, 5120) == STATUS_SUCCESS && Events(""));
    CHECK(Flip(NULL, 7680) == STATUS_SUCCESS && Events("SrC") && m.requestor == MODE_REQUESTOR);
}

static void Failures(void)
{
    BC250_PLANE_PLAN a = Plane(1280, 720, 1920, 1080, 60, 5120);
    /* The requestor write fails after the capture: unknown, so neither the plane's nor the mode's pipe, and the
       full-size flip writes the mode's registers back. */
    Fresh(); CHECK(Check(&a, 5120, 2000) == BC250_PLANE_OK); Clear();
    m.failRequestor = BC250_DISP_STATUS_DEVICE;
    CHECK(Flip(&a.Scaler, 5120) == BC250_DISP_STATUS_DEVICE && Events("gSR"));
    CHECK(!DcnPlaneShapeIs(&dev, &a.Scaler) && !DcnPlaneShapeIs(&dev, NULL) && dev.DcnPlaneShapeFailures == 1);
    m.failRequestor = 0;
    CHECK(Flip(NULL, 7680) == STATUS_SUCCESS && Events("Sr"));
    CHECK(m.requestor == MODE_REQUESTOR && memcmp(&m.otg, &modeSync, sizeof(modeSync)) == 0 && DcnPlaneShapeIs(&dev, NULL));
    /* The capture fails: nothing written, nothing to restore but the sync. */
    Fresh(); CHECK(Check(&a, 5120, 2000) == BC250_PLANE_OK); Clear();
    m.failCapture = BC250_DISP_STATUS_DEVICE;
    CHECK(Flip(&a.Scaler, 5120) == BC250_DISP_STATUS_DEVICE && Events("gS") && m.requestor == MODE_REQUESTOR);
    m.failCapture = 0;
    CHECK(Flip(NULL, 7680) == STATUS_SUCCESS && Events("S") && DcnPlaneShapeIs(&dev, NULL));
    /* The OTG has another VSTARTUP than the plan now: the sync step refuses before any write, and no requestor
       follows. The admission of the next transaction is needed again. */
    Fresh(); CHECK(Check(&a, 5120, 2000) == BC250_PLANE_OK); Clear();
    m.otg.VStartup = 23;
    CHECK(Flip(&a.Scaler, 5120) == BC250_DISP_STATUS_NOT_SUPPORTED && Events("g") && m.requestor == MODE_REQUESTOR);
    CHECK(DcnPlaneShapeLocked(&dev, &a.Scaler, 5120) == STATUS_INVALID_DEVICE_STATE && Events(""));
    /* The sync read fails: nothing written, nothing saved. */
    Fresh(); CHECK(Check(&a, 5120, 2000) == BC250_PLANE_OK); Clear();
    m.failSyncRead = BC250_DISP_STATUS_DEVICE;
    CHECK(Flip(&a.Scaler, 5120) == BC250_DISP_STATUS_DEVICE && Events("g") && DcnPlaneShapeIs(&dev, NULL));
    /* The scaler write fails: the pipe counts as scaled with no known shape; the full-size flip restores all. */
    Fresh(); CHECK(Check(&a, 5120, 2000) == BC250_PLANE_OK); Clear();
    m.failScaler = BC250_DISP_STATUS_DEVICE;
    CHECK(Flip(&a.Scaler, 5120) == BC250_DISP_STATUS_DEVICE && Events("gSRC") && !DcnPlaneShapeIs(&dev, &a.Scaler));
    m.failScaler = 0;
    CHECK(Flip(NULL, 7680) == STATUS_SUCCESS && Events("SrC") && DcnPlaneShapeIs(&dev, NULL));
    /* The restore fails: the rest stays for the next full-size flip. */
    Fresh(); CHECK(Check(&a, 5120, 2000) == BC250_PLANE_OK); Clear();
    CHECK(Flip(&a.Scaler, 5120) == STATUS_SUCCESS && Events("gSRC"));
    m.failRestore = BC250_DISP_STATUS_DEVICE;
    CHECK(Flip(NULL, 7680) == BC250_DISP_STATUS_DEVICE && Events("Sr") && !DcnPlaneShapeIs(&dev, NULL));
    m.failRestore = 0;
    CHECK(Flip(NULL, 7680) == STATUS_SUCCESS && Events("rC") && DcnPlaneShapeIs(&dev, NULL));
}

static void ModePrograms(void)
{
    BC250_PLANE_PLAN a = Plane(1280, 720, 1920, 1080, 60, 5120);
    LONG stamp;
    /* modeset.c writes only the scaler: the mode's sync and requestor go back here, in their own lock. */
    Fresh(); CHECK(Check(&a, 5120, 2000) == BC250_PLANE_OK); Clear();
    CHECK(Flip(&a.Scaler, 5120) == STATUS_SUCCESS && Events("gSRC"));
    stamp = dev.DcnPlaneModeStamp;
    DcnPlaneShapeModeset(&dev, FALSE);
    CHECK(Events("LSrU") && m.requestor == MODE_REQUESTOR && memcmp(&m.otg, &modeSync, sizeof(modeSync)) == 0);
    CHECK(dev.DcnPlaneModeStamp == stamp + 1 && dev.DcnPlaneRequestorDropped == 0 && DcnPlaneShapeIs(&dev, NULL));
    CHECK(!m.lock);
    /* No plane: nothing written. */
    DcnPlaneShapeModeset(&dev, FALSE); CHECK(Events("") && dev.DcnPlaneRequestorDropped == 0);
    /* The lock is not acknowledged: no restore, the lock released, and the plane counts as dropped. */
    Fresh(); CHECK(Check(&a, 5120, 2000) == BC250_PLANE_OK); Clear();
    CHECK(Flip(&a.Scaler, 5120) == STATUS_SUCCESS && Events("gSRC"));
    m.lockAck = 0;
    DcnPlaneShapeModeset(&dev, FALSE);
    CHECK(Events("LU") && !m.lock && dev.DcnLockTimeouts == 1 && dev.DcnPlaneRequestorDropped == 1);
    CHECK(DcnPlaneShapeIs(&dev, NULL));
    /* connector_link.c programs the whole pipe: nothing written here, the saved registers are old. */
    Fresh(); CHECK(Check(&a, 5120, 2000) == BC250_PLANE_OK); Clear();
    CHECK(Flip(&a.Scaler, 5120) == STATUS_SUCCESS && Events("gSRC"));
    DcnPlaneShapeModeset(&dev, TRUE);
    CHECK(Events("") && dev.DcnPlaneRequestorDropped == 1 && DcnPlaneShapeIs(&dev, NULL));
    CHECK(Flip(NULL, 7680) == STATUS_SUCCESS && Events(""));
}

int main(void)
{
    CheckAndApprovals();
    FlipsAndWayBack();
    Failures();
    ModePrograms();
    printf("dcn_plane_state: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
