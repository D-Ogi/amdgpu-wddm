/* ISM (verify-15-28, 26): ModesetStop (modeset.c), compiled unchanged against host fakes. The stop puts the native
 * viewport and scaler shape back for the next owner of the display. Before the fix it cleared PipeChanged and dropped
 * the status of that restore, so DcnStop reported a successful handoff over a scaled pipe. Now the restore's status
 * is the result, and a failed restore keeps PipeChanged, so that ResetDevice (ModesetRestoreQuiet) tries again.
 * run_modeset.ps1 extracts the body into modeset_stop_actual.inc. */
#include <stdio.h>
#include <string.h>
typedef long LONG, NTSTATUS;
typedef unsigned long ULONG;
typedef unsigned int UINT;
typedef unsigned char BOOLEAN;
#define TRUE 1
#define FALSE 0
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_IO_TIMEOUT ((NTSTATUS)0xC00000B5L)
#define NT_SUCCESS(s) ((NTSTATUS)(s) >= 0)
#define BC250_SCALING_IDENTITY 0u
typedef struct { ULONG SclModeBefore, SclModeAfter; } BC250_SCALER_RESULT;
typedef struct {
    int Lock;
    BOOLEAN PipeChanged;
    ULONG Commits, Changes, Refusals, Busy, Underflows, Restores, Scaling;
    volatile LONG SourceWidth, SourceHeight;
    LONG LastStatus;
} BC250_MODESET;
typedef struct {
    volatile LONG RuntimeDisplayChanged;
    struct { UINT Width, Height; } Post;
    BC250_MODESET Modeset;
} BC250_DEVICE;
static unsigned checks, failures, restores, locks;
static long restore_status;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %u: %s\n", (unsigned)__LINE__, #x); } } while (0)
static LONG InterlockedCompareExchange(volatile LONG* p, LONG value, LONG compare)
{ LONG old = *p; if (old == compare) *p = value; return old; }
static LONG InterlockedExchange(volatile LONG* p, LONG value) { LONG old = *p; *p = value; return old; }
static void ExAcquireFastMutex(int* lock) { CHECK(!*lock); *lock = 1; ++locks; }
static void ExReleaseFastMutex(int* lock) { CHECK(*lock); *lock = 0; --locks; }
static void GuardLog(const char* format, ...) { (void)format; }
static long RestoreNative(BC250_DEVICE* Device, BC250_SCALER_RESULT* Result)
{
    CHECK(Device->Modeset.Lock);
    Result->SclModeBefore = 1; Result->SclModeAfter = 2;
    ++restores;
    return restore_status;
}

#include "modeset_stop_actual.inc"

static void reset(BC250_DEVICE* d, BOOLEAN changed, long status)
{
    memset(d, 0, sizeof(*d));
    d->Post.Width = 1920; d->Post.Height = 1080;
    d->Modeset.PipeChanged = changed; d->Modeset.SourceWidth = 800; d->Modeset.SourceHeight = 600;
    d->Modeset.Scaling = 3;
    restores = locks = 0; restore_status = status;
}

int main(void)
{
    BC250_DEVICE d;
    /* A scaled pipe whose restore succeeds: native again, success. */
    reset(&d, TRUE, 0);
    CHECK(ModesetStop(&d) == STATUS_SUCCESS && restores == 1 && !d.Modeset.PipeChanged && !locks);
    CHECK(d.Modeset.SourceWidth == 1920 && d.Modeset.SourceHeight == 1080 && d.Modeset.Scaling == BC250_SCALING_IDENTITY);
    /* A scaled pipe whose restore fails: the failure is the result, and the pipe is still known to be scaled. */
    reset(&d, TRUE, STATUS_IO_TIMEOUT);
    CHECK(ModesetStop(&d) == STATUS_IO_TIMEOUT && restores == 1 && !locks);
    CHECK(d.Modeset.PipeChanged && d.Modeset.LastStatus == STATUS_IO_TIMEOUT && d.Modeset.Restores == 1);
    /* A native pipe: nothing to restore, success. */
    reset(&d, FALSE, STATUS_IO_TIMEOUT);
    CHECK(ModesetStop(&d) == STATUS_SUCCESS && !restores && !locks && !d.Modeset.PipeChanged);
    /* A runtime display (the connector owns the pipe): the modeset stop does nothing. */
    reset(&d, TRUE, STATUS_IO_TIMEOUT); d.RuntimeDisplayChanged = 1;
    CHECK(ModesetStop(&d) == STATUS_SUCCESS && !restores && !locks && d.Modeset.PipeChanged);
    printf("modeset stop: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
