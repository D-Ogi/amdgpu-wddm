/* Host test for driver\kmd\dcn_cursor.c (docs/design/hardware-pointer.md): the life of the hardware pointer around
 * the shape conversion that dcn_cursor_test.c tests. The production body is compiled unchanged against a host model
 * of the kernel calls, the registers and the connector: the prepare, the caps, a shape before any pipe, on a ready
 * pipe and in the gap of a mode change, the two surfaces and the latch wait, the pipe going and coming back with the
 * cursor TTU, a failed register write (the update lock is released), the stop, the quiet off and the release.
 * run_dcn_cursor_life.ps1 writes dcn_cursor_actual.inc from dcn_cursor.c and, with -Mutation all, builds each
 * negative control, which must fail. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef long NTSTATUS;
typedef unsigned long ULONG;
typedef long LONG;
typedef unsigned int UINT;
typedef int INT;
typedef unsigned long long ULONGLONG;
typedef long long LONGLONG;
typedef unsigned char BOOLEAN;
typedef unsigned char UCHAR;
typedef UCHAR* PUCHAR;
typedef size_t SIZE_T;
typedef UCHAR KIRQL;
typedef struct { int Held; } KSPIN_LOCK;
typedef struct { int Held; } FAST_MUTEX;
typedef struct { LONGLONG QuadPart; } PHYSICAL_ADDRESS;
typedef struct { LONGLONG QuadPart; } LARGE_INTEGER;
#define TRUE 1
#define FALSE 0
#include <sal.h>
#define NT_SUCCESS(s) ((NTSTATUS)(s) >= 0)
#define UNREFERENCED_PARAMETER(p) ((void)(p))
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_NOT_SUPPORTED ((NTSTATUS)0xC00000BBL)
#define STATUS_DEVICE_NOT_READY ((NTSTATUS)0xC00000A3L)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000DL)
#define STATUS_NO_MEMORY ((NTSTATUS)0xC0000017L)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xC000009AL)
#define STATUS_IO_DEVICE_ERROR ((NTSTATUS)0xC0000185L)
#define MAXULONGLONG UINT64_MAX
#define MAXLONGLONG INT64_MAX
#define APC_LEVEL 1
#define DISPATCH_LEVEL 2
#define KernelMode 0
#define POOL_FLAG_NON_PAGED 0x40ull
#define PAGE_READWRITE 4
#define BC250_VRAM_TOP_RESERVED (32ull << 20)

#include "regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"
#include "display_io.h"
#include "display_rotation.h"
#include "dcn_scale.h"
#include "dcn_cursor_shape.h"

typedef struct { UINT Value; } DXGK_POINTERFLAGS;
typedef struct { ULONG VidPnSourceId; DXGK_POINTERFLAGS Flags; UINT Width, Height, Pitch; const void* pPixels; UINT XHot, YHot; }
    DXGKARG_SETPOINTERSHAPE;
typedef struct { UINT Visible; } DXGK_SETPOINTERPOSITIONFLAGS;
typedef struct { ULONG VidPnSourceId; INT X, Y; DXGK_SETPOINTERPOSITIONFLAGS Flags; } DXGKARG_SETPOINTERPOSITION;

typedef struct _BC250_DEVICE {
    void* Cursor;
    void* Mmio;
    BOOLEAN FullWddm, DcnWriteEnabled, VidPnFlipEnabled, DcnPlaneScaled;
    void* RuntimeFramebuffer;
    ULONGLONG DisplayReservationEnd, VramLength;
    PHYSICAL_ADDRESS VramPhysical;
    volatile LONG DcnSurfaceSequence;
    BC250_SCALER_PLAN DcnPlaneShape;
    ULONG Rotation;
    struct { BOOLEAN PipeChanged; BC250_SCALER_PLAN Plan; } Modeset;
} BC250_DEVICE;

static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

/* ---- the model ---------------------------------------------------------------------------------------------- */
static ULONG regs[0x40000 / 4];
static unsigned writes, failAt, quiet, connectorEnabled, delays, maps, unmaps, logs;
static ULONG settingModel, fullTable;
static int level, spinHeld, mutexHeld;
static ULONGLONG nowTime;
static int pending;                 /* CUR0_UPDATE_PENDING: set by a surface address write, cleared by Latch() */
static unsigned char vram[2 * BC250_CURSOR_BUFFER_BYTES];

static ULONG Reg(ULONG offset) { return regs[offset / 4]; }
static ULONG Get(ULONG offset, ULONG mask)
{
    ULONG v = Reg(offset) & mask;
    while (mask && !(mask & 1u)) { mask >>= 1; v >>= 1; }
    return v;
}
static int CursorRegister(ULONG offset)
{
    return offset == BC250_REG_DMU_CURSOR0_0_CURSOR_CONTROL || offset == BC250_REG_DMU_CURSOR0_0_CURSOR_POSITION ||
        offset == BC250_REG_DMU_CURSOR0_0_CURSOR_HOT_SPOT || offset == BC250_REG_DMU_CURSOR0_0_CURSOR_DST_OFFSET ||
        offset == BC250_REG_DMU_CURSOR0_0_CURSOR_SIZE || offset == BC250_REG_DMU_CURSOR0_0_CURSOR_SURFACE_ADDRESS ||
        offset == BC250_REG_DMU_CURSOR0_0_CURSOR_SURFACE_ADDRESS_HIGH || offset == BC250_REG_DMU_CNVC_CUR0_CURSOR0_CONTROL ||
        offset == BC250_REG_DMU_CNVC_CUR0_CURSOR0_FP_SCALE_BIAS || offset == BC250_REG_DMU_HUBPREQ0_CURSOR_SETTINGS;
}
static NTSTATUS MmioDcnRead(const BC250_DEVICE* d, ULONG offset, ULONG* value)
{
    (void)d;
    *value = Reg(offset);
    if (offset == BC250_REG_DMU_CNVC_CUR0_CURSOR0_CONTROL && pending) *value |= CNVC_CUR0_CURSOR0_CONTROL__CUR0_UPDATE_PENDING_MASK;
    return STATUS_SUCCESS;
}
static NTSTATUS MmioDcnWriteEx(const BC250_DEVICE* d, ULONG offset, ULONG value, BOOLEAN log)
{
    (void)d; (void)log;
    ++writes;
    if (failAt && writes == failAt) return STATUS_IO_DEVICE_ERROR;
    /* Every write of this file is under Lock, except the quiet off of the bugcheck path. Every cursor register is
     * written inside the cursor's update lock, so that one VUPDATE latches a whole batch. */
    if (!quiet) CHECK(spinHeld);
    if (!quiet && CursorRegister(offset)) CHECK(Get(BC250_REG_DMU_CUR_VUPDATE_LOCK_SET0, CUR_VUPDATE_LOCK_SET0__CUR_VUPDATE_LOCK_SET_MASK));
    if (offset == BC250_REG_DMU_CNVC_CUR0_CURSOR0_CONTROL) value &= ~(ULONG)CNVC_CUR0_CURSOR0_CONTROL__CUR0_UPDATE_PENDING_MASK;
    if (offset == BC250_REG_DMU_CURSOR0_0_CURSOR_SURFACE_ADDRESS) pending = 1;
    regs[offset / 4] = value;
    return STATUS_SUCCESS;
}
static void Latch(void) { pending = 0; }
static LONG InterlockedIncrement(volatile LONG* p) { return ++*p; }
static LONG InterlockedCompareExchange(volatile LONG* p, LONG x, LONG c) { LONG o = *p; if (o == c) *p = x; return o; }
static void KeInitializeSpinLock(KSPIN_LOCK* l) { l->Held = 0; }
static void KeAcquireSpinLock(KSPIN_LOCK* l, KIRQL* old)
{ CHECK(!spinHeld && !l->Held); l->Held = spinHeld = 1; *old = (KIRQL)level; level = DISPATCH_LEVEL; }
static void KeReleaseSpinLock(KSPIN_LOCK* l, KIRQL old) { CHECK(l->Held); l->Held = spinHeld = 0; level = old; }
static void ExInitializeFastMutex(FAST_MUTEX* m) { m->Held = 0; }
/* The lock order of dcn_cursor.c: ShapeLock, then Lock, never the reverse. */
static void ExAcquireFastMutex(FAST_MUTEX* m) { CHECK(!spinHeld && !mutexHeld && level == 0); m->Held = mutexHeld = 1; level = APC_LEVEL; }
static void ExReleaseFastMutex(FAST_MUTEX* m) { CHECK(m->Held && !spinHeld); m->Held = mutexHeld = 0; level = 0; }
static ULONGLONG KeQueryInterruptTime(void) { return nowTime; }
static NTSTATUS KeDelayExecutionThread(int mode, BOOLEAN alertable, LARGE_INTEGER* t)
{ (void)mode; (void)alertable; CHECK(!spinHeld && t->QuadPart < 0); ++delays; nowTime += (ULONGLONG)-t->QuadPart; return 0; }
static void KeMemoryBarrier(void) {}
static int KeGetCurrentIrql(void) { return level; }
static void* ExAllocatePool2(ULONGLONG flags, SIZE_T bytes, ULONG tag) { (void)flags; (void)tag; return calloc(1, bytes); }
static void ExFreePoolWithTag(void* p, ULONG tag) { (void)tag; free(p); }
static void RtlZeroMemory(void* p, SIZE_T n) { memset(p, 0, n); }
static void RtlCopyMemory(void* d, const void* s, SIZE_T n) { memcpy(d, s, n); }
static void* VramMapCpuRange(BC250_DEVICE* d, PHYSICAL_ADDRESS p, SIZE_T n, ULONG protection)
{ (void)d; (void)p; CHECK(n == sizeof(vram) && protection == PAGE_READWRITE); ++maps; return vram; }
static void MmUnmapIoSpace(void* p, SIZE_T n) { CHECK(p == vram && n == sizeof(vram)); ++unmaps; }
static ULONG DisplaySourceWidth(const BC250_DEVICE* d) { (void)d; return 1920; }
static ULONG DisplaySourceHeight(const BC250_DEVICE* d) { (void)d; return 1080; }
static void GuardLog(const char* format, ...) { (void)format; ++logs; }
static ULONG GuardHardwarePointerSetting(void) { return settingModel; }
static BOOLEAN WddmFullTableSelected(void) { return (BOOLEAN)fullTable; }
static BOOLEAN ConnectorIsEnabled(BC250_DEVICE* d) { (void)d; return (BOOLEAN)connectorEnabled; }

void DcnCursorRelease(BC250_DEVICE* Device);
#include "dcn_cursor_actual.inc"

/* ---- the scenarios ------------------------------------------------------------------------------------------- */
#define VRAM_BASE 0x8000000000ll
static BC250_DEVICE device;
static unsigned char pixels[BC250_CURSOR_RAW_MAX_BYTES];
static int mmioToken;

static void Fresh(ULONG setting)
{
    if (device.Cursor) DcnCursorRelease(&device);
    memset(&device, 0, sizeof(device));
    memset(regs, 0, sizeof(regs));
    memset(vram, 0xAA, sizeof(vram));
    device.Mmio = &mmioToken; device.FullWddm = device.DcnWriteEnabled = device.VidPnFlipEnabled = 1;
    device.RuntimeFramebuffer = &mmioToken; device.DisplayReservationEnd = 0x01234567ull;
    device.VramLength = 8ull << 30; device.VramPhysical.QuadPart = VRAM_BASE; device.Rotation = BC250_VPPR_IDENTITY;
    settingModel = setting; fullTable = 1; connectorEnabled = 1;
    writes = failAt = quiet = delays = maps = unmaps = 0; pending = 0; level = 0; spinHeld = mutexHeld = 0; nowTime = 1000000;
}
static BC250_CURSOR_PIPE Pipe(unsigned width, int ttu)
{
    BC250_CURSOR_PIPE p;
    memset(&p, 0, sizeof(p));
    p.CursorWidth = width; p.RefClockKhz = 100000; p.PixelClockKhz = 148500; p.WriteTtu = ttu;
    p.Delivery = 77; p.DeliveryPre = 66; p.QosLevelFixed = 8; p.QosRampDisable = 1;
    return p;
}
static void Ready(void) { BC250_CURSOR_PIPE p = Pipe(256, 0); DcnCursorPipeReady(&device, &p); }
static NTSTATUS SetShape(unsigned flags, unsigned w, unsigned h, unsigned xhot, unsigned yhot)
{
    DXGKARG_SETPOINTERSHAPE a;
    memset(&a, 0, sizeof(a));
    a.Flags.Value = flags; a.Width = w; a.Height = h; a.Pitch = flags == BC250_POINTER_MONOCHROME ? (w + 7) / 8 : w * 4;
    a.XHot = xhot; a.YHot = yhot; a.pPixels = pixels;
    return DcnCursorSetShape(&device, &a);
}
static void Move(int x, int y, int visible)
{
    DXGKARG_SETPOINTERPOSITION a;
    memset(&a, 0, sizeof(a));
    a.X = x; a.Y = y; a.Flags.Visible = (UINT)visible;
    CHECK(DcnCursorSetPosition(&device, &a) == STATUS_SUCCESS);
}
static ULONG Enable(void)
{
    ULONG hubp = Get(BC250_REG_DMU_CURSOR0_0_CURSOR_CONTROL, CURSOR0_0_CURSOR_CONTROL__CURSOR_ENABLE_MASK);
    ULONG cnvc = Get(BC250_REG_DMU_CNVC_CUR0_CURSOR0_CONTROL, CNVC_CUR0_CURSOR0_CONTROL__CUR0_ENABLE_MASK);
    CHECK(hubp == cnvc);        /* HUBP and DPP always agree */
    return hubp;
}
static ULONGLONG Address(void)
{
    return ((ULONGLONG)Reg(BC250_REG_DMU_CURSOR0_0_CURSOR_SURFACE_ADDRESS_HIGH) << 32) |
        Reg(BC250_REG_DMU_CURSOR0_0_CURSOR_SURFACE_ADDRESS);
}
static ULONG Unlocked(void) { return !Get(BC250_REG_DMU_CUR_VUPDATE_LOCK_SET0, CUR_VUPDATE_LOCK_SET0__CUR_VUPDATE_LOCK_SET_MASK); }
static ULONGLONG CursorBase(void) { return ((ULONGLONG)VRAM_BASE + ((0x01234567ull + 65535ull) & ~65535ull)); }

static void Prepare(void)
{
    Fresh(0); CHECK(DcnCursorPrepare(&device) == STATUS_NOT_SUPPORTED && !device.Cursor);
    CHECK(device.DisplayReservationEnd == 0x01234567ull && !maps);
    CHECK(DcnCursorPlanWidth(&device) == 0 && !DcnCursorCaps(&device));
    Fresh(1); fullTable = 0; CHECK(DcnCursorPrepare(&device) == STATUS_DEVICE_NOT_READY && !device.Cursor);
    Fresh(1); device.VramLength = BC250_VRAM_TOP_RESERVED + 0x01240000ull;
    CHECK(DcnCursorPrepare(&device) == STATUS_NO_MEMORY && !device.Cursor && device.DisplayReservationEnd == 0x01234567ull);
    Fresh(1); CHECK(DcnCursorPrepare(&device) == STATUS_SUCCESS && device.Cursor && maps == 1);
    /* Two surfaces after the handoff buffer, 64 KiB aligned, out of every VidMm segment, cleared. */
    CHECK(device.DisplayReservationEnd == ((0x01234567ull + 65535ull) & ~65535ull) + 2ull * BC250_CURSOR_BUFFER_BYTES);
    CHECK(vram[0] == 0 && vram[sizeof(vram) - 1] == 0);
    CHECK(DcnCursorPlanWidth(&device) == 256 && !writes);
}

static void Caps(void)
{
    BC250_CURSOR_PIPE p;
    Fresh(1); (void)DcnCursorPrepare(&device);
    CHECK(!DcnCursorCaps(&device));                         /* no pipe yet */
    p = Pipe(0, 0); DcnCursorPipeReady(&device, &p);        /* a plan without the cursor: no pointer on it */
    CHECK(!DcnCursorCaps(&device));
    Ready(); CHECK(DcnCursorCaps(&device));
    /* A start refused after the inherited pipe was adopted releases the link (connector.c): not enabled, no caps. */
    connectorEnabled = 0; CHECK(!DcnCursorCaps(&device)); connectorEnabled = 1;
    device.VidPnFlipEnabled = 0; CHECK(!DcnCursorCaps(&device)); device.VidPnFlipEnabled = 1;
    DcnCursorPipeGone(&device); CHECK(DcnCursorCaps(&device));      /* a mode change keeps the caps */
    /* A shape before any cursor pipe of this start is refused: dxgkrnl draws it. Nothing is written. */
    Fresh(1); (void)DcnCursorPrepare(&device); writes = 0;
    CHECK(SetShape(BC250_POINTER_COLOR, 32, 32, 0, 0) == STATUS_DEVICE_NOT_READY && !writes);
    /* A hot spot outside the image is refused on a ready pipe too. */
    Ready(); CHECK(SetShape(BC250_POINTER_COLOR, 32, 32, 32, 0) == STATUS_NOT_SUPPORTED);
}

static void ShapeAndPosition(void)
{
    ULONGLONG first;
    Fresh(1); (void)DcnCursorPrepare(&device); Ready();
    CHECK(SetShape(BC250_POINTER_COLOR, 32, 16, 3, 4) == STATUS_SUCCESS && Unlocked());
    CHECK(Get(BC250_REG_DMU_CURSOR0_0_CURSOR_SIZE, CURSOR0_0_CURSOR_SIZE__CURSOR_WIDTH_MASK) == 32);
    CHECK(Get(BC250_REG_DMU_CURSOR0_0_CURSOR_SIZE, CURSOR0_0_CURSOR_SIZE__CURSOR_HEIGHT_MASK) == 16);
    CHECK(Get(BC250_REG_DMU_CURSOR0_0_CURSOR_CONTROL, CURSOR0_0_CURSOR_CONTROL__CURSOR_PITCH_MASK) == 2);
    CHECK(Enable() == 0);                                   /* no visible position yet */
    first = Address();
    CHECK(first == CursorBase() || first == CursorBase() + BC250_CURSOR_BUFFER_BYTES);
    Move(100, 200, 1);
    CHECK(Enable() == 1 && Unlocked());
    CHECK(Get(BC250_REG_DMU_CURSOR0_0_CURSOR_POSITION, CURSOR0_0_CURSOR_POSITION__CURSOR_X_POSITION_MASK) == 100);
    CHECK(Get(BC250_REG_DMU_CURSOR0_0_CURSOR_POSITION, CURSOR0_0_CURSOR_POSITION__CURSOR_Y_POSITION_MASK) == 200);
    Move(0, 0, 0); CHECK(Enable() == 0);
    Move(-10, 5, 1); CHECK(Enable() == 1);                  /* left of the plane: X 0, the overhang in the hot spot */
    CHECK(Get(BC250_REG_DMU_CURSOR0_0_CURSOR_POSITION, CURSOR0_0_CURSOR_POSITION__CURSOR_X_POSITION_MASK) == 0);
    CHECK(Get(BC250_REG_DMU_CURSOR0_0_CURSOR_HOT_SPOT, CURSOR0_0_CURSOR_HOT_SPOT__CURSOR_HOT_SPOT_X_MASK) == 10);
}

/* No tear: each shape goes into the surface the registers do not name, and only after the hardware latched the
 * change that left it (UPDATE_PENDING clear, or DCN_CURSOR_LATCH_MS old). */
static void TwoSurfaces(void)
{
    ULONGLONG a, b, c;
    Fresh(1); (void)DcnCursorPrepare(&device); Ready(); Move(50, 50, 1);
    CHECK(SetShape(BC250_POINTER_COLOR, 32, 32, 0, 0) == STATUS_SUCCESS); a = Address();
    delays = 0;
    CHECK(SetShape(BC250_POINTER_COLOR, 48, 48, 0, 0) == STATUS_SUCCESS); b = Address();
    CHECK(a != b && (a > b ? a - b : b - a) == BC250_CURSOR_BUFFER_BYTES);
    /* The switch to a was not latched (pending set by its address write): the draw into a waited. */
    CHECK(delays >= 1 && delays <= 130);
    Latch(); delays = 0;
    CHECK(SetShape(BC250_POINTER_COLOR, 16, 16, 0, 0) == STATUS_SUCCESS); c = Address();
    CHECK(c == a && delays == 0);
    CHECK(Enable() == 1 && Unlocked());
}

/* A mode change: the cursor goes off before the pipe changes and comes back on the new pipe with the stored shape,
 * also with a shape that came in the gap (stored and answered with success, drawn only on the new pipe). */
static void PipeGap(void)
{
    unsigned before;
    unsigned char canary;
    Fresh(1); (void)DcnCursorPrepare(&device); Ready(); Move(300, 300, 1);
    CHECK(SetShape(BC250_POINTER_COLOR, 32, 32, 0, 0) == STATUS_SUCCESS && Enable() == 1);
    DcnCursorPipeGone(&device);
    CHECK(Enable() == 0 && Unlocked());
    before = writes; Move(310, 310, 1); CHECK(writes == before);      /* nothing programmed between the pipes */
    memset(vram, 0x77, sizeof(vram)); canary = vram[0];
    CHECK(SetShape(BC250_POINTER_COLOR, 64, 40, 1, 2) == STATUS_SUCCESS && writes == before);
    CHECK(vram[0] == canary && vram[BC250_CURSOR_BUFFER_BYTES] == canary);   /* nothing drawn either */
    Latch(); Ready();
    CHECK(Enable() == 1 && Unlocked());
    CHECK(Get(BC250_REG_DMU_CURSOR0_0_CURSOR_SIZE, CURSOR0_0_CURSOR_SIZE__CURSOR_WIDTH_MASK) == 64);
    CHECK(Get(BC250_REG_DMU_CURSOR0_0_CURSOR_POSITION, CURSOR0_0_CURSOR_POSITION__CURSOR_X_POSITION_MASK) == 310);
    /* The rotation: the stored shape is drawn again turned, and the size swaps. */
    Latch(); device.Rotation = BC250_VPPR_ROTATE90; DcnCursorRefresh(&device);
    CHECK(Get(BC250_REG_DMU_CURSOR0_0_CURSOR_SIZE, CURSOR0_0_CURSOR_SIZE__CURSOR_WIDTH_MASK) == 40);
    CHECK(Get(BC250_REG_DMU_CURSOR0_0_CURSOR_SIZE, CURSOR0_0_CURSOR_SIZE__CURSOR_HEIGHT_MASK) == 64);
    /* Not at DISPATCH_LEVEL: the ready call takes ShapeLock. */
    DcnCursorPipeGone(&device); before = writes; level = DISPATCH_LEVEL; Ready(); level = 0;
    CHECK(writes == before && Enable() == 0);
}

/* The inherited pipe: the cursor writes the plan's two cursor TTU words, with the cursor off, and refuses values that
 * do not fit their fields (the pipe is then not ready, and the caps stay off on a fresh start). */
static void Ttu(void)
{
    BC250_CURSOR_PIPE p;
    Fresh(1); (void)DcnCursorPrepare(&device);
    p = Pipe(256, 1); DcnCursorPipeReady(&device, &p);
    CHECK(DcnCursorCaps(&device));
    CHECK(Get(BC250_REG_DMU_HUBPREQ0_DCN_CUR0_TTU_CNTL0, HUBPREQ0_DCN_CUR0_TTU_CNTL0__REFCYC_PER_REQ_DELIVERY_MASK) == 77);
    CHECK(Get(BC250_REG_DMU_HUBPREQ0_DCN_CUR0_TTU_CNTL0, HUBPREQ0_DCN_CUR0_TTU_CNTL0__QoS_LEVEL_FIXED_MASK) == 8);
    CHECK(Get(BC250_REG_DMU_HUBPREQ0_DCN_CUR0_TTU_CNTL0, HUBPREQ0_DCN_CUR0_TTU_CNTL0__QoS_RAMP_DISABLE_MASK) == 1);
    CHECK(Get(BC250_REG_DMU_HUBPREQ0_DCN_CUR0_TTU_CNTL1, HUBPREQ0_DCN_CUR0_TTU_CNTL1__REFCYC_PER_REQ_DELIVERY_PRE_MASK) == 66);
    Fresh(1); (void)DcnCursorPrepare(&device);
    p = Pipe(256, 1); p.Delivery = HUBPREQ0_DCN_CUR0_TTU_CNTL0__REFCYC_PER_REQ_DELIVERY_MASK + 1u;
    DcnCursorPipeReady(&device, &p);
    CHECK(!DcnCursorCaps(&device) && !Reg(BC250_REG_DMU_HUBPREQ0_DCN_CUR0_TTU_CNTL0));
    Fresh(1); (void)DcnCursorPrepare(&device);
    p = Pipe(256, 1); p.QosLevelFixed = 16; DcnCursorPipeReady(&device, &p);
    CHECK(!DcnCursorCaps(&device) && !Reg(BC250_REG_DMU_HUBPREQ0_DCN_CUR0_TTU_CNTL0));
}

/* A failed register write in a batch: the update lock is released, and the next batch writes the enable again. */
static void FailedWrite(void)
{
    unsigned base;
    Fresh(1); (void)DcnCursorPrepare(&device); Ready();
    CHECK(SetShape(BC250_POINTER_COLOR, 32, 32, 0, 0) == STATUS_SUCCESS);
    base = writes; failAt = base + 3;            /* lock set, CURSOR_CONTROL enable, then the position: refused */
    Move(10, 10, 1);
    CHECK(Unlocked());
    failAt = 0; Move(20, 20, 1);
    CHECK(Enable() == 1 && Unlocked());
    CHECK(Get(BC250_REG_DMU_CURSOR0_0_CURSOR_POSITION, CURSOR0_0_CURSOR_POSITION__CURSOR_X_POSITION_MASK) == 20);
    /* A refused lock write: the unlock is still written. */
    base = writes; failAt = base + 1; Move(30, 30, 1); CHECK(Unlocked()); failAt = 0;
}

/* The stop: off even when the last batch said off, nothing programmed again, no caps, shapes refused. */
static void Stop(void)
{
    unsigned before;
    Fresh(1); (void)DcnCursorPrepare(&device); Ready();
    CHECK(SetShape(BC250_POINTER_COLOR, 32, 32, 0, 0) == STATUS_SUCCESS);
    Move(5, 5, 0); CHECK(Enable() == 0);
    regs[BC250_REG_DMU_CURSOR0_0_CURSOR_CONTROL / 4] |= CURSOR0_0_CURSOR_CONTROL__CURSOR_ENABLE_MASK;  /* as found */
    regs[BC250_REG_DMU_CNVC_CUR0_CURSOR0_CONTROL / 4] |= CNVC_CUR0_CURSOR0_CONTROL__CUR0_ENABLE_MASK;
    before = writes; DcnCursorStop(&device);
    CHECK(writes > before && Enable() == 0 && Unlocked());
    before = writes; Move(6, 6, 1); CHECK(writes == before && Enable() == 0);
    CHECK(SetShape(BC250_POINTER_COLOR, 32, 32, 0, 0) == STATUS_DEVICE_NOT_READY && writes == before);
    Ready(); CHECK(writes == before && !DcnCursorCaps(&device));
    DcnCursorStop(&device); CHECK(Enable() == 0);
    /* The quiet off of the bugcheck path: no lock, the update lock cleared first, both enables 0. */
    regs[BC250_REG_DMU_CUR_VUPDATE_LOCK_SET0 / 4] = 1;
    regs[BC250_REG_DMU_CURSOR0_0_CURSOR_CONTROL / 4] |= CURSOR0_0_CURSOR_CONTROL__CURSOR_ENABLE_MASK;
    regs[BC250_REG_DMU_CNVC_CUR0_CURSOR0_CONTROL / 4] |= CNVC_CUR0_CURSOR0_CONTROL__CUR0_ENABLE_MASK;
    quiet = 1; level = 15; DcnCursorOffQuiet(&device); level = 0; quiet = 0;
    CHECK(Enable() == 0 && Unlocked());
    DcnCursorRelease(&device); CHECK(!device.Cursor && unmaps == 1);
    DcnCursorRelease(&device); CHECK(unmaps == 1);
}

int main(void)
{
    Prepare();
    Caps();
    ShapeAndPosition();
    TwoSurfaces();
    PipeGap();
    Ttu();
    FailedWrite();
    Stop();
    if (device.Cursor) DcnCursorRelease(&device);
    printf("dcn cursor life: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
