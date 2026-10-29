/* Software transaction tests, not hardware validation. The included functions
 * are extracted unchanged from connector_link.c. Numerical planning and MMIO
 * implementations have separate real-source gates; this model tests ownership,
 * propagation, transaction order, cancellation and recovery at their boundary. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include "../dcn_scale.h"
#include "../flip_state.h"
#include "../plane_format.h"
#include "../dpaux.h"
#include "../../shim/bc250_sink.h"
#include "../../shim/bc250_atom_firmware.h"
#include "../../shim/bc250_link_atom.h"
#include "../../shim/bc250_hdmi_link.h"
#include "../../shim/bc250_dcn201_pipe.h"
#include "../../shim/bc250_optc.h"
/* BD-177: BC250_LOG_TEXT, so the refusal line is proved to fit the ring instead of assuming it. */
#include "../bc250kmd_escape.h"
#include "../../shim/bc250_dp_train.h"
#include "../../shim/bc250_dp_phy.h"
#include "../../shim/bc250_dp_stream.h"
#include "../../shim/dpcd_constants.inc"

typedef unsigned long ULONG;
typedef long LONG, NTSTATUS;
typedef unsigned UINT;
typedef unsigned char UCHAR, BOOLEAN, KIRQL;
typedef unsigned long long ULONGLONG;
typedef long long LONGLONG;
typedef size_t SIZE_T;
typedef void* PKTHREAD;
typedef int D3DKMDT_VIDPN_PRESENT_PATH_SCALING, XSTATE_SAVE;
typedef struct { LONGLONG QuadPart; } LARGE_INTEGER;
typedef struct {
    ULONGLONG PixelRate;
    struct { UINT Numerator, Denominator; } HSyncFreq, VSyncFreq;
    struct { ULONG cx, cy; } ActiveSize, TotalSize;
    unsigned VideoStandard, ScanLineOrdering;
} D3DKMDT_VIDEO_SIGNAL_INFO;
typedef struct {
    ULONG Width, Height, Pitch, ColorFormat, TargetId;
    LARGE_INTEGER PhysicAddress;
} TEST_POST;
#include "../connector_diag.h"
#define RTL_NUMBER_OF(a) (sizeof(a)/sizeof((a)[0]))
typedef struct _BC250_DEVICE {
    ULONG* Mmio;
    BOOLEAN DcnWriteEnabled, SourceVisible, ModeActive, InheritedSignalValid;
    volatile LONG StartVisibilityPending;
    D3DKMDT_VIDEO_SIGNAL_INFO InheritedSignal;
    void* ConnectorLink;
    NTSTATUS ConnectorLinkCleanup;
    void* HdmiLinkContext;
    void* Psp;
    unsigned PspSessionLock;
    BOOLEAN MmioPspEnabled,GpuStopUnconfirmed;
    unsigned char* RuntimeFramebuffer;
    ULONGLONG RuntimeFramebufferLength;
    LARGE_INTEGER RuntimeFramebufferPhysical;
    TEST_POST Post, RuntimePost;
    volatile LONG RuntimePostValid, RuntimePlaneBlank, RuntimeDisplayChanged;
    struct { volatile LONG SourceWidth, SourceHeight; ULONG Scaling,InheritedSupport;NTSTATUS InheritedPipeStatus;BC250_INHERITED_PIPE InheritedPipe; } Modeset;
    struct {unsigned Lock;BC250_CONNECTOR_OPEN_DIAG FirstOpen;BC250_CONNECTOR_TARGET_DIAG FirstAttempt;
        BC250_CONNECTOR_FAILURE_DIAG FirstFailure;BC250_CONNECTOR_RECOVERY_DIAG FirstRecovery;} Connector;
    struct { unsigned Lock; ULONG PortId, OsManufacturer, OsProduct; } DpAudio;
} BC250_DEVICE;
#include "../connector_surface.h"
#include "../connector_handoff.h"
typedef struct { volatile LONG References; ULONG Generation; BC250_SINK Sink; } BC250_CONNECTOR_SNAPSHOT;
#define TRUE 1
#define FALSE 0
#define PASSIVE_LEVEL 0
#define KernelMode 0
#define Executive 0
#define STATUS_TIMEOUT ((long)0x102)
#define POOL_FLAG_NON_PAGED 0
#define STATUS_INSUFFICIENT_RESOURCES ((long)0xc000009a)
#define STATUS_ACCESS_DENIED ((long)0xc0000022)
#define STATUS_SUCCESS 0L
#define STATUS_NOT_SUPPORTED ((long)0xc00000bb)
#define STATUS_INVALID_PARAMETER ((long)0xc000000d)
#define STATUS_DEVICE_NOT_READY ((long)0xc00000a3)
#define STATUS_DEVICE_DATA_ERROR ((long)0xc000009c)
#define STATUS_IO_DEVICE_ERROR ((long)0xc0000185)
#define STATUS_DEVICE_BUSY ((long)0x80000011)
#define STATUS_INVALID_DEVICE_STATE ((long)0xc0000184)
#define STATUS_RETRY ((long)0xc000022d)
#define STATUS_GRAPHICS_INVALID_STRIDE ((long)0xc01e033c)
#define STATUS_GRAPHICS_INVALID_VIDPN ((long)0xc01e0303)
#define NT_SUCCESS(s) ((NTSTATUS)(s)>=0)
#define D3DKMDT_VPPS_IDENTITY 1
#define D3DKMDT_VSS_OTHER 0
#define D3DDDI_VSSLO_PROGRESSIVE 0
#define D3DDDIFMT_A8R8G8B8 21
#define BC250_CHILD_UID 1
#define XSTATE_MASK_LEGACY 3
#define UNREFERENCED_PARAMETER(x) (void)(x)
#define RtlZeroMemory(p,n) memset((p),0,(n))
#define RtlCopyMemory(p,s,n) memcpy((p),(s),(n))
#define KeMemoryBarrier() ((void)0)
#include "connector_link_types.inc"
#include "hdmi_stop_types.inc"
static HDMI_LINK_CONTEXT inherited_hdmi;
static unsigned inherited_restores,inherited_frees,nested_primary,hdmi_fail_once;

static unsigned checks, failures, boundary_steps, fail_step, cancel_step, power_step, timeout_step;
static unsigned operation_locks, primary_locks, spin_locks, fp_saved, fp_saves, fp_restores;
static unsigned primary_enters, primary_exits, audio_starts, audio_stops, plan_calls, logs;
static unsigned primary_end_fail;
static char log_ring[64][512];
static unsigned log_next,diag_locks;
static unsigned char diag_snapshot[sizeof(((BC250_DEVICE*)0)->Connector)];
static unsigned test_irql, acquire_fail, hdmi_stop_fail, save_fail, plan_fail, bad_clocks, clock_reads;
/* BD-177: a display plan that returns no valid plan, and the term the OPTC gate answers with. */
static unsigned plan_invalid, optc_reason;
static unsigned apply_count, tx_enabled, plane_blank, otg_blank, crtc_enabled, output_black, aux_open;
static unsigned pipe_programs, pipe_plans;   /* BD-197: successful pipe steps, and their recorded plans */
static unsigned persistent_aux_fail, firmware_frees, pool_frees, audio_debt;
static unsigned observed_generation, fault_triggered;
/* 1 = inherited firmware surface; 2 = reserved runtime surface. */
static unsigned scanned_surface, original_surface_restores;
static BC250_TARGET_TIMING inherited_timing;
static unsigned inherited_timing_fail;
static unsigned inherited_rate;     /* BD-139: the LINK_BW_SET the firmware's link reads back with */
static unsigned inherited_lanes;    /* GitHub #45: its LANE_COUNT_SET */
static unsigned tx_off_khz,tx_off_lanes;   /* GitHub #45: the symbol clock and lanes of the last TX_OFF */
static unsigned psp_locks,psp_wait_fail,psp_cancel_on_wait,psp_waits,psp_releases;
static LONGLONG psp_last_timeout;
static unsigned trace_recovery,clock_failures,clock_argument_count,timing_argument_count;
static unsigned clock_arguments[4];
static BC250_TARGET_TIMING timing_argument;
static ULONGLONG clock_us;
static const char* events[1024];
static unsigned event_count;
static BC250_DEVICE device;
static CONNECTOR_LINK link;
static BC250_CONNECTOR_SNAPSHOT snapshot;
static ULONG fake_mmio;
static unsigned char framebuffer[1920*1080*4];
static unsigned char workspace[64];
static unsigned char atom_workspace[64];
static PKTHREAD thread=(PKTHREAD)(uintptr_t)1;
static void check(int yes,unsigned line,const char* expression)
{ ++checks;if(!yes) {++failures;printf("FAIL line %u: %s [step %u, inject %u, phase %lu]\n",line,expression,boundary_steps,fail_step,link.Phase);} }
#define CHECK(x) check(!!(x),(unsigned)__LINE__,#x)
static ULONGLONG KeQueryInterruptTime(void) { return clock_us*10ull; }
static PKTHREAD KeGetCurrentThread(void) { return thread; }
static KIRQL KeGetCurrentIrql(void) { return (KIRQL)test_irql; }
static int ConnectorGenerationCurrent(BC250_DEVICE* d,ULONG generation)
{ CHECK(d==&device);return generation==observed_generation; }
static void ConnectorOperationAcquire(BC250_DEVICE* d)
{ CHECK(d==&device && !operation_locks);++operation_locks; }
static void ConnectorOperationRelease(BC250_DEVICE* d)
{ CHECK(d==&device && operation_locks==1);--operation_locks; }
static int scanout_object=1,last_invalidate;
static int WddmPrimaryExclusiveBegin(BC250_DEVICE* d,ULONG k,ULONG* g)
{ CHECK(d==&device && k>BC250_WRITER_NONE && k<BC250_WRITER_KINDS);if(primary_locks){++nested_primary;return 0;}if(acquire_fail)return 0;*g=77;++primary_locks;++primary_enters;scanned_surface=2;return 1; }
static NTSTATUS WddmPrimaryExclusiveEnd(BC250_DEVICE* d,ULONG g,BOOLEAN changed)
{
    CHECK(d==&device && primary_locks==1 && g==77);
    // BD-195, invariant I3: no writer leaves the runtime display flag over an empty RuntimePost.
    CHECK(!d->RuntimeDisplayChanged || d->RuntimePost.Pitch);
    if(primary_end_fail)return STATUS_IO_DEVICE_ERROR;
    last_invalidate=changed;if(changed)scanout_object=0;--primary_locks;++primary_exits;
    return STATUS_SUCCESS;
}
static LONG InterlockedExchange(volatile LONG* p,LONG v) { LONG old=*p;*p=v;return old; }
static LONG InterlockedCompareExchange(volatile LONG* p,LONG v,LONG expected) { LONG old=*p;if(old==expected)*p=v;return old; }
static void KeAcquireSpinLock(unsigned* p,KIRQL* old)
{
    CHECK((p==&device.DpAudio.Lock || p==&device.Connector.Lock) && !spin_locks);
    if(p==&device.Connector.Lock) {
        CHECK(!memcmp(diag_snapshot,&device.Connector,sizeof(diag_snapshot)));++diag_locks;
    }
    *old=(KIRQL)test_irql;test_irql=2;++spin_locks;
}
static void KeReleaseSpinLock(unsigned* p,KIRQL old)
{
    CHECK((p==&device.DpAudio.Lock || p==&device.Connector.Lock) && spin_locks==1);
    if(p==&device.Connector.Lock)memcpy(diag_snapshot,&device.Connector,sizeof(diag_snapshot));
    --spin_locks;test_irql=old;
}
static void KeStallExecutionProcessor(ULONG us) { clock_us+=us; }
static NTSTATUS KeDelayExecutionThread(int mode,BOOLEAN alert,LARGE_INTEGER* delay)
{ CHECK(mode==KernelMode && !alert && delay->QuadPart<0);clock_us+=(ULONGLONG)(-delay->QuadPart)/10;return 0; }
static NTSTATUS KeSaveExtendedProcessorState(unsigned mask,XSTATE_SAVE* saved)
{ CHECK(mask==3 && !fp_saved && !primary_locks && test_irql==0);if(save_fail)return STATUS_NOT_SUPPORTED;*saved=123;++fp_saved;++fp_saves;return 0; }
static void KeRestoreExtendedProcessorState(XSTATE_SAVE* saved)
{ CHECK(*saved==123 && fp_saved==1);--fp_saved;++fp_restores; }
static void DpAudioStop(BC250_DEVICE* d) { CHECK(d==&device);++audio_stops; }
static BOOLEAN DpAudioQuiesced(BC250_DEVICE* d) { CHECK(d==&device && audio_stops);return (BOOLEAN)!audio_debt; }
static void DpAudioStart(BC250_DEVICE* d) { CHECK(d==&device && !link.Owner);++audio_starts; }
BOOLEAN HdmiLinkStop(BC250_DEVICE* d);
BOOLEAN HdmiLinkStopOwned(BC250_DEVICE* d);
static ULONG DcnPrimaryPitch(ULONG width) { return (width*4u+255u)&~255u; }
/* BD-195: dcn.c's DcnLinkSurfaceAdopt (dcn_flip_test runs the real one). Inside the writer, before the flag, and a
 * no-op once the flag is set. The sink's connection state for ConnectorLinkNeedsPowerOn. */
static unsigned adopt_calls;
static BOOLEAN sink_connected;
void DcnLinkSurfaceAdopt(BC250_DEVICE* d)
{
    CHECK(d==&device && primary_locks==1);++adopt_calls;
    if(d->RuntimeDisplayChanged || !d->RuntimeFramebuffer || !d->RuntimeFramebufferLength)return;
    d->RuntimePost=d->Post;d->RuntimePost.Pitch=DcnPrimaryPitch(d->Post.Width);
    d->RuntimePost.PhysicAddress=d->RuntimeFramebufferPhysical;
    d->RuntimePost.ColorFormat=D3DDDIFMT_A8R8G8B8;d->RuntimePost.TargetId=BC250_CHILD_UID;
}
void DcnPipeConnectorPlan(BC250_DEVICE* d,ULONG det);   /* BD-197, defined after the pipe step's stub */
/* BD-223: the mode shape replaces the shape of a plane, before the scaler step and with the pipe stopped. */
static unsigned plane_shape_forgets;
static void DcnPlaneShapeModeset(BC250_DEVICE* d,BOOLEAN programmed) { CHECK(d==&device);CHECK(programmed);++plane_shape_forgets; }
static BOOLEAN ConnectorIsConnected(BC250_DEVICE* d) { CHECK(d==&device && !operation_locks);return sink_connected; }
static NTSTATUS DcnSetVisibility(BC250_DEVICE*,BOOLEAN);
static NTSTATUS DcnRuntimeSurface(BC250_DEVICE*,BOOLEAN);
NTSTATUS ConnectorLinkRelease(BC250_DEVICE*);
static NTSTATUS DcnRestorePostDisplay(BC250_DEVICE*);
static long boundary_step(void*,const char*,int);
static BC250_LINK_SURFACE last_restored;
static ULONG captured_visible;
NTSTATUS DcnLinkSurfaceCapture(BC250_DEVICE* d,ULONG w,ULONG h,BC250_LINK_SURFACE* surface) {
    long status=boundary_step(d->ConnectorLink,"capture-surface",0);
    CHECK(primary_locks && scanned_surface==2 && ((w==1920 && h==1080) || (w==2560 && h==1440) || (w==3840 && h==2160)));
    if(!status) {
        captured_visible=!otg_blank && !plane_blank;
        // Only Visible is consumed. The deliberately different address and
        // layout must never reach the rollback's reserved-buffer operation.
        surface->Physical=0x82000000ull;surface->Pitch=256;
        surface->Format=BC250_PLANE_FORMAT_ABGR2101010;
        surface->Width=w;surface->Height=h;surface->Visible=captured_visible;
    }
    return status;
}
NTSTATUS DcnLinkSurfaceRestore(BC250_DEVICE* d,const BC250_LINK_SURFACE* surface) {
    long status=boundary_step(d->ConnectorLink,"restore-surface",0);
    CHECK(primary_locks && crtc_enabled && surface!=NULL);
    CHECK(pipe_programs && pipe_plans==pipe_programs);   /* BD-197: the format write keeps this pipe's DET */
    CHECK(surface->Physical==(ULONGLONG)d->RuntimeFramebufferPhysical.QuadPart);
    CHECK(surface->Width==link.Previous.Timing.HActive && surface->Height==link.Previous.Timing.VActive);
    CHECK(surface->Pitch==link.Previous.Pitch && surface->Format==BC250_PLANE_FORMAT_ARGB8888);
    CHECK(surface->Visible==captured_visible);
    if(!status) {last_restored=*surface;scanned_surface=2;plane_blank=0;otg_blank=!surface->Visible;++original_surface_restores;}
    return status;
}
static void GuardLog(const char* format,...)
{
    va_list args;int n;
    CHECK(format!=NULL && !spin_locks);
    CHECK(!memcmp(diag_snapshot,&device.Connector,sizeof(diag_snapshot)));
    va_start(args,format);n=vsnprintf(log_ring[log_next%64],512,format,args);va_end(args);
    CHECK(n>=0 && n<512);++log_next;++logs;
}
/* BD-195 breadcrumbs: KeepLog is off unless a case sets keep_active; each keep is one append of the ring. */
static int keep_active;static unsigned keeps;
static BOOLEAN GuardKeepActive(void) { return (BOOLEAN)keep_active; }
static void GuardLogKeep(void) { CHECK(keep_active && !spin_locks);++keeps; }
static unsigned cold_allocations,cold_fail_allocation,cold_capture_fail,cold_identity_fail;
static unsigned cold_atom_fail,cold_refclk_fail,cold_live_writes;
static unsigned policy_mode,policy_address_refused,policy_read_refused;
static ULONG policy_before,policy_written;
static ULONG mock_read(ULONG* p) { (void)p;return policy_before; }
static void mock_write(ULONG* p,ULONG v) { (void)p;policy_written=v;++cold_live_writes; }
#define READ_REGISTER_ULONG(p) mock_read(p)
#define WRITE_REGISTER_ULONG(p,v) mock_write(p,v)
static int Bc250LinkRegisterAllowed(uint32_t dword,int write)
{ (void)dword;return !policy_address_refused && (write || !policy_read_refused); }
/* Register-value policy has its own real-source gate; this fixture isolates the transaction. */
static int Bc250LinkRegisterWriteNeedsBefore(uint32_t dword) { (void)dword;return policy_mode!=0; }
static int Bc250LinkRegisterPrepareWrite(uint32_t dword,uint32_t before,uint32_t value,
    uint32_t* effective,uint32_t* reason)
{
    (void)dword;*reason=policy_mode;*effective=(before&0xffffff00u)|(value&0xffu);
    return policy_mode!=2;
}
int Bc250LinkAtomScopedRegister(const BC250_LINK_ATOM* a,uint32_t dword,int write)
{ (void)dword;(void)write;return a->ClockScope!=0; }
static NTSTATUS KeWaitForSingleObject(void* mutex,int reason,int mode,int alert,LARGE_INTEGER* timeout)
{
    CHECK(mutex==&device.PspSessionLock && !test_irql && !psp_locks);
    CHECK(reason==Executive && mode==KernelMode && !alert);
    ++psp_waits;psp_last_timeout=timeout?timeout->QuadPart:0;
    if(psp_cancel_on_wait)++observed_generation;
    if(psp_wait_fail)return STATUS_TIMEOUT;
    psp_locks=1;return STATUS_SUCCESS;
}
static void KeReleaseMutex(void* mutex,int wait)
{ CHECK(mutex==&device.PspSessionLock && psp_locks==1 && !wait);psp_locks=0;++psp_releases; }
int Bc250DispReadAllowed(unsigned long offset) { (void)offset;return 1; }
int Bc250DispWriteAllowed(unsigned long offset) { (void)offset;return 1; }
static void* ExAllocatePool2(unsigned flags,size_t bytes,ULONG tag) {
    void* p=NULL;(void)flags;++cold_allocations;
    if(cold_allocations==cold_fail_allocation)return NULL;
    if(tag==LINK_TAG) { CHECK(bytes==sizeof(link));p=&link; }
    else if(cold_allocations==2) { CHECK(bytes==sizeof(atom_workspace));p=atom_workspace; }
    else { CHECK(bytes==sizeof(workspace));p=workspace; }
    memset(p,0,bytes);return p;
}
size_t Bc250AtomContextSize(void) { return sizeof(atom_workspace); }
static NTSTATUS AtomFirmwareCapture(BC250_DEVICE* d,BC250_ATOM_FIRMWARE* f,BC250_ATOM_STATUS* why) {
    CHECK(d==&device && f==&link.Firmware);*why=BC250_ATOM_OK;
    f->Identity.VendorId=0x1002;f->Identity.DeviceId=cold_identity_fail?0:0x13fe;
    return cold_capture_fail?STATUS_DEVICE_DATA_ERROR:STATUS_SUCCESS;
}
BC250_ATOM_STATUS Bc250LinkAtomValidate(BC250_LINK_ATOM* a) {
    CHECK(a==&link.Atom && a->Workspace==atom_workspace);
    return cold_atom_fail?BC250_ATOM_IO:BC250_ATOM_OK;
}
BC250_ATOM_STATUS Bc250LinkAtomReferenceClock(const BC250_ATOM_IMAGE* image,unsigned* khz) {
    CHECK(image==&link.Firmware.Image);*khz=27000;
    return cold_refclk_fail?BC250_ATOM_IO:BC250_ATOM_OK;
}
static void AtomFirmwareRelease(BC250_ATOM_FIRMWARE* f)
{ CHECK(f==&link.Firmware);++firmware_frees; }
static void ExFreePoolWithTag(void* p,ULONG tag)
{
    if(p==&inherited_hdmi) {CHECK(tag=='LhAB');++inherited_frees;return;}
    CHECK((p==&link && tag==LINK_TAG) || ((p==workspace || p==atom_workspace) && tag==LINK_WORK_TAG));++pool_frees;
}
#include "hdmi_stop_bodies.inc"
/* BD-090: connector.c's owner and snapshot, for the post-display release's handoff. Neither is taken under
 * Connector.Operation, and every snapshot reference taken is given back. */
static int connector_enabled;
static unsigned snapshot_refs;
static BC250_CONNECTOR_SNAPSHOT* snapshot_given;
static BOOLEAN ConnectorIsEnabled(BC250_DEVICE* d) { CHECK(d==&device && !operation_locks);return (BOOLEAN)connector_enabled; }
static BC250_CONNECTOR_SNAPSHOT* ConnectorSnapshotAcquire(BC250_DEVICE* d)
{ CHECK(d==&device && !operation_locks);if(snapshot_given)++snapshot_refs;return snapshot_given; }
static void ConnectorSnapshotRelease(BC250_CONNECTOR_SNAPSHOT* s)
{ if(!s)return;CHECK(s==snapshot_given && snapshot_refs);--snapshot_refs; }
/* BD-139: dp_train_test.c runs the real rate ladder against its caps; this is the same ladder for the fixture's
 * caps, so the plan and the training model agree on the rate a timing starts at. */
unsigned Bc250DpFirstRate(const uint8_t caps[16],uint32_t khz)
{
    static const unsigned rates[4]={6,10,20,30};
    unsigned lanes=caps[DP_MAX_LANE_COUNT]&DP_MAX_LANE_COUNT_MASK,max=caps[DP_MAX_LINK_RATE],i;
    if(!khz || (lanes!=1 && lanes!=2 && lanes!=4) || (max!=6 && max!=10 && max!=20 && max!=30))return 0;
    for(i=max>=10?1u:0u;i<4u && rates[i]<=max;++i)if(khz<=rates[i]*27000u*lanes/3u)return rates[i];
    return 0;
}
int Bc250LinkAtomDpSymbolClock(unsigned khz) { return khz==162000 || khz==270000 || khz==540000 || khz==810000; }
/* BD-139: the eight-frame OTG measurement (display_tmds.h, display_timing_test.c runs the real one). The fixture
 * answers measure_hz, or fails measure_fail times first. */
static unsigned measure_calls,measure_fail,measure_expire;
static ULONG measure_hz,measure_pixel_khz;
static NTSTATUS DisplayMeasureRefresh(const BC250_DEVICE* d,ULONG ht,ULONG vt,ULONG pixel_khz,ULONG* hz,ULONG* milli)
{
    /* Before the mode is visible: the OTG runs and the plane is still blank. */
    CHECK(d==&device && crtc_enabled && plane_blank && ht && vt && pixel_khz && primary_locks && !spin_locks);
    ++measure_calls;measure_pixel_khz=pixel_khz;
    *hz=*milli=0;
    /* A try that used the rest of the transaction's deadline without seeing a frame. */
    if(measure_expire) {measure_expire=0;clock_us=link.Deadline+1;return STATUS_DEVICE_DATA_ERROR;}
    if(measure_fail) {--measure_fail;return STATUS_DEVICE_DATA_ERROR;}
    *hz=measure_hz;*milli=(ULONG)((ULONGLONG)measure_hz*1000u/((ULONGLONG)ht*vt));return STATUS_SUCCESS;
}
static ULONG Protocol(const BC250_SINK* Sink);
#include "connector_link_bodies.inc"
/* BD-169 review: connector.c's own decision about a refused start, unchanged. */
#include "connector_decision.inc"

/* Model each hardware boundary as a cancellation-aware call, as the real I/O
 * callbacks require. Failure is one-shot so rollback is independently testable.
 * No address, register, clock or timing correctness is inferred from this model. */
static long boundary_step(void* context,const char* name,int needs_owner)
{
    CONNECTOR_LINK* c=context;
    CHECK(c==&link && test_irql==0 && !spin_locks && !fp_saved);
    CHECK(event_count<sizeof(events)/sizeof(events[0]));events[event_count++]=name;
    ++boundary_steps;
    if(boundary_steps==cancel_step) { ++observed_generation;fault_triggered=1; }
    if(boundary_steps==power_step) { device.Mmio=NULL;fault_triggered=1; }
    if(boundary_steps==timeout_step) { clock_us=c->Deadline+1;fault_triggered=1; }
    if(needs_owner?!LinkOwned(c):(!device.Mmio || !device.DcnWriteEnabled))return STATUS_DEVICE_NOT_READY;
    if(boundary_steps==fail_step) { fault_triggered=1;return STATUS_IO_DEVICE_ERROR; }
    clock_us+=10;
    return 0;
}
static long step(void* context,const char* name) { return boundary_step(context,name,1); }
static long io_step(BC250_DISP_IO* io,const char* name) { return step(io->Context,name); }
static int event_index(const char* s,unsigned start)
{ unsigned i;for(i=start;i<event_count;++i)if(!strcmp(events[i],s))return (int)i;return -1; }
static unsigned event_total(const char* s)
{ unsigned i,n=0;for(i=0;i<event_count;++i)if(!strcmp(events[i],s))++n;return n; }
static void before(const char* a,const char* b)
{ int x=event_index(a,0),y=event_index(b,0);CHECK(x>=0 && y>x); }
int Bc250TargetTimingValid(const BC250_TARGET_TIMING* t)
{ return t && t->PixelClockKhz && t->HActive && t->VActive && t->HSyncWidth && t->VSyncWidth; }
int Bc250TargetTimingEqual(const BC250_TARGET_TIMING* a,const BC250_TARGET_TIMING* b)
{ return a && b && !memcmp(a,b,sizeof(*a)); }
size_t Bc250Dml201WorkspaceSize(void) { return sizeof(workspace); }
int Bc250Dml201Plan(const BC250_DML201_INPUT* in,void* w,size_t size,BC250_DML201_PLAN* p)
{
    ++plan_calls;CHECK(fp_saved==1 && w==workspace && size==sizeof(workspace));
    CHECK((in->HActive==1920 || in->HActive==2560 || in->HActive==3840) && in->PitchPixels==in->HActive);
    CHECK(in->DchubRefClockKhz==48000 && in->DentistVcoKhz==3000000);
    CHECK(in->CursorWidth==0 && in->AudioSampleRateKhz==192);
    CHECK(in->Output==(unsigned)(snapshot.Sink.Kind==BC250_SINK_DP?BC250_DML201_DP:BC250_DML201_HDMI));
    memset(p,0,sizeof(*p));if(plan_fail)return -1;
    if(plan_invalid)return 0;        /* BD-177: a plan that returns without a valid one is its own refusal. */
    p->Valid=1;p->VReady=340;p->VStartup=20;p->VUpdateOffset=550;p->VUpdateWidth=255;
    p->Rq.plane1_base_address=0x2A5;   /* BD-197: what the pipe step programs and the surface writes keep */
    return 0;
}
/* BD-177: the plan asks the gate for a term now. optc_reason is the term this fixture answers with. */
int Bc250OptcTimingReason(const BC250_TARGET_TIMING* t,const BC250_OPTC_SYNC* s)
{ CHECK(t!=NULL && s->VStartup==20);return (int)optc_reason; }
int Bc250OptcTimingValid(const BC250_TARGET_TIMING* t,const BC250_OPTC_SYNC* s)
{ return Bc250OptcTimingReason(t,s)==BC250_OPTC_REASON_OK; }
const char* Bc250OptcReasonName(int reason)
{
    /* The spelling is bc250_optc.c's; optc_test.c holds it against the real table. */
    static const char* const names[]={"optc-ok","optc-extent","optc-flags","optc-sync","optc-total",
        "optc-vstartup-blank","optc-vstartup-field","optc-vready-field","optc-vupdate-offset-field",
        "optc-vupdate-width-field"};
    if(reason<0 || (unsigned)reason>=sizeof(names)/sizeof(names[0]))return "optc-invalid";
    return names[reason];
}
long Bc250ScalerPlan(unsigned long sw,unsigned long sh,unsigned long tw,unsigned long th,unsigned long scaling,BC250_SCALER_PLAN* p)
{ CHECK(sw==tw && sh==th && scaling==BC250_SCALING_IDENTITY);memset(p,0,sizeof(*p));p->SourceWidth=sw;p->SourceHeight=sh;return 0; }
long Bc250Dcn201ReadClocks(BC250_DISP_IO* io,unsigned crystal,BC250_DCN201_CLOCKS* c)
{
    long s=io_step(io,"clocks");++clock_reads;if(s)return s;CHECK(crystal==27000);
    memset(c,0,sizeof(*c));c->CrystalClockKhz=crystal;c->DchubRefClockKhz=48000;c->DpRefClockKhz=600000;
    c->DentistVcoKhz=(bad_clocks && clock_reads==2)?2999000:3000000;return 0;
}
long Bc250Dcn201PlaneBlank(BC250_DISP_IO* io,int blank)
{ long s=io_step(io,"plane-blank");CHECK(blank==1 && primary_locks==1);if(!s)plane_blank=1;return s; }
static NTSTATUS DcnSetVisibility(BC250_DEVICE* d,BOOLEAN visible)
{ long s=boundary_step(d->ConnectorLink,"visibility-off",0);CHECK(!visible && primary_locks);if(!s)otg_blank=1;return s; }
long Bc250DpStreamBlank(BC250_DISP_IO* io) { return io_step(io,"dp-blank"); }
int Bc250HdmiLinkRestore(BC250_HDMI_LINK* h)
{
    long s;
    if(h==&inherited_hdmi.Link) {
        CHECK(primary_locks==1 && test_irql==PASSIVE_LEVEL && inherited_hdmi.Owner==thread);
        ++inherited_restores;
        if(hdmi_stop_fail || hdmi_fail_once) {
            hdmi_fail_once=0;
            h->RestoreFailure.Valid=1;h->RestoreFailure.Operation=4;
            h->RestoreFailure.Register=0x123;h->RestoreFailure.Wanted=0x55;
            h->RestoreFailure.Readback=0x44;h->RestoreFailure.Mask=0xff;
            h->RestoreFailure.ReadbackValid=1;return 1;
        }
        h->Dirty=0;return 0;
    }
    s=step(h->Io.Context,"hdmi-restore");if(!s)h->Dirty=0;return s?1:0;
}
BC250_ATOM_STATUS Bc250LinkAtomTransmitter(BC250_LINK_ATOM* a,enum bc250_link_tx_action action,unsigned protocol,unsigned khz,unsigned lanes,unsigned swing,unsigned pre)
{
    long s=step(a->Io.Context,action==BC250_LINK_TX_ON?"tx-on":action==BC250_LINK_TX_OFF?"tx-off":"tx-drive");
    CHECK(protocol==0 || protocol==2 || protocol==3);CHECK(khz && lanes && swing+pre<=3);
    if(action==BC250_LINK_TX_OFF) {tx_off_khz=khz;tx_off_lanes=lanes;}
    if(!s) {if(action==BC250_LINK_TX_ON)tx_enabled=1;else if(action==BC250_LINK_TX_OFF)tx_enabled=0;}
    return s?BC250_ATOM_IO:BC250_ATOM_OK;
}
long Bc250OptcReadTiming(BC250_DISP_IO* io,unsigned long pixelKhz,BC250_TARGET_TIMING* timing)
{
    long status=io_step(io,"read-inherited-timing");
    if(status)return status;
    if(inherited_timing_fail)return STATUS_DEVICE_DATA_ERROR;
    CHECK(pixelKhz==inherited_timing.PixelClockKhz);
    *timing=inherited_timing;return STATUS_SUCCESS;
}
long Bc250OptcDisable(BC250_DISP_IO* io)
{ long s=io_step(io,"otg-off");CHECK(plane_blank);if(!s)crtc_enabled=0;return s; }
long Bc250Dcn201PipeApply(BC250_DISP_IO* io,const BC250_DML201_PLAN* p,const BC250_DCN201_CLOCKS* c)
{
    long s=io_step(io,"pipe");CHECK(plane_blank && !crtc_enabled && p->Valid && c->DentistVcoKhz==3000000);++apply_count;
    if(!s)++pipe_programs;
    return s;
}
/* BD-197: dcn.c's DcnPipeConnectorPlan (dcn_flip_test runs the real one). Once per successful pipe step, right after
 * it and before any surface write, with the DET plane-1 base of the plan that step programmed. A failed pipe step
 * records nothing. */
void DcnPipeConnectorPlan(BC250_DEVICE* d,ULONG det)
{ CHECK(d==&device && primary_locks && !crtc_enabled && det==0x2A5 && pipe_plans+1==pipe_programs);++pipe_plans; }
BC250_ATOM_STATUS Bc250LinkAtomClock(BC250_LINK_ATOM* a,unsigned protocol,unsigned khz)
{
    long status;CHECK(protocol==0 || protocol==2 || protocol==3);CHECK(khz);
    CHECK(psp_locks==1 && ((CONNECTOR_LINK*)a->Io.Context)->ClockSession);
    if(trace_recovery) {CHECK(clock_argument_count<4);if(clock_argument_count<4)clock_arguments[clock_argument_count++]=khz;}
    status=step(a->Io.Context,"pixel-clock");
    if(trace_recovery && clock_failures) {--clock_failures;return BC250_ATOM_IO;}
    return status?BC250_ATOM_IO:BC250_ATOM_OK;
}
long Bc250OptcProgram(BC250_DISP_IO* io,const BC250_TARGET_TIMING* t,const BC250_OPTC_SYNC* sync,int dp)
{
    CHECK(t->HActive && sync->VReady==340 && (dp==0 || dp==1));CHECK(!crtc_enabled && plane_blank);
    if(trace_recovery) {timing_argument=*t;++timing_argument_count;}
    return io_step(io,"otg-program");
}
long Bc250Dcn201OutputProgram(BC250_DISP_IO* io,const BC250_DML201_PLAN* p)
{ long s=io_step(io,"output-program");CHECK(p->Valid && !crtc_enabled && plane_blank);if(!s)output_black=1;return s; }
long Bc250ScalerProgramStopped(BC250_DISP_IO* io,const BC250_SCALER_PLAN* p,BC250_SCALER_RESULT* r)
{ CHECK(p->SourceWidth && plane_blank);memset(r,0,sizeof(*r));return io_step(io,"scaler"); }
long Bc250Dcn201Route(BC250_DISP_IO* io,unsigned mode,int connect)
{ CHECK(connect==1 && (mode==0 || mode==2 || mode==3));return io_step(io,"route"); }
long Bc250DpStreamProgram(BC250_DISP_IO* io,const BC250_TARGET_TIMING* t)
{ CHECK(t->PixelClockKhz && plane_blank);return io_step(io,"dp-program"); }
long Bc250AuxOpenForDetection(BC250_AUX_SESSION* a,BC250_DISP_IO* io)
{ long s;CHECK(!aux_open);a->Io=io;a->Open=1;aux_open=1;s=io_step(io,"aux-open");return s; }
long Bc250AuxClose(BC250_AUX_SESSION* a)
{ long s;if(!a->Open)return 0;s=io_step(a->Io,"aux-close");if(persistent_aux_fail)s=STATUS_IO_DEVICE_ERROR;a->CleanupStatus=s;if(!s) {a->Open=0;aux_open=0;}return s; }
long Bc250AuxDpcdRead(BC250_AUX_SESSION* a,unsigned long address,unsigned char* data,unsigned long length)
{ CHECK(aux_open && address && length);memset(data,0,length);if(address==DP_LINK_BW_SET && length==2){data[0]=(unsigned char)inherited_rate;data[1]=(unsigned char)inherited_lanes;}return io_step(a->Io,"aux-read"); }
long Bc250AuxTransfer(BC250_AUX_SESSION* a,unsigned long action,unsigned long address,unsigned char* data,unsigned long length,unsigned long* got)
{ CHECK(aux_open && action==BC250_AUX_DP_WRITE && address && data && length);*got=length;return io_step(a->Io,"aux-write"); }
long Bc250DpPhyConfigure(BC250_DISP_IO* io,unsigned lanes,int enhanced)
{ CHECK(lanes==4 && enhanced==1);return io_step(io,"phy"); }
long Bc250DpPhyPattern(BC250_DISP_IO* io,unsigned pattern,int enhanced)
{ CHECK(pattern<=4 && enhanced==1);return io_step(io,"pattern"); }
long Bc250DpPhyDisableTraining(BC250_DISP_IO* io) { return io_step(io,"phy-off"); }
/* BD-139: the model trains at the rate the plan's ladder picks, with TPS3 above HBR; train_fail one-shot ends the
 * next training in a clock recovery failure, as a sink that does not lock at that rate. */
static unsigned train_fail,trained_caps_rate;
enum BC250_DP_TRAIN_STATUS Bc250DpTrain(const BC250_DP_TRAIN_IO* io,const uint8_t caps[16],uint32_t khz,BC250_DP_TRAIN_RESULT* r)
{
    unsigned char data[6]={0};unsigned rate=Bc250DpFirstRate(caps,khz);
    CHECK(aux_open && (caps[DP_MAX_LANE_COUNT]&DP_MAX_LANE_COUNT_MASK)==4 && khz && rate);
    trained_caps_rate=caps[DP_MAX_LINK_RATE];
    memset(r,0,sizeof(*r));r->Rate=r->FirstRate=rate;r->Lanes=4;r->Enhanced=1;r->Attempts=1;
    r->EqPattern=rate>=20?3u:2u;r->Tried[0].Rate=rate;r->Tried[0].EqPattern=r->EqPattern;r->Tried[0].Phase=2;
    if(!io->Current(io->Context) || io->Enable(io->Context,rate,4,1) ||
       io->Write(io->Context,DP_LINK_BW_SET,data,2) || io->Pattern(io->Context,1) ||
       io->Drive(io->Context,0,0) || io->DelayUs(io->Context,400) ||
       io->Read(io->Context,DP_LANE0_1_STATUS,data,6)) {
        (void)io->Disable(io->Context);r->Status=BC250_DP_TRAIN_IO_ERROR;return r->Status;
    }
    if(train_fail) {
        --train_fail;(void)io->Disable(io->Context);
        r->Phase=r->Tried[0].Phase=1;r->AmdResult=r->Tried[0].AmdResult=1;r->Status=BC250_DP_TRAIN_CR_FAILED;
        return r->Status;
    }
    if(io->Pattern(io->Context,r->EqPattern) || io->Pattern(io->Context,0)) {
        (void)io->Disable(io->Context);r->Status=BC250_DP_TRAIN_IO_ERROR;return r->Status;
    }
    r->Phase=3;return BC250_DP_TRAIN_OK;
}
BC250_ATOM_STATUS Bc250LinkAtomStream(BC250_LINK_ATOM* a,unsigned protocol,unsigned khz,unsigned lanes,unsigned rate)
{ CHECK(khz && lanes==4 && (protocol==0?(rate==6 || rate==10 || rate==20 || rate==30):rate==0));return step(a->Io.Context,"atom-stream")?BC250_ATOM_IO:BC250_ATOM_OK; }
int Bc250HdmiStreamProgram(BC250_HDMI_LINK* h,const BC250_ATOM_CALLBACKS* io,uint32_t khz,unsigned w,unsigned height,unsigned ht,unsigned vt)
{ long s;h->Io=*io;h->Dirty=1;CHECK(khz && w<ht && height<vt && !tx_enabled && !crtc_enabled);s=step(io->Context,"hdmi-program");return s?1:0; }
long Bc250OptcEnable(BC250_DISP_IO* io)
{ long s=io_step(io,"otg-on");CHECK(plane_blank && output_black && tx_enabled);if(!s)crtc_enabled=1;return s; }
static NTSTATUS DcnRuntimeSurface(BC250_DEVICE* d,BOOLEAN unblank)
{ long s=boundary_step(d->ConnectorLink,unblank?"surface":"suspend-surface",0);CHECK(primary_locks);if(unblank)CHECK(crtc_enabled && d->RuntimePostValid);
  CHECK(pipe_plans==pipe_programs);if(unblank)CHECK(pipe_programs);   /* BD-197: the plan before every surface write */
  /* BD-179: an internal unblank cannot lift the startup hold (DcnSetVisibilityCore). */
  if(!s) {scanned_surface=2;plane_blank=0;d->RuntimePlaneBlank=0;if(unblank && !d->StartVisibilityPending)otg_blank=0;}return s; }
static NTSTATUS DcnRestorePostDisplay(BC250_DEVICE* d)
{
    long status=boundary_step(d->ConnectorLink,"original-surface",0);
    CHECK(primary_locks && crtc_enabled);
    if(!status) {scanned_surface=1;plane_blank=0;otg_blank=0;++original_surface_restores;}
    return status;
}
long Bc250DpStreamUnblank(BC250_DISP_IO* io,unsigned khz,unsigned rate)
{ CHECK(crtc_enabled && !plane_blank && khz && rate==link.Training.Rate && (rate==10 || rate==20 || rate==30));return io_step(io,"dp-unblank"); }
long Bc250Dcn201OutputBlank(BC250_DISP_IO* io,unsigned w,unsigned h,int blank)
{ long s=io_step(io,"output-video");CHECK(w && h && crtc_enabled && !plane_blank);if(!s)output_black=(unsigned)blank;return s; }
int Bc250HdmiStreamMute(BC250_HDMI_LINK* h,int mute)
{ CHECK(!mute && tx_enabled && !plane_blank && (!output_black || !link.RecoveryVisible));return step(h->Io.Context,"hdmi-unmute")?1:0; }

static void reset(unsigned kind)
{
    BC250_TARGET_TIMING t={148500,1920,280,88,44,1080,45,4,5,0};
    memset(&device,0,sizeof(device));memset(&link,0,sizeof(link));memset(&snapshot,0,sizeof(snapshot));
    memset(diag_snapshot,0,sizeof(diag_snapshot));memset(log_ring,0,sizeof(log_ring));log_next=diag_locks=0;
    memset(&inherited_hdmi,0,sizeof(inherited_hdmi));
    inherited_restores=inherited_frees=nested_primary=hdmi_fail_once=0;
    device.Mmio=&fake_mmio;device.DcnWriteEnabled=1;device.ConnectorLink=&link;
    device.RuntimeFramebuffer=framebuffer;device.RuntimeFramebufferLength=sizeof(framebuffer);
    device.RuntimeFramebufferPhysical.QuadPart=0x100000;device.Post.Width=1920;device.Post.Height=1080;device.Post.Pitch=7680;
    device.SourceVisible=1;device.ModeActive=1;device.RuntimePostValid=1;
    link.Device=&device;link.Io.Context=&link;link.Atom.Io.Context=&link;link.Atom.Io.Owned=LinkOwned;
    link.DmlWorkspace=workspace;link.AtomWorkspace=atom_workspace;link.CrystalKhz=27000;link.Inherited=1;link.Active=0;
    link.Protocol=kind==BC250_SINK_DP?0:kind==BC250_SINK_DVI?2:3;
    link.PixelKhz=148500;link.Rate=10;link.Lanes=4;link.NeedsDisable=1;
    snapshot.Generation=7;snapshot.Sink.Kind=kind;snapshot.Sink.MaxPixelClockKhz=300000;
    snapshot.Sink.TargetCount=1;snapshot.Sink.Targets[0]=t;snapshot.Sink.Dpcd[DP_MAX_LANE_COUNT]=4;
    snapshot.Sink.Dpcd[DP_MAX_LINK_RATE]=10;inherited_rate=10;inherited_lanes=4;tx_off_khz=tx_off_lanes=0;
    train_fail=trained_caps_rate=measure_calls=measure_fail=measure_expire=0;measure_hz=0;
    device.InheritedSignalValid=1;device.Modeset.InheritedSupport=1;
    device.InheritedSignal.PixelRate=148500000;
    device.InheritedSignal.ActiveSize.cx=1920;device.InheritedSignal.ActiveSize.cy=1080;
    device.InheritedSignal.TotalSize.cx=2200;device.InheritedSignal.TotalSize.cy=1125;
    inherited_timing=t;inherited_timing_fail=0;
    device.Psp=&device;device.MmioPspEnabled=TRUE;
    psp_locks=psp_wait_fail=psp_cancel_on_wait=psp_waits=psp_releases=0;psp_last_timeout=0;
    trace_recovery=clock_failures=clock_argument_count=timing_argument_count=0;
    cold_allocations=cold_fail_allocation=cold_capture_fail=cold_identity_fail=0;
    cold_atom_fail=cold_refclk_fail=cold_live_writes=0;
    policy_mode=policy_address_refused=policy_read_refused=0;policy_before=policy_written=0;
    boundary_steps=fail_step=cancel_step=power_step=timeout_step=0;
    operation_locks=primary_locks=spin_locks=fp_saved=fp_saves=fp_restores=0;
    primary_enters=primary_exits=audio_starts=audio_stops=plan_calls=logs=0;
    primary_end_fail=0;
    test_irql=acquire_fail=hdmi_stop_fail=save_fail=plan_fail=bad_clocks=clock_reads=0;
    plan_invalid=optc_reason=0;
    apply_count=plane_blank=otg_blank=output_black=aux_open=event_count=fault_triggered=0;
    pipe_programs=pipe_plans=0;
    persistent_aux_fail=firmware_frees=pool_frees=audio_debt=0;
    scanned_surface=1;original_surface_restores=0;
    tx_enabled=crtc_enabled=1;observed_generation=7;clock_us=100;thread=(PKTHREAD)(uintptr_t)1;
    connector_enabled=1;snapshot_given=&snapshot;snapshot_refs=0;
}
static NTSTATUS commit(void)
{ const BC250_TARGET_TIMING* t=&snapshot.Sink.Targets[0];return ConnectorLinkCommit(&device,&snapshot,t,t->HActive,t->VActive,D3DKMDT_VPPS_IDENTITY); }
static void balanced(void)
{ CHECK(!operation_locks && !primary_locks && !spin_locks && !fp_saved && !psp_locks && !link.ClockSession);CHECK(!link.Owner && !link.Cleanup);CHECK(primary_enters==primary_exits && fp_saves==fp_restores);CHECK(aux_open==(unsigned)link.Aux.Open);if(device.Mmio && !persistent_aux_fail)CHECK(!aux_open); }
static void success(void)
{
    unsigned kind;
    for(kind=BC250_SINK_DP;kind<=BC250_SINK_DVI;++kind) {
        unsigned old_steps;D3DKMDT_VIDEO_SIGNAL_INFO signal;
        reset(kind);CHECK(commit()==0);balanced();CHECK(link.Active && !link.Inherited && link.Current.Valid);
        CHECK(link.Completed==1 && !link.Refused && link.Phase==6 && audio_starts==1);
        CHECK(device.RuntimePostValid && device.SourceVisible && !plane_blank && !device.RuntimePlaneBlank && !otg_blank && !output_black);
        CHECK(device.RuntimePost.Width==1920 && device.RuntimePost.Pitch==7680);
        CHECK(device.RuntimePost.PhysicAddress.QuadPart==device.RuntimeFramebufferPhysical.QuadPart);
        CHECK(link.Signal.PixelRate==148500000 && link.Signal.TotalSize.cx==2200);
        before("plane-blank","otg-off");before("tx-off","otg-off");before("pipe","pixel-clock");
        before("otg-program","output-program");before("output-program","otg-on");
        before("scaler","route");before("route","tx-on");before("tx-on","otg-on");
        before("otg-on","surface");before("surface","output-video");CHECK(event_total("route")==2);
        if(kind==BC250_SINK_DP) {before("dp-program","aux-open");before("aux-close","otg-on");before("dp-unblank","output-video");}
        if(kind==BC250_SINK_HDMI) {before("hdmi-program","tx-on");before("output-video","hdmi-unmute");}
        CHECK(ConnectorLinkCurrentSignal(&device,&signal)==0 && signal.PixelRate==148500000);
        observed_generation=8;CHECK(ConnectorLinkCurrentSignal(&device,&signal)==STATUS_DEVICE_NOT_READY);observed_generation=7;
        old_steps=boundary_steps;CHECK(commit()==0 && boundary_steps==old_steps && audio_starts==1 && audio_stops==1);balanced();
    }
}
static void refusal(void)
{
    reset(BC250_SINK_HDMI);observed_generation=8;CHECK(commit()==STATUS_RETRY);CHECK(!boundary_steps);balanced();
    reset(BC250_SINK_HDMI);device.Mmio=NULL;CHECK(commit()==STATUS_RETRY);CHECK(!boundary_steps);balanced();
    reset(BC250_SINK_HDMI);device.DcnWriteEnabled=0;CHECK(commit()==STATUS_RETRY);CHECK(!boundary_steps);balanced();
    reset(BC250_SINK_HDMI);test_irql=2;CHECK(commit()==STATUS_INVALID_PARAMETER && !boundary_steps);test_irql=0;balanced();
    reset(BC250_SINK_HDMI);snapshot.Sink.MaxPixelClockKhz=100000;CHECK(commit()==STATUS_NOT_SUPPORTED && !boundary_steps);balanced();
    reset(BC250_SINK_HDMI);device.RuntimeFramebufferLength=1;CHECK(commit()==STATUS_GRAPHICS_INVALID_STRIDE && !boundary_steps);balanced();
    reset(BC250_SINK_HDMI);save_fail=1;CHECK(commit()==STATUS_NOT_SUPPORTED && !primary_enters);balanced();
    reset(BC250_SINK_HDMI);plan_fail=1;CHECK(commit()==STATUS_NOT_SUPPORTED && !primary_enters);balanced();
    reset(BC250_SINK_HDMI);acquire_fail=1;CHECK(commit()==STATUS_DEVICE_BUSY && !primary_enters);balanced();
    reset(BC250_SINK_HDMI);device.HdmiLinkContext=&inherited_hdmi;hdmi_stop_fail=1;CHECK(commit()==STATUS_DEVICE_DATA_ERROR && primary_enters==1);balanced();
    reset(BC250_SINK_HDMI);audio_debt=1;CHECK(commit()==STATUS_DEVICE_NOT_READY && primary_enters==1 && !apply_count);balanced();
    reset(BC250_SINK_HDMI);bad_clocks=1;CHECK(commit()==STATUS_DEVICE_DATA_ERROR && !apply_count);balanced();
    reset(BC250_SINK_HDMI);CHECK(ConnectorLinkCommit(&device,&snapshot,&snapshot.Sink.Targets[0],1000,1080,D3DKMDT_VPPS_IDENTITY)==STATUS_GRAPHICS_INVALID_VIDPN && !boundary_steps);balanced();
    reset(BC250_SINK_HDMI);snapshot.Sink.TargetCount=0;CHECK(commit()==STATUS_GRAPHICS_INVALID_VIDPN && !boundary_steps);balanced();
}
static void failure_sweep(void)
{
    unsigned kind,n,total;
    for(kind=BC250_SINK_DP;kind<=BC250_SINK_DVI;++kind) {
        reset(kind);CHECK(commit()==0);total=boundary_steps;
        for(n=1;n<=total;++n) {
            reset(kind);fail_step=n;CHECK(!NT_SUCCESS(commit()));CHECK(fault_triggered);balanced();
            CHECK(link.Refused==1 && link.Completed==0);
            if(primary_enters)CHECK(!link.Active && !device.RuntimePostValid && !device.SourceVisible && plane_blank);
        }
    }
}
static void rollback(void)
{
    unsigned kind,n,total;
    for(kind=BC250_SINK_DP;kind<=BC250_SINK_DVI;++kind) {
        reset(kind);CHECK(commit()==0);total=boundary_steps;
        for(n=2;n<=total;++n) {
            BC250_TARGET_TIMING previous;
            reset(kind);CHECK(commit()==0);previous=link.Current.Timing;
            snapshot.Sink.Targets[0].PixelClockKhz=148510;boundary_steps=0;event_count=0;fail_step=n;fault_triggered=0;
            CHECK(!NT_SUCCESS(commit()));CHECK(fault_triggered);balanced();
            CHECK(link.Active && device.RuntimePostValid && device.SourceVisible);
            CHECK(Bc250TargetTimingEqual(&link.Current.Timing,&previous));
            CHECK(link.Completed==1 && link.Refused==1 && audio_starts==(event_total("restore-surface")?2u:1u));
        }
    }
}
static void cancellation_and_power(void)
{
    unsigned mode,n,total;
    reset(BC250_SINK_DP);CHECK(commit()==0);total=boundary_steps;
    for(mode=0;mode<3;++mode)for(n=2;n<=total;++n) {
        reset(BC250_SINK_DP);CHECK(commit()==0);snapshot.Sink.Targets[0].PixelClockKhz=148510;
        boundary_steps=0;event_count=0;fault_triggered=0;
        if(mode==0)cancel_step=n;else if(mode==1)power_step=n;else timeout_step=n;
        CHECK(!NT_SUCCESS(commit()));CHECK(fault_triggered);balanced();
        if(mode<2 && n!=2)CHECK(!link.Active && !device.RuntimePostValid && !device.SourceVisible);
        else if(mode<2)CHECK(link.Active && device.RuntimePostValid);
        else CHECK(link.Active && device.RuntimePostValid); // new bounded replay can recover after one expired operation
        if(mode==0)CHECK(event_total("pipe")<=1); // never replay a stale generation
    }
}
static void disable_suspend_resume(void)
{
    D3DKMDT_VIDEO_SIGNAL_INFO s;unsigned next_fail;
    reset(BC250_SINK_HDMI);CHECK(commit()==0);CHECK(ConnectorLinkCommit(&device,&snapshot,NULL,0,0,D3DKMDT_VPPS_IDENTITY)==0);
    balanced();CHECK(!link.Active && !device.RuntimePostValid && !device.SourceVisible && !tx_enabled);
    CHECK(!link.Current.Valid && !link.Previous.Valid);
    CHECK(ConnectorLinkCurrentSignal(&device,&s)==STATUS_DEVICE_NOT_READY);
    /* A prepared inherited link is active (cold_prepare): suspend restores the firmware surface under the writer. */
    reset(BC250_SINK_HDMI);link.Active=1;
    CHECK(ConnectorLinkSuspend(&device)==0 && link.Suspended && primary_enters==1);balanced();
    CHECK(event_total("original-surface")==1 && event_total("visibility-off")==1);
    CHECK(ConnectorLinkResume(&device,7)==0 && !link.Suspended);balanced();
    reset(BC250_SINK_HDMI);CHECK(commit()==0);
    /* Suspend caller owns Connector.Operation. DcnRuntimeSurface's production
     * implementation does not require this transaction's LinkOwned callback. */
    CHECK(ConnectorLinkSuspend(&device)==0 && link.Suspended);balanced();
    CHECK(ConnectorLinkCurrentSignal(&device,&s)==STATUS_DEVICE_NOT_READY);
    CHECK(ConnectorLinkResume(&device,7)==0);balanced();CHECK(link.Active && !link.Suspended && !device.SourceVisible && otg_blank && !plane_blank);
    reset(BC250_SINK_HDMI);CHECK(commit()==0);next_fail=boundary_steps+1;fail_step=next_fail;
    CHECK(!NT_SUCCESS(ConnectorLinkResume(&device,7)));balanced();CHECK(!link.Active && !device.RuntimePostValid);
    reset(BC250_SINK_HDMI);CHECK(commit()==0);device.Mmio=NULL;
    CHECK(!NT_SUCCESS(ConnectorLinkResume(&device,7)));balanced();CHECK(link.Active && device.RuntimePostValid); // refused before mutation
    reset(BC250_SINK_HDMI);audio_debt=1;CHECK(ConnectorLinkDisable(&device,7)==STATUS_DEVICE_NOT_READY && !primary_enters && !boundary_steps);balanced();
    reset(BC250_SINK_HDMI);audio_debt=1;CHECK(ConnectorLinkSuspend(&device)==STATUS_DEVICE_NOT_READY && !primary_enters && !boundary_steps);balanced();
}
/* BD-195, 31v2's sequence: the firmware display is inherited and no runtime mode was ever applied (RuntimePost empty,
 * RuntimePostValid 0). The detach adopts the reserved buffer inside its writer before it sets RuntimeDisplayChanged
 * (I3, which the fake End checks for every writer). A re-plug on DP, on HDMI, and from one to the other then commits.
 * While the link is off, suspend and resume take no writer and touch no hardware (I6), even when Begin would refuse. */
static void detach_before_apply(void)
{
    unsigned from,to,steps,enters;BOOLEAN active,connected;
    for(from=BC250_SINK_DP;from<=BC250_SINK_HDMI;++from)for(to=BC250_SINK_DP;to<=BC250_SINK_HDMI;++to) {
        reset(from);link.Active=1;device.RuntimePostValid=0;adopt_calls=0;
        CHECK(ConnectorLinkDisable(&device,7)==STATUS_SUCCESS);balanced();
        CHECK(adopt_calls==1 && device.RuntimeDisplayChanged && !device.RuntimePostValid && !link.Active);
        CHECK(device.RuntimePost.Pitch==7680 && device.RuntimePost.PhysicAddress.QuadPart==0x100000);
        CHECK(!device.SourceVisible && !tx_enabled && plane_blank);
        steps=boundary_steps;enters=primary_enters;acquire_fail=1;
        CHECK(ConnectorLinkSuspend(&device)==STATUS_SUCCESS && link.Suspended);balanced();
        CHECK(ConnectorLinkResume(&device,7)==STATUS_SUCCESS && !link.Suspended);balanced();
        CHECK(boundary_steps==steps && primary_enters==enters && !nested_primary);
        acquire_fail=0;snapshot.Sink.Kind=to;
        CHECK(commit()==STATUS_SUCCESS);balanced();
        CHECK(adopt_calls==2 && link.Active && link.Current.Valid && device.RuntimePostValid);
        CHECK(device.RuntimeDisplayChanged && device.RuntimePost.Pitch==7680 && device.SourceVisible);
        CHECK(link.Protocol==(to==BC250_SINK_DP?0u:3u));
    }
    /* The power-on question of CommitVidPn (display.c): a connected sink with the link off and not suspended. */
    reset(BC250_SINK_DP);sink_connected=1;
    CHECK(ConnectorLinkNeedsPowerOn(&device,&active,&connected) && !active && connected);balanced();
    link.Suspended=1;CHECK(!ConnectorLinkNeedsPowerOn(&device,&active,&connected) && connected);link.Suspended=0;
    link.Active=1;CHECK(!ConnectorLinkNeedsPowerOn(&device,&active,&connected) && active && connected);link.Active=0;
    sink_connected=0;CHECK(!ConnectorLinkNeedsPowerOn(&device,&active,&connected) && !connected);
    sink_connected=1;device.ConnectorLink=NULL;
    CHECK(!ConnectorLinkNeedsPowerOn(&device,&active,&connected) && !active && connected);
    CHECK(!operation_locks && !boundary_steps && !primary_enters);
    sink_connected=0;
}
static void end_failure_propagation(void)
{
    unsigned arm,begin=checks,bad=failures;
    // The production End preserves its writer state when synchronization
    // fails. Each real caller must surface that failure and release only its
    // unrelated PASSIVE locks. It must not start audio after a failed End.
    for(arm=0;arm<6;++arm) {
        reset(BC250_SINK_HDMI);
        // An inactive link suspends without the writer (BD-195), so the suspend arm starts from an active one.
        if(arm==2 || arm==3 || arm==4)CHECK(commit()==STATUS_SUCCESS);
        if(arm==4)psp_wait_fail=1;
        if(arm==5) {
            device.HdmiLinkContext=&inherited_hdmi;
            inherited_hdmi.Device=&device;inherited_hdmi.Link.Dirty=1;
        }
        primary_end_fail=1;
        if(arm==0)CHECK(commit()==STATUS_IO_DEVICE_ERROR && !audio_starts && link.Refused==1);
        if(arm==1)CHECK(ConnectorLinkDisable(&device,7)==STATUS_IO_DEVICE_ERROR);
        if(arm==2)CHECK(ConnectorLinkSuspend(&device)==STATUS_IO_DEVICE_ERROR);
        if(arm==3 || arm==4)CHECK(ConnectorLinkResume(&device,7)==STATUS_IO_DEVICE_ERROR);
        if(arm==5)CHECK(!HdmiLinkStop(&device));
        CHECK(primary_locks==1 && primary_enters==primary_exits+1);
        CHECK(!operation_locks && !spin_locks && !fp_saved && !psp_locks && !link.ClockSession && !link.Owner);
    }
    printf("Connector End failure: %u checks, %u failures; six callers propagate and retain writer ownership\n",checks-begin,failures-bad);
    reset(BC250_SINK_HDMI);
}
static void ownership_and_cleanup(void)
{
    ULONGLONG original_deadline,now_before;
    reset(BC250_SINK_DP);Begin(&link,7,FALSE);
    CHECK(LinkOwned(&link));thread=(PKTHREAD)(uintptr_t)2;CHECK(!LinkOwned(&link));
    CHECK(LinkDelay(&link,1)==-1);thread=(PKTHREAD)(uintptr_t)1;
    now_before=clock_us;CHECK(LinkDelay(&link,100001)==-1 && clock_us==now_before);
    CHECK(LinkDelay(&link,1000)==0 && clock_us==now_before+1000);
    CHECK(LinkDelay(&link,1001)==0 && clock_us==now_before+2001);
    original_deadline=link.Deadline;clock_us=original_deadline+1;observed_generation=8;
    link.Io.BudgetUs=123;link.Io.StalledUs=122;link.Aux.Io=&link.Io;link.Aux.Open=1;aux_open=1;
    CHECK(!LinkOwned(&link));CHECK(CloseAux(&link)==0 && !link.Aux.Open);
    CHECK(link.Deadline==original_deadline && !link.Cleanup && link.Io.BudgetUs==123 && link.Io.StalledUs==122);
    CHECK(!LinkOwned(&link));End(&link);balanced();
    reset(BC250_SINK_DP);Begin(&link,7,FALSE);link.Aux.Io=&link.Io;link.Aux.Open=1;aux_open=1;
    device.Mmio=NULL;CHECK(!NT_SUCCESS(CloseAux(&link)) && link.Aux.Open);
    CHECK(!link.Cleanup);device.Mmio=&fake_mmio;CHECK(CloseAux(&link)==0);End(&link);balanced();
}
static void retained_aux_journal(void)
{
    unsigned old_steps;
    reset(BC250_SINK_DP);CHECK(commit()==0);
    snapshot.Sink.Targets[0].PixelClockKhz=148510;
    /* A prior controller cleanup failed. The journal must survive and a later
     * operation must not overwrite it by opening a fresh AUX session. */
    link.Aux.Open=1;aux_open=1;persistent_aux_fail=1;event_count=0;
    CHECK(!NT_SUCCESS(commit()));balanced();CHECK(link.Aux.Open && aux_open);
    CHECK(!link.Active && !device.RuntimePostValid && !device.SourceVisible);
    CHECK(event_total("aux-open")==0 && event_total("pipe")==0);
    Begin(&link,7,FALSE);old_steps=boundary_steps;
    CHECK(Apply(&link,&link.Current,link.Caps,NULL)==STATUS_INVALID_DEVICE_STATE && boundary_steps==old_steps);
    End(&link);
    /* PnP release is terminal. It reports restoration failure, frees the owner
     * and cannot claim to have restored the failed controller state. */
    CHECK(!NT_SUCCESS(ConnectorLinkRelease(&device)));
    CHECK(!device.ConnectorLink && pool_frees==3 && firmware_frees==1 && link.Aux.Open);
    balanced();old_steps=boundary_steps;CHECK(ConnectorLinkRelease(&device)==0 && pool_frees==3 && boundary_steps==old_steps);
    reset(BC250_SINK_DP);link.Aux.Io=&link.Io;link.Aux.Open=1;aux_open=1;
    CHECK(ConnectorLinkRelease(&device)==0);CHECK(!device.ConnectorLink && pool_frees==3 && firmware_frees==1);
    CHECK(!link.Aux.Open);balanced();
}
static void cold_prepare(void)
{
    unsigned kind,fault;
    for(kind=BC250_SINK_DP;kind<=BC250_SINK_DVI;++kind) {
        reset(kind);device.ConnectorLink=NULL;
        CHECK(ConnectorLinkPrepare(&device,&snapshot.Sink,7)==STATUS_SUCCESS);
        CHECK(device.ConnectorLink==&link && snapshot.Sink.TargetCount==1 && plan_calls==2);
        CHECK(cold_allocations==3 && !pool_frees && !cold_live_writes);
        CHECK(link.Active && link.Inherited && !link.Owner && !primary_enters);
        CHECK(!apply_count && !audio_starts && !audio_stops && tx_enabled && crtc_enabled);
        CHECK(ConnectorLinkRelease(&device)==STATUS_SUCCESS);
        CHECK(pool_frees==3 && firmware_frees==1 && !device.ConnectorLink);
        CHECK(device.Connector.FirstOpen.Recorded && device.Connector.FirstOpen.Step==BC250_OPEN_READY);
        device.RuntimeFramebuffer=NULL;
        CHECK(ConnectorLinkPrepare(&device,&snapshot.Sink,8)==STATUS_NOT_SUPPORTED);
        CHECK(device.Connector.FirstOpen.Step==BC250_OPEN_READY && device.Connector.FirstOpen.Generation==7);
    }
    for(fault=1;fault<=7;++fault) {
        CONNECTOR_LINK* out=(CONNECTOR_LINK*)(uintptr_t)1;
        reset(BC250_SINK_HDMI);device.ConnectorLink=NULL;
        if(fault<=3)cold_fail_allocation=fault;
        if(fault==4)cold_capture_fail=1;
        if(fault==5)cold_identity_fail=1;
        if(fault==6)cold_atom_fail=1;
        if(fault==7)cold_refclk_fail=1;
        CHECK(!NT_SUCCESS(OpenContext(&device,7,&out)) && out==NULL);
        CHECK(pool_frees==(fault<=3?fault-1:3u));
        CHECK(!cold_live_writes && !apply_count && !device.ConnectorLink);
        CHECK(device.Connector.FirstOpen.Recorded && device.Connector.FirstOpen.Step==fault+3);
        CHECK(device.Connector.FirstOpen.Status<0);
        device.RuntimeFramebuffer=NULL;
        CHECK(OpenContext(&device,8,&out)==STATUS_NOT_SUPPORTED);
        CHECK(device.Connector.FirstOpen.Step==fault+3 && device.Connector.FirstOpen.Generation==7);
    }
    for(fault=1;fault<=3;++fault) {
        reset(BC250_SINK_HDMI);device.ConnectorLink=NULL;
        if(fault==1)device.RuntimeFramebuffer=NULL;
        if(fault==2)device.Modeset.InheritedSupport=0;
        if(fault==3)device.InheritedSignalValid=0;
        CHECK(ConnectorLinkPrepare(&device,&snapshot.Sink,7)==STATUS_NOT_SUPPORTED);
        CHECK(!cold_allocations && !cold_live_writes);
        CHECK(device.Connector.FirstOpen.Step==fault);
    }
    reset(BC250_SINK_HDMI);device.ConnectorLink=NULL;
    snapshot.Sink.Targets[1]=snapshot.Sink.Targets[0];snapshot.Sink.TargetCount=2;
    snapshot.Sink.Targets[0].PixelClockKhz=400000;
    CHECK(ConnectorLinkPrepare(&device,&snapshot.Sink,7)==STATUS_SUCCESS);
    CHECK(snapshot.Sink.TargetCount==1 && snapshot.Sink.Targets[0].PixelClockKhz==148500);
    CHECK(!cold_live_writes && !apply_count && !audio_starts);
    CHECK(ConnectorLinkRelease(&device)==STATUS_SUCCESS && pool_frees==3);
    reset(BC250_SINK_HDMI);device.ConnectorLink=NULL;plan_fail=1;
    CHECK(ConnectorLinkPrepare(&device,&snapshot.Sink,7)==STATUS_NOT_SUPPORTED);
    CHECK(snapshot.Sink.TargetCount==0 && !cold_live_writes && !link.Owner && !apply_count);
    CHECK(ConnectorLinkRelease(&device)==STATUS_SUCCESS && pool_frees==3);
}
static void prepare_cold(unsigned kind)
{
    scanout_object=1;last_invalidate=0;
    reset(kind);device.ConnectorLink=NULL;device.RuntimePostValid=0;device.RuntimeDisplayChanged=0;
    device.SourceVisible=FALSE;device.ModeActive=FALSE;
    CHECK(ConnectorLinkPrepare(&device,&snapshot.Sink,7)==STATUS_SUCCESS);
    CHECK(link.Current.Valid && link.Current.Inherited && scanned_surface==1);
    if(kind==BC250_SINK_HDMI) {
        inherited_hdmi.Device=&device;inherited_hdmi.Link.Dirty=1;
        device.HdmiLinkContext=&inherited_hdmi;
    }
    snapshot.Sink.Targets[0].PixelClockKhz=148510;
    boundary_steps=event_count=0;plan_calls=0;clock_reads=0;
}
static void release_then_fresh_start(void)
{
    unsigned i,old_steps,begin=checks,bad=failures;
    reset(BC250_SINK_HDMI);CHECK(commit()==STATUS_SUCCESS);
    for(i=0;i<3;++i) {
        CHECK(tx_enabled && crtc_enabled && link.Hdmi.Dirty);
        old_steps=boundary_steps;
        CHECK(ConnectorLinkRelease(&device)==STATUS_SUCCESS);
        CHECK(!device.ConnectorLink && boundary_steps==old_steps);
        CHECK(tx_enabled && crtc_enabled); // PnP handoff preserves the picture.
        cold_allocations=0; // fresh allocation sequence, retained hardware state
        inherited_timing=snapshot.Sink.Targets[0];
        device.InheritedSignal.PixelRate=(ULONGLONG)inherited_timing.PixelClockKhz*1000u;
        device.RuntimePostValid=0;device.RuntimeDisplayChanged=0;
        device.SourceVisible=FALSE;device.ModeActive=FALSE;
        snapshot.Generation=++observed_generation;event_count=0;
        CHECK(ConnectorLinkPrepare(&device,&snapshot.Sink,snapshot.Generation)==STATUS_SUCCESS);
        CHECK(link.NeedsDisable && !link.Hdmi.Dirty && tx_enabled && crtc_enabled);
        // As in the lab, the selected EDID clock differs from the measured
        // inherited clock; an identical mode would correctly need no modeset.
        snapshot.Sink.Targets[0].PixelClockKhz+=10;
        CHECK(commit()==STATUS_SUCCESS);balanced();
        before("tx-off","otg-off");before("otg-off","hdmi-program");before("hdmi-program","tx-on");
        CHECK(!event_total("hdmi-restore")); // old journal was freed
        CHECK(tx_enabled && crtc_enabled && device.RuntimePostValid && device.SourceVisible);
    }
    CHECK(ConnectorLinkRelease(&device)==STATUS_SUCCESS);
    printf("Connector fresh start: %u checks, %u failures; release preserves hardware, new owner stops before HDMI\n",checks-begin,failures-bad);
}
static void cold_recovery(void)
{
    unsigned kind,n,total;
    for(kind=BC250_SINK_DP;kind<=BC250_SINK_DVI;++kind) {
        prepare_cold(kind);CHECK(commit()==STATUS_SUCCESS);CHECK(!scanout_object && last_invalidate);total=boundary_steps;
        for(n=1;n<=total;++n) {
            BC250_CONNECTOR_FAILURE_DIAG savedFailure;
            prepare_cold(kind);fail_step=n;
            CHECK(!NT_SUCCESS(commit()) && fault_triggered);balanced();
            CHECK(link.Active && !device.SourceVisible && !device.ModeActive);
            CHECK(scanned_surface==(primary_enters?2u:1u) && !plane_blank);
            CHECK(!otg_blank);
            if(event_total("restore-surface"))CHECK(last_restored.Physical==(ULONGLONG)device.RuntimeFramebufferPhysical.QuadPart);
            CHECK(scanout_object && !last_invalidate);
            CHECK(!device.RuntimeDisplayChanged && !device.RuntimePostValid);
            CHECK(Bc250TargetTimingEqual(&link.Current.Timing,&inherited_timing));
            CHECK(device.Connector.FirstAttempt.Recorded && device.Connector.FirstAttempt.PixelKhz==148510);
            CHECK(device.Connector.FirstFailure.Recorded && device.Connector.FirstFailure.Status<0);
            savedFailure=device.Connector.FirstFailure;
            CHECK(ConnectorLinkRelease(&device)==STATUS_SUCCESS);
            CHECK(!memcmp(&savedFailure,&device.Connector.FirstFailure,sizeof(savedFailure)));
        }
    }
}
static void cold_retry_and_cancel(void)
{
    unsigned n,total,mode;
    reset(BC250_SINK_HDMI);device.ConnectorLink=NULL;inherited_timing_fail=1;
    CHECK(!NT_SUCCESS(ConnectorLinkPrepare(&device,&snapshot.Sink,7)));
    CHECK(device.ConnectorLink==NULL && pool_frees==3 && !cold_live_writes);
    inherited_timing_fail=0;cold_allocations=0;
    CHECK(ConnectorLinkPrepare(&device,&snapshot.Sink,7)==STATUS_SUCCESS);
    CHECK(link.Current.Valid && link.Current.Inherited && !cold_live_writes);
    CHECK(ConnectorLinkRelease(&device)==STATUS_SUCCESS);
    prepare_cold(BC250_SINK_DP);CHECK(commit()==STATUS_SUCCESS);total=boundary_steps;
    for(mode=0;mode<2;++mode)for(n=3;n<=total;++n) {
        prepare_cold(BC250_SINK_DP);
        if(mode)power_step=n;else cancel_step=n;
        CHECK(!NT_SUCCESS(commit()) && fault_triggered);balanced();
        CHECK(!link.Active && !device.SourceVisible && !device.RuntimePostValid);
        CHECK(!scanout_object && last_invalidate);
        CHECK(!event_total("restore-surface"));
        CHECK(device.Connector.FirstFailure.Recorded);
    }
}
static void callback_error_retention(void)
{
    ULONG value;
    reset(BC250_SINK_HDMI);Begin(&link,7,FALSE);
    LinkStep(&link,BC250_LINK_SURFACE_CAPTURE,FALSE);observed_generation=8;
    CHECK(LinkRead(&link,4,&value)==STATUS_DEVICE_NOT_READY);
    observed_generation=7;
    CHECK(LinkRead(&link,0,&value)==STATUS_SUCCESS);
    CHECK(link.IoStatus==STATUS_DEVICE_NOT_READY && link.IoOffset==4);
    LinkStep(&link,BC250_LINK_SURFACE_APPLY,FALSE);
    CHECK(link.IoStatus==STATUS_SUCCESS && link.IoOffset==0);
    observed_generation=8;
    CHECK(LinkWrite(&link,8,0)==STATUS_DEVICE_NOT_READY);
    observed_generation=7;
    CHECK(LinkRead(&link,0,&value)==STATUS_SUCCESS);
    CHECK(link.IoStatus==STATUS_DEVICE_NOT_READY && link.IoOffset==8);
    End(&link);
}
static void inherited_journal_controls(void)
{
    ULONG generation;BC250_HDMI_RESTORE_DETAIL detail;
    reset(BC250_SINK_HDMI);device.HdmiLinkContext=&inherited_hdmi;
    inherited_hdmi.Device=&device;inherited_hdmi.Link.Dirty=1;
    test_irql=2;CHECK(!HdmiLinkStop(&device) && !inherited_restores && !primary_enters);
    CHECK(!HdmiLinkStopOwned(&device) && !inherited_restores);test_irql=0;
    acquire_fail=1;CHECK(!HdmiLinkStop(&device) && !inherited_restores);acquire_fail=0;
    hdmi_stop_fail=1;
    CHECK(!HdmiLinkStop(&device) && inherited_restores==1 && !inherited_frees);
    CHECK(device.HdmiLinkContext==&inherited_hdmi && !inherited_hdmi.Owner);
    CHECK(primary_enters==1 && primary_exits==1 && !primary_locks);
    HdmiLinkGetRestoreFailure(&device,&detail);
    CHECK(detail.Valid && detail.Register==0x123 && detail.Wanted==0x55 && detail.Readback==0x44);
    hdmi_stop_fail=0;
    CHECK(HdmiLinkStop(&device) && inherited_restores==2 && inherited_frees==1);
    CHECK(!device.HdmiLinkContext && !primary_locks && !inherited_hdmi.Owner);
    HdmiLinkGetRestoreFailure(&device,&detail);CHECK(!detail.Valid && !detail.ReadbackValid);
    device.HdmiLinkContext=&inherited_hdmi;
    CHECK(WddmPrimaryExclusiveBegin(&device,BC250_WRITER_HDMI_STOP,&generation));
    CHECK(HdmiLinkStopOwned(&device) && !nested_primary && inherited_frees==2);
    CHECK(primary_locks==1);WddmPrimaryExclusiveEnd(&device,generation,FALSE);
    prepare_cold(BC250_SINK_HDMI);hdmi_fail_once=1;
    CHECK(commit()==STATUS_DEVICE_DATA_ERROR);
    CHECK(!nested_primary && inherited_restores==2 && inherited_frees==1);
    CHECK(link.Active && scanned_surface==2 && !otg_blank && !plane_blank);
    CHECK(!device.SourceVisible && !device.ModeActive && !device.HdmiLinkContext);
    CHECK(device.Connector.FirstFailure.Step==BC250_LINK_HDMI_HANDOFF);
    CHECK(device.Connector.FirstFailure.HdmiRestore.Valid &&
        device.Connector.FirstFailure.HdmiRestore.Wanted==0x55 &&
        device.Connector.FirstFailure.HdmiRestore.Readback==0x44);
    CHECK(commit()==STATUS_SUCCESS && !nested_primary);
    CHECK(ConnectorLinkCommit(&device,&snapshot,NULL,0,0,D3DKMDT_VPPS_IDENTITY)==STATUS_SUCCESS);
    CHECK(!link.Active && !device.ModeActive && !device.SourceVisible);
    CHECK(commit()==STATUS_SUCCESS && !nested_primary);
    prepare_cold(BC250_SINK_HDMI);hdmi_fail_once=1;
    CHECK(ConnectorLinkCommit(&device,&snapshot,NULL,0,0,D3DKMDT_VPPS_IDENTITY)==STATUS_DEVICE_DATA_ERROR);
    CHECK(link.Active && scanned_surface==2 && !otg_blank && !plane_blank);
    CHECK(!device.SourceVisible && !device.ModeActive && !nested_primary);
    CHECK(inherited_restores==2 && inherited_frees==1);
    CHECK(device.Connector.FirstFailure.Step==BC250_LINK_HDMI_HANDOFF &&
        device.Connector.FirstFailure.HdmiRestore.Valid &&
        device.Connector.FirstFailure.HdmiRestore.ReadbackValid);
    CHECK(ConnectorLinkCommit(&device,&snapshot,NULL,0,0,D3DKMDT_VPPS_IDENTITY)==STATUS_SUCCESS);
    CHECK(!link.Active && !device.ModeActive && !device.SourceVisible);
    CHECK(commit()==STATUS_SUCCESS && !nested_primary);
}
static int logged(const char* needle)
{
    unsigned i;for(i=0;i<64;++i)if(strstr(log_ring[i],needle))return 1;return 0;
}
static void diagnostic_wrap_replay(void)
{
    BC250_CONNECTOR_FAILURE_DIAG first;
    BC250_CONNECTOR_RECOVERY_DIAG recovery;
    unsigned i,before;
    prepare_cold(BC250_SINK_HDMI);
    audio_debt=1;CHECK(commit()==STATUS_DEVICE_NOT_READY);audio_debt=0;
    first=device.Connector.FirstFailure;
    CHECK(first.Recorded && first.Step==BC250_LINK_AUDIO);
    hdmi_fail_once=1;CHECK(commit()==STATUS_DEVICE_DATA_ERROR);
    recovery=device.Connector.FirstRecovery;
    CHECK(recovery.Recorded && recovery.Attempt==2 && recovery.PreviousValid && recovery.GenerationCurrent);
    CHECK(recovery.Status==STATUS_SUCCESS && recovery.Active);
    CHECK(recovery.Original.Recorded && recovery.Original.Step==BC250_LINK_HDMI_HANDOFF);
    CHECK(recovery.Original.HdmiRestore.Valid && recovery.Original.HdmiRestore.Wanted==0x55);
    CHECK(!recovery.Failure.Recorded && !memcmp(&first,&device.Connector.FirstFailure,sizeof(first)));
    for(i=0;i<128;++i)GuardLog("unrelated ring overwrite %u",i);
    CHECK(!logged("link: failure") && !logged("hdmi-restore"));
    before=diag_locks;ConnectorLinkLogOpen(&device);
    CHECK(diag_locks>before && logged("first-retained") && logged("hdmi-restore"));
    CHECK(logged("recovery retained attempt 2 status 00000000 previous 1 generation-current 1 active 1"));
    CHECK(!memcmp(&first,&device.Connector.FirstFailure,sizeof(first)));
    CHECK(!memcmp(&recovery,&device.Connector.FirstRecovery,sizeof(recovery)));
    audio_debt=1;CHECK(commit()==STATUS_DEVICE_NOT_READY);audio_debt=0;
    CHECK(!memcmp(&recovery,&device.Connector.FirstRecovery,sizeof(recovery)));
    CHECK(ConnectorLinkRelease(&device)==STATUS_SUCCESS);
    memset(log_ring,0,sizeof(log_ring));ConnectorLinkLogOpen(&device);
    CHECK(logged("first-retained") && logged("hdmi-restore"));
    CHECK(!memcmp(&recovery,&device.Connector.FirstRecovery,sizeof(recovery)));
}
static void detached_recovery_record(void)
{
    prepare_cold(BC250_SINK_HDMI);
    CHECK(ConnectorLinkCommit(&device,&snapshot,NULL,0,0,D3DKMDT_VPPS_IDENTITY)==STATUS_SUCCESS);
    CHECK(!link.Active && !link.Current.Valid && !link.Previous.Valid);
    bad_clocks=1;clock_reads=0;
    CHECK(commit()==STATUS_DEVICE_DATA_ERROR);
    CHECK(device.Connector.FirstRecovery.Recorded && !device.Connector.FirstRecovery.PreviousValid);
    CHECK(device.Connector.FirstRecovery.Status==STATUS_NOT_SUPPORTED);
    CHECK(!device.Connector.FirstRecovery.Active && device.Connector.FirstRecovery.GenerationCurrent);
    CHECK(device.Connector.FirstRecovery.Original.Recorded && !device.Connector.FirstRecovery.Failure.Recorded);
    memset(log_ring,0,sizeof(log_ring));ConnectorLinkLogOpen(&device);
    CHECK(logged("recovery retained attempt 2 status C00000BB previous 0 generation-current 1 active 0"));
    CHECK(!logged("failure recovery-retained"));
    CHECK(ConnectorLinkRelease(&device)==STATUS_SUCCESS);
}
static void failure_detail_scope(void)
{
    unsigned atom,step;
    const unsigned steps[]={BC250_LINK_STREAM,BC250_LINK_HDMI_MUTE,BC250_LINK_SCALER};
    for(atom=0;atom<2;++atom)for(step=0;step<3;++step) {
        BC250_CONNECTOR_FAILURE_DIAG saved;
        reset(BC250_SINK_HDMI);
        link.Step=steps[step];link.AtomValid=atom;
        link.ScalerResult.LockStatusValid=1;link.ScalerResult.LockStatus=0x1234;
        link.Hdmi.StreamFailure.Valid=1;link.Hdmi.StreamFailure.Operation=3;
        link.Hdmi.StreamFailure.Register=0x4321;link.Hdmi.StreamFailure.Wanted=0x55;
        link.Hdmi.StreamStage=4;
        SaveFailure(&link,STATUS_IO_DEVICE_ERROR);
        CHECK(link.Failure.Recorded && link.Failure.Status==STATUS_IO_DEVICE_ERROR);
        CHECK(link.Failure.LockStatusValid==(unsigned)(steps[step]==BC250_LINK_SCALER));
        if(steps[step]==BC250_LINK_SCALER)CHECK(link.Failure.LockStatus==0x1234);
        CHECK(link.Failure.HdmiStream.Valid==(unsigned)(!atom && steps[step]!=BC250_LINK_SCALER));
        if(link.Failure.HdmiStream.Valid) {
            CHECK(link.Failure.HdmiStage==4 && link.Failure.HdmiStream.Register==0x4321);
            CHECK(link.Failure.HdmiStream.Wanted==0x55 && link.Failure.HdmiStream.Operation==3);
        }
        saved=link.Failure;link.Hdmi.StreamFailure.Wanted=0x99;
        SaveFailure(&link,STATUS_DEVICE_NOT_READY);
        CHECK(!memcmp(&saved,&link.Failure,sizeof(saved)));
    }
}
static void atom_write_decisions(void)
{
    BC250_CONNECTOR_ATOM_WRITE_DIAG first;
    unsigned writes;
    reset(BC250_SINK_HDMI);Begin(&link,snapshot.Generation,FALSE);
    LinkStep(&link,BC250_LINK_PIXEL_CLOCK,TRUE);
    policy_mode=1;policy_before=0xaabbcc00;
    CHECK(AtomWrite(&link,0,0x77)==0 && policy_written==0xaabbcc77);
    CHECK(cold_live_writes==1 && link.AtomWriteDiag.Valid && link.AtomWriteDiag.Allowed);
    CHECK(link.AtomWriteDiag.Before==policy_before && link.AtomWriteDiag.Requested==0x77);
    CHECK(link.AtomWriteDiag.Effective==0xaabbcc77 && link.AtomWriteDiag.Reason==1);
    first=link.AtomWriteDiag;
    CHECK(AtomWrite(&link,0,0x88)==0 && policy_written==0xaabbcc88);
    CHECK(!memcmp(&first,&link.AtomWriteDiag,sizeof(first)));
    writes=cold_live_writes;policy_mode=2;
    CHECK(AtomWrite(&link,0,0x99)!=0 && cold_live_writes==writes);
    CHECK(!link.AtomWriteDiag.Allowed && link.AtomWriteDiag.Requested==0x99);
    CHECK(link.AtomWriteDiag.Effective==0 && link.IoStatus==STATUS_ACCESS_DENIED);
    SaveFailure(&link,STATUS_IO_DEVICE_ERROR);
    CHECK(link.Failure.AtomWrite.Valid && !link.Failure.AtomWrite.Allowed);
    RetainFailure(&device,&link.Failure);
    memset(log_ring,0,sizeof(log_ring));ConnectorLinkLogOpen(&device);
    CHECK(logged("atom-write") && logged("request 00000099"));
    LinkStep(&link,BC250_LINK_PIXEL_CLOCK,TRUE);
    CHECK(!link.AtomWriteDiag.Valid && link.IoStatus==STATUS_SUCCESS);
    policy_read_refused=1;
    CHECK(AtomWrite(&link,0,0x11)!=0 && cold_live_writes==writes);
    CHECK(!link.AtomWriteDiag.Valid && link.IoStatus==STATUS_ACCESS_DENIED);
    policy_read_refused=0;policy_address_refused=1;
    LinkStep(&link,BC250_LINK_PIXEL_CLOCK,TRUE);
    CHECK(AtomWrite(&link,0,0x11)!=0 && cold_live_writes==writes);
    CHECK(!link.AtomWriteDiag.Valid && link.IoStatus==STATUS_ACCESS_DENIED);
    policy_address_refused=0;End(&link);LinkStep(&link,BC250_LINK_PIXEL_CLOCK,TRUE);
    CHECK(AtomWrite(&link,0,0x11)!=0 && cold_live_writes==writes);
    CHECK(link.IoStatus==STATUS_DEVICE_NOT_READY);
}
static void distinct_recovery_target(void)
{
    unsigned failures_to_inject;
    for(failures_to_inject=1;failures_to_inject<=2;++failures_to_inject) {
        BC250_TARGET_TIMING previous,requested;
        const BC250_CONNECTOR_RECOVERY_DIAG* r=&device.Connector.FirstRecovery;
        reset(BC250_SINK_HDMI);requested=snapshot.Sink.Targets[0];
        previous=requested;previous.PixelClockKhz=174500;previous.HBlank=160;previous.VBlank=39;
        snapshot.Sink.Targets[0]=previous;CHECK(commit()==STATUS_SUCCESS);
        CHECK(link.Current.Timing.PixelClockKhz==174500);
        snapshot.Sink.Targets[0]=requested;trace_recovery=1;clock_failures=failures_to_inject;
        CHECK(commit()==STATUS_IO_DEVICE_ERROR);balanced();
        CHECK(clock_argument_count==2 && clock_arguments[0]==148500 && clock_arguments[1]==174500);
        CHECK(r->Recorded && r->ReplayAttempted && r->PreviousValid);
        CHECK(r->Original.Target.PixelKhz==148500 && r->Original.Target.HTotal==2200);
        CHECK(r->PreviousTarget.PixelKhz==174500 && r->PreviousTarget.HTotal==2080 && r->PreviousTarget.VTotal==1119);
        CHECK(Bc250TargetTimingEqual(&link.Current.Timing,&previous));
        CHECK(!aux_open && !audio_debt);
        if(failures_to_inject==1) {
            CHECK(r->Status==STATUS_SUCCESS && r->Active && !r->Failure.Recorded);
            CHECK(timing_argument_count==1 && Bc250TargetTimingEqual(&timing_argument,&previous));
            CHECK(link.Active && device.RuntimePostValid && device.SourceVisible && device.ModeActive);
            CHECK(!plane_blank && !output_black && tx_enabled && crtc_enabled);
            CHECK(link.Signal.PixelRate==174500000 && link.Signal.TotalSize.cx==2080 && link.Signal.TotalSize.cy==1119);
            CHECK(original_surface_restores==1 && audio_starts==2 && link.Hdmi.Dirty);
        } else {
            CHECK(r->Status==STATUS_IO_DEVICE_ERROR && !r->Active && r->Failure.Recorded);
            CHECK(r->Failure.Target.PixelKhz==174500 && r->Failure.Target.HTotal==2080);
            CHECK(!timing_argument_count && !link.Active && !device.RuntimePostValid && !device.SourceVisible);
            CHECK(plane_blank && !link.Hdmi.Dirty && audio_starts==1);
        }
        memset(log_ring,0,sizeof(log_ring));ConnectorLinkLogOpen(&device);
        CHECK(logged("recovery-original") && logged("recovery-previous") && logged("replay-attempted 1"));
        trace_recovery=0;
    }
}
static void psp_session_controls(void)
{
    unsigned arm,writes,stops;
    for(arm=0;arm<5;++arm) {
        reset(BC250_SINK_HDMI);CHECK(commit()==STATUS_SUCCESS);
        writes=cold_live_writes;stops=audio_stops;
        snapshot.Sink.Targets[0].PixelClockKhz=148510;
        if(arm==0)psp_wait_fail=1;
        if(arm==1)psp_cancel_on_wait=1;
        if(arm==2)device.Psp=NULL;
        if(arm==3)device.MmioPspEnabled=FALSE;
        if(arm==4)device.GpuStopUnconfirmed=TRUE;
        CHECK(!NT_SUCCESS(commit()));balanced();
        CHECK(cold_live_writes==writes && audio_stops==stops && link.Active && device.RuntimePostValid);
        CHECK(psp_last_timeout<0 && psp_last_timeout>=-(LONGLONG)LINK_DEADLINE_US*10);
        CHECK(psp_releases==psp_waits-(arm==0?1u:0u));
    }
    reset(BC250_SINK_HDMI);Begin(&link,7,FALSE);policy_address_refused=1;
    LinkStep(&link,BC250_LINK_PIXEL_CLOCK,TRUE);
    CHECK(!AtomAllowed(&link,0x5e02,0));
    CHECK(link.IoOffset==0x5e02*4u && link.IoAccess==1 && link.IoStatus==STATUS_ACCESS_DENIED);
    link.Atom.ClockScope=1;LinkStep(&link,BC250_LINK_PIXEL_CLOCK,TRUE);
    CHECK(!AtomAllowed(&link,0x5e02,1)); // scope cannot substitute for the session
    CHECK(ClockSessionEnter(&link)==STATUS_SUCCESS);
    LinkStep(&link,BC250_LINK_PIXEL_CLOCK,TRUE);
    CHECK(AtomAllowed(&link,0x5e02,1));
    ++observed_generation;
    CHECK(!AtomAllowed(&link,0x5e02,1) && link.IoAccess==2);
    ClockSessionLeave(&link);End(&link);balanced();
}
static void programmed_clock_bound(void)
{
    LINK_MODE mode;BC250_TARGET_TIMING timing;LINK_PLAN_REFUSAL refusal;
    reset(BC250_SINK_HDMI);snapshot.Sink.MaxPixelClockKhz=600000;
    Begin(&link,snapshot.Generation,FALSE);
    timing=snapshot.Sink.Targets[0];timing.PixelClockKhz=304250;
    CHECK(Plan(&link,&snapshot.Sink,&timing,&mode,&refusal)==STATUS_NOT_SUPPORTED);
    CHECK(!mode.Valid && !plan_calls && !clock_reads);
    // BD-177: the bound of this driver, not of the sink, and the line says which of the two refused.
    CHECK(refusal.Check==LINK_PLAN_CLOCK_CEILING && refusal.Optc==0);
    timing.PixelClockKhz=300001;
    CHECK(Plan(&link,&snapshot.Sink,&timing,&mode,&refusal)==STATUS_NOT_SUPPORTED);
    CHECK(!mode.Valid && !plan_calls && !clock_reads);
    CHECK(refusal.Check==LINK_PLAN_CLOCK_CEILING);
    timing.PixelClockKhz=300000;
    CHECK(Plan(&link,&snapshot.Sink,&timing,&mode,&refusal)==STATUS_SUCCESS && mode.Valid);
    CHECK(refusal.Check==LINK_PLAN_OK);
    // A plan with no refusal record at all is still admitted the same way.
    CHECK(Plan(&link,&snapshot.Sink,&timing,&mode,NULL)==STATUS_SUCCESS && mode.Valid);
    End(&link);balanced();
}
// BD-177: every check of the plan that answers STATUS_NOT_SUPPORTED names itself, the OPTC term is carried
// through, and the one log line stays inside the ring's text budget. The lab monitor's refused preferred mode
// is the last case: 1920x1080 at 228800 kHz, 20 lines of vertical blanking, prefetch 20.
static void plan_refusal_named(void)
{
    LINK_MODE mode;BC250_TARGET_TIMING timing;LINK_PLAN_REFUSAL refusal;unsigned i;
    static const struct { unsigned long clock; unsigned long width, height; unsigned check; } cases[]={
        {24999,1920,1080,LINK_PLAN_CLOCK_FLOOR},
        {250000,1920,1080,LINK_PLAN_CLOCK_SINK},
        {150000,4097,1080,LINK_PLAN_EXTENT},
        {150000,1920,2161,LINK_PLAN_EXTENT}
    };
    reset(BC250_SINK_HDMI);snapshot.Sink.MaxPixelClockKhz=240000;
    Begin(&link,snapshot.Generation,FALSE);
    for(i=0;i<RTL_NUMBER_OF(cases);++i) {
        timing=snapshot.Sink.Targets[0];
        timing.PixelClockKhz=cases[i].clock;timing.HActive=cases[i].width;timing.VActive=cases[i].height;
        CHECK(Plan(&link,&snapshot.Sink,&timing,&mode,&refusal)==STATUS_NOT_SUPPORTED);
        CHECK(refusal.Check==cases[i].check && !mode.Valid && !plan_calls);
    }
    // A descriptor the EDID gate itself refuses, before any clock term.
    timing=snapshot.Sink.Targets[0];timing.VSyncWidth=0;
    CHECK(Plan(&link,&snapshot.Sink,&timing,&mode,&refusal)==STATUS_NOT_SUPPORTED);
    CHECK(refusal.Check==LINK_PLAN_TIMING && refusal.VBlank==snapshot.Sink.Targets[0].VBlank);
    // The two display-plan refusals are told apart.
    snapshot.Sink.MaxPixelClockKhz=300000;timing=snapshot.Sink.Targets[0];
    plan_fail=1;
    CHECK(Plan(&link,&snapshot.Sink,&timing,&mode,&refusal)==STATUS_NOT_SUPPORTED);
    CHECK(refusal.Check==LINK_PLAN_DML_CALL && refusal.VStartup==0);
    plan_fail=0;plan_invalid=1;
    CHECK(Plan(&link,&snapshot.Sink,&timing,&mode,&refusal)==STATUS_NOT_SUPPORTED);
    CHECK(refusal.Check==LINK_PLAN_DML_INVALID && refusal.VStartup==0);
    plan_invalid=0;
    // The OPTC gate: the term and the two numbers it compared travel to the line.
    optc_reason=BC250_OPTC_REASON_VSTARTUP_BLANK;
    timing.VBlank=20;
    CHECK(Plan(&link,&snapshot.Sink,&timing,&mode,&refusal)==STATUS_NOT_SUPPORTED);
    CHECK(refusal.Check==LINK_PLAN_OPTC && refusal.Optc==BC250_OPTC_REASON_VSTARTUP_BLANK);
    CHECK(refusal.VStartup==20 && refusal.VBlank==20);
    memset(log_ring,0,sizeof(log_ring));
    timing.PixelClockKhz=228800;timing.HActive=1920;timing.VActive=1080;
    LogPlanRefusal(LINK_SITE_TARGET,&timing,STATUS_NOT_SUPPORTED,&refusal);
    CHECK(logged("connector-link: target 1920x1080 228800 kHz refused C00000BB "
                 "check optc-vstartup-blank vstartup 20 vblank 20"));
    // The three sites are told apart, and a check outside the OPTC gate names itself.
    memset(log_ring,0,sizeof(log_ring));
    refusal.Check=LINK_PLAN_DML_INVALID;refusal.Optc=0;
    LogPlanRefusal(LINK_SITE_COMMIT,&timing,STATUS_NOT_SUPPORTED,&refusal);
    CHECK(logged("connector-link: commit 1920x1080 228800 kHz refused C00000BB check dml-invalid"));
    // The widest line this form can produce still fits BC250_LOG_TEXT, so the ring never truncates a refusal.
    memset(log_ring,0,sizeof(log_ring));
    refusal.Check=LINK_PLAN_OPTC;refusal.Optc=BC250_OPTC_REASON_VUPDATE_OFFSET_FIELD;
    refusal.VStartup=4095;refusal.VBlank=4095;
    timing.PixelClockKhz=300000;timing.HActive=4096;timing.VActive=2160;
    LogPlanRefusal(LINK_SITE_INHERITED,&timing,STATUS_NOT_SUPPORTED,&refusal);
    CHECK(logged("connector-link: inherited 4096x2160 300000 kHz refused C00000BB "
                 "check optc-vupdate-offset-field vstartup 4095 vblank 4095"));
    CHECK(strlen(log_ring[(log_next-1)%64])<BC250_LOG_TEXT);
    optc_reason=BC250_OPTC_REASON_OK;
    End(&link);balanced();
}
// BD-169 review: a cold prepare that fails releases its own context, and that
// release is the only one that meets a failed AUX restore: the caller's release
// answers SUCCESS for the freed context, while dpaux.c keeps the session open on
// purpose. The decision a refused start makes must therefore read all three
// results, or it preserves a display over hardware it did not restore.
static void prepare_cleanup_preserved(void)
{
    NTSTATUS prepare,release;unsigned dirty;
    // The control: a cold prepare that restores AUX, released by its caller.
    reset(BC250_SINK_DP);device.ConnectorLink=NULL;
    prepare=ConnectorLinkPrepare(&device,&snapshot.Sink,7);
    CHECK(NT_SUCCESS(prepare) && NT_SUCCESS(device.ConnectorLinkCleanup));
    release=ConnectorLinkRelease(&device);
    CHECK(NT_SUCCESS(release) && !aux_open && !device.ConnectorLink);
    CHECK(ConnectorRefusalPreserved(&device,STATUS_SUCCESS,release));
    // The other two terms still count when the cleanup is clean.
    CHECK(!ConnectorRefusalPreserved(&device,STATUS_DEVICE_NOT_READY,release));
    CHECK(!ConnectorRefusalPreserved(&device,STATUS_SUCCESS,STATUS_DEVICE_NOT_READY));
    balanced();
    // A restore that keeps failing: the prepare fails, its own release cannot
    // close the session either, and the start refuses to call that preserved.
    reset(BC250_SINK_DP);device.ConnectorLink=NULL;persistent_aux_fail=1;
    prepare=ConnectorLinkPrepare(&device,&snapshot.Sink,7);
    dirty=aux_open;
    CHECK(!NT_SUCCESS(prepare) && dirty && !device.ConnectorLink);
    CHECK(device.ConnectorLinkCleanup==STATUS_IO_DEVICE_ERROR);
    release=ConnectorLinkRelease(&device);
    CHECK(NT_SUCCESS(release));     // nothing is left to release: NULL answers success
    CHECK(!ConnectorRefusalPreserved(&device,STATUS_SUCCESS,release));
    balanced();
    // The next attempt owns the result again: no stale obligation of a previous
    // one, and none missing because this one released nothing.
    reset(BC250_SINK_DP);device.ConnectorLink=NULL;
    prepare=ConnectorLinkPrepare(&device,&snapshot.Sink,7);
    release=ConnectorLinkRelease(&device);
    CHECK(NT_SUCCESS(prepare) && ConnectorRefusalPreserved(&device,STATUS_SUCCESS,release));
    balanced();
}
/* BD-195: with KeepLog set, each step of one operation reaches the disk once, before it runs, also while DP
 * training alternates TRAIN and PHY; without KeepLog, never. A detach keeps its own steps again. */
static void keep_breadcrumbs(void)
{
    unsigned on;
    reset(BC250_SINK_DP);keep_active=0;keeps=0;CHECK(commit()==0 && !keeps);
    reset(BC250_SINK_DP);keep_active=1;keeps=0;CHECK(commit()==0);on=keeps;
    CHECK(on>=4 && on<32);
    reset(BC250_SINK_HDMI);link.Active=1;keeps=0;CHECK(ConnectorLinkDisable(&device,7)==STATUS_SUCCESS);balanced();
    CHECK(keeps>=2 && keeps<32);
    keep_active=0;
}
/* BD-090: DxgkDdiStopDeviceAndReleasePostDisplayOwnership on a link that is off (ConnectorLinkHandoff; dispmprt.md,
 * required steps 2, 5 and 6). The stop restores only a link that scans a surface: after an OS detach (an empty-topology
 * commit) or a physical one, the release enables a target first. The mode that the OS detach took down comes first,
 * while the same sink generation offers it; else the first offered timing of at least 800x600. */
static NTSTATUS commit_second(void)
{ const BC250_TARGET_TIMING* t=&snapshot.Sink.Targets[1];return ConnectorLinkCommit(&device,&snapshot,t,t->HActive,t->VActive,D3DKMDT_VPPS_IDENTITY); }
static void two_targets(unsigned kind)
{ reset(kind);snapshot.Sink.TargetCount=2;snapshot.Sink.Targets[1]=snapshot.Sink.Targets[0];snapshot.Sink.Targets[1].PixelClockKhz=148510; }
static NTSTATUS os_detach(void) { return ConnectorLinkCommit(&device,&snapshot,NULL,0,0,D3DKMDT_VPPS_IDENTITY); }
static void release_handoff(void)
{
    BC250_LINK_HANDOFF h;unsigned kind,steps,begin=checks,bad=failures;
    BC250_TARGET_TIMING small={25175,640,160,16,96,480,45,10,2,0};
    for(kind=BC250_SINK_DP;kind<=BC250_SINK_DVI;++kind) {
        /* The OS detach, then the release: the detached mode again, on the reserved buffer, black and visible. */
        two_targets(kind);CHECK(commit_second()==STATUS_SUCCESS);CHECK(os_detach()==STATUS_SUCCESS);balanced();
        CHECK(!link.Active && device.RuntimeDisplayChanged && !device.RuntimePostValid && !device.SourceVisible);
        framebuffer[0]=0xcd;framebuffer[sizeof(framebuffer)-1]=0xcd;
        CHECK(ConnectorLinkHandoff(&device,&h)==STATUS_SUCCESS);balanced();
        CHECK(h.Action==BC250_LINK_HANDOFF_ENABLED && h.Status==STATUS_SUCCESS && h.Detached==1);
        CHECK(h.Width==1920 && h.Height==1080 && h.PixelKhz==148510);
        CHECK(link.Active && link.Current.Valid && link.Current.Timing.PixelClockKhz==148510 && link.Completed==3);
        CHECK(device.RuntimePostValid && device.RuntimeDisplayChanged && device.SourceVisible && !otg_blank && !plane_blank);
        CHECK(device.RuntimePost.Width==1920 && device.RuntimePost.Height==1080 && device.RuntimePost.Pitch==7680);
        CHECK(device.RuntimePost.PhysicAddress.QuadPart==device.RuntimeFramebufferPhysical.QuadPart);
        CHECK(!framebuffer[0] && !framebuffer[sizeof(framebuffer)-1] && !snapshot_refs);
        /* A link that is on: the stop restores what it scans, and nothing is programmed. */
        steps=boundary_steps;CHECK(ConnectorLinkHandoff(&device,&h)==STATUS_SUCCESS);balanced();
        CHECK(h.Action==BC250_LINK_HANDOFF_ACTIVE && !h.Width && boundary_steps==steps && link.Completed==3 && !snapshot_refs);
        /* A physical detach forgets the mode: a sink offered again before the OS commits gets its first target. */
        two_targets(kind);CHECK(commit_second()==STATUS_SUCCESS);CHECK(ConnectorLinkDisable(&device,7)==STATUS_SUCCESS);
        CHECK(ConnectorLinkHandoff(&device,&h)==STATUS_SUCCESS);balanced();
        CHECK(h.Action==BC250_LINK_HANDOFF_ENABLED && !h.Detached && h.PixelKhz==148500 && link.Active && device.RuntimePostValid);
        /* An OS detach in an older sink generation: its mode is not offered to the new one. */
        two_targets(kind);CHECK(commit_second()==STATUS_SUCCESS);CHECK(os_detach()==STATUS_SUCCESS);
        snapshot.Generation=8;observed_generation=8;
        CHECK(ConnectorLinkHandoff(&device,&h)==STATUS_SUCCESS);balanced();
        CHECK(h.Action==BC250_LINK_HANDOFF_ENABLED && !h.Detached && h.PixelKhz==148500 && link.Active);
        /* The enable fails: the link stays off, and the release fails as before, with this reason. */
        two_targets(kind);CHECK(commit_second()==STATUS_SUCCESS);CHECK(os_detach()==STATUS_SUCCESS);
        fail_step=boundary_steps+1;fault_triggered=0;
        CHECK(!NT_SUCCESS(ConnectorLinkHandoff(&device,&h)));balanced();CHECK(fault_triggered);
        CHECK(h.Action==BC250_LINK_HANDOFF_FAILED && !NT_SUCCESS(h.Status) && h.Detached==1 && h.PixelKhz==148510);
        CHECK(!link.Active && !device.RuntimePostValid && !snapshot_refs);
    }
    /* Required step 2: no display connected. */
    reset(BC250_SINK_DP);snapshot.Sink.Kind=BC250_SINK_NONE;
    CHECK(ConnectorLinkHandoff(&device,&h)==STATUS_NOT_SUPPORTED);balanced();
    CHECK(h.Action==BC250_LINK_HANDOFF_NO_SINK && !boundary_steps && !primary_enters && !snapshot_refs);
    reset(BC250_SINK_DP);snapshot_given=NULL;
    CHECK(ConnectorLinkHandoff(&device,&h)==STATUS_NOT_SUPPORTED && h.Action==BC250_LINK_HANDOFF_NO_SINK);balanced();
    /* Required step 5: no offered timing of at least 800x600, so nothing to enable. */
    reset(BC250_SINK_DP);snapshot.Sink.Targets[0]=small;
    CHECK(ConnectorLinkHandoff(&device,&h)==STATUS_NOT_SUPPORTED);balanced();
    CHECK(h.Action==BC250_LINK_HANDOFF_NO_TARGET && !boundary_steps && !primary_enters && !snapshot_refs);
    /* Suspended, or no link owner: nothing is programmed. */
    reset(BC250_SINK_DP);link.Suspended=1;
    CHECK(ConnectorLinkHandoff(&device,&h)==STATUS_DEVICE_NOT_READY);balanced();
    CHECK(h.Action==BC250_LINK_HANDOFF_SUSPENDED && !boundary_steps && !snapshot_refs);
    reset(BC250_SINK_DP);device.ConnectorLink=NULL;
    CHECK(ConnectorLinkHandoff(&device,&h)==STATUS_DEVICE_NOT_READY && h.Action==BC250_LINK_HANDOFF_SUSPENDED && !snapshot_refs);
    /* The static inherited route (BD-155): no connector owner, and the firmware stream stays as it is. */
    reset(BC250_SINK_DP);connector_enabled=0;
    CHECK(ConnectorLinkHandoff(&device,&h)==STATUS_SUCCESS);balanced();
    CHECK(h.Action==BC250_LINK_HANDOFF_STATIC && !boundary_steps && !primary_enters && !snapshot_refs);
    printf("Post-display handoff: %u checks, %u failures; detached mode, first target, refusals\n",checks-begin,failures-bad);
    reset(BC250_SINK_HDMI);
}
/* BD-139: the HP X32 of the field report (2560x1440, 165 Hz by its maker, on DP directly). The 75 Hz entry is that tester's firmware timing
 * (total 2720x1492, 304.25 MHz); 120, 144 and 165 Hz are reduced-blanking reconstructions, not that monitor's EDID. */
static const BC250_TARGET_TIMING hpx32[4]={
    {304250,2560,160,48,32,1440,52,3,5,0x1a},{497750,2560,160,48,32,1440,85,3,5,0x1a},
    {586600,2560,80,8,32,1440,103,3,8,0x1a},{645100,2560,80,8,32,1440,41,3,8,0x1a}};
/* Model caps for that sink (no DPCD dump of it exists): DPCD 1.2, HBR2, four lanes, TPS3, enhanced framing. */
static void hbr2_sink(unsigned kind)
{
    reset(kind);snapshot.Sink.Dpcd[DP_MAX_LINK_RATE]=0x14;snapshot.Sink.Dpcd[DP_MAX_LANE_COUNT]=0xc4;
    snapshot.Sink.MaxPixelClockKhz=kind==BC250_SINK_DP?720000:300000;
    device.RuntimeFramebufferLength=10240ull*1440u;
}
static NTSTATUS commit_timing(const BC250_TARGET_TIMING* t)
{
    snapshot.Sink.Targets[0]=*t;snapshot.Sink.TargetCount=1;
    return ConnectorLinkCommit(&device,&snapshot,t,t->HActive,t->VActive,D3DKMDT_VPPS_IDENTITY);
}
static void hbr2_admission(void)
{
    LINK_MODE mode;LINK_PLAN_REFUSAL refusal;BC250_TARGET_TIMING timing;unsigned i,kind;
    static const unsigned rates[4]={10,20,20,20};
    CHECK(BC250_TARGET_MAX_PIXEL_KHZ==BC250_DP_MAX_PIXEL_KHZ);
    hbr2_sink(BC250_SINK_DP);Begin(&link,snapshot.Generation,FALSE);
    for(i=0;i<4;++i) {
        CHECK(Plan(&link,&snapshot.Sink,&hpx32[i],&mode,&refusal)==STATUS_SUCCESS && mode.Valid);
        CHECK(refusal.Check==LINK_PLAN_OK && mode.Lanes==4 && mode.Protocol==BC250_LINK_DP);
        CHECK(Bc250DpFirstRate(snapshot.Sink.Dpcd,hpx32[i].PixelClockKhz)==rates[i]);
    }
    /* An HBR x4 sink (the Lenovo's caps) keeps 360 MHz: 75 Hz fits, 120 Hz and up are the sink's own refusal. */
    snapshot.Sink.Dpcd[DP_MAX_LINK_RATE]=10;snapshot.Sink.MaxPixelClockKhz=360000;
    CHECK(Plan(&link,&snapshot.Sink,&hpx32[0],&mode,&refusal)==STATUS_SUCCESS);
    for(i=1;i<4;++i)
        CHECK(Plan(&link,&snapshot.Sink,&hpx32[i],&mode,&refusal)==STATUS_NOT_SUPPORTED && refusal.Check==LINK_PLAN_CLOCK_SINK);
    /* The DP ceiling is HBR3 x4 at RGB8, whatever a sink claims. */
    snapshot.Sink.Dpcd[DP_MAX_LINK_RATE]=0x1e;snapshot.Sink.MaxPixelClockKhz=1200000;timing=hpx32[3];
    timing.PixelClockKhz=1080001;
    CHECK(Plan(&link,&snapshot.Sink,&timing,&mode,&refusal)==STATUS_NOT_SUPPORTED && refusal.Check==LINK_PLAN_CLOCK_CEILING);
    timing.PixelClockKhz=1080000;
    CHECK(Plan(&link,&snapshot.Sink,&timing,&mode,&refusal)==STATUS_SUCCESS && mode.Valid);
    End(&link);balanced();
    /* TMDS keeps the 300 MHz of its route, also when an adapter claims more (the LG behind the passive adapter). */
    for(kind=BC250_SINK_HDMI;kind<=BC250_SINK_DVI;++kind) {
        hbr2_sink(kind);snapshot.Sink.MaxPixelClockKhz=720000;Begin(&link,snapshot.Generation,FALSE);
        for(i=0;i<4;++i)
            CHECK(Plan(&link,&snapshot.Sink,&hpx32[i],&mode,&refusal)==STATUS_NOT_SUPPORTED && refusal.Check==LINK_PLAN_CLOCK_CEILING);
        End(&link);balanced();
    }
    /* A discovery of all four names each timing above 300 MHz with its rate, and the fastest. */
    hbr2_sink(BC250_SINK_DP);
    for(i=0;i<4;++i)snapshot.Sink.Targets[i]=hpx32[i];
    snapshot.Sink.TargetCount=4;memset(log_ring,0,sizeof(log_ring));
    CHECK(ConnectorLinkPrepare(&device,&snapshot.Sink,7)==STATUS_SUCCESS && snapshot.Sink.TargetCount==4);
    CHECK(logged("connector-link: target 2560x1440 304250 kHz offered rate 10 lanes 4"));
    CHECK(logged("connector-link: target 2560x1440 497750 kHz offered rate 20 lanes 4"));
    CHECK(logged("connector-link: target 2560x1440 645100 kHz offered rate 20 lanes 4"));
    CHECK(logged("connector-link: offered 4 of 4 targets fastest 2560x1440 645100 kHz rate 20"));
    balanced();
}
static void hbr2_commit(void)
{
    hbr2_sink(BC250_SINK_DP);CHECK(commit()==STATUS_SUCCESS);balanced();
    CHECK(link.Current.Rate==10 && link.Training.EqPattern==2 && !measure_calls && trained_caps_rate==0x14);
    memset(log_ring,0,sizeof(log_ring));measure_hz=645100000;
    CHECK(commit_timing(&hpx32[3])==STATUS_SUCCESS);balanced();
    CHECK(link.Active && link.Current.Rate==20 && link.Current.Lanes==4 && link.Training.EqPattern==3);
    CHECK(measure_calls==1 && device.SourceVisible && !plane_blank);
    CHECK(logged("connector-link: dp trained 645100 kHz first 20 rate 20 lanes 4 tps 3 attempts 1 status 0"));
    CHECK(logged("connector-link: dto check 645100 kHz ref 600000 kHz measured 645100000 Hz"));
    CHECK(logged("connector-link: enabled generation 7 protocol 0 2560x1440 pitch 10240 pixel 645100 kHz lanes 4 rate 20"));
    /* 144 Hz is under the DTO reference, so nothing is measured; 75 Hz trains at HBR with TPS2 as before. */
    measure_calls=0;
    CHECK(commit_timing(&hpx32[2])==STATUS_SUCCESS && !measure_calls && link.Current.Rate==20);balanced();
    CHECK(commit_timing(&hpx32[0])==STATUS_SUCCESS && link.Current.Rate==10 && link.Training.EqPattern==2);balanced();
}
static void hbr2_training_fallback(void)
{
    BC250_TARGET_TIMING previous;unsigned steps;
    hbr2_sink(BC250_SINK_DP);CHECK(commit()==STATUS_SUCCESS);previous=link.Current.Timing;
    memset(log_ring,0,sizeof(log_ring));train_fail=1;
    CHECK(commit_timing(&hpx32[3])==STATUS_IO_DEVICE_ERROR);balanced();
    /* The previous mode is back at HBR, and this attachment stays below HBR2. */
    CHECK(link.Active && device.RuntimePostValid && Bc250TargetTimingEqual(&link.Current.Timing,&previous));
    CHECK(link.Current.Rate==10 && link.RateLimit==10 && !train_fail && !link.PixelLimitKhz);
    CHECK(logged("connector-link: dp train try 1 rate 20 lanes 4 tps 3 phase 1 result 1"));
    CHECK(logged("connector-link: dp trained 645100 kHz first 20 rate 20 lanes 4 tps 3 attempts 1 status 6"));
    CHECK(logged("connector-link: dp rate 20 failed for 645100 kHz; this sink trains at rate 10 or below until replugged"));
    /* Asked again, the HBR2 timings are refused before any hardware step, and the line names the check. */
    memset(log_ring,0,sizeof(log_ring));steps=boundary_steps;
    CHECK(commit_timing(&hpx32[3])==STATUS_NOT_SUPPORTED && boundary_steps==steps);balanced();
    CHECK(logged("connector-link: commit 2560x1440 645100 kHz refused C00000BB check link-rate"));
    CHECK(commit_timing(&hpx32[1])==STATUS_NOT_SUPPORTED && boundary_steps==steps);balanced();
    /* What HBR carries still sets, and trains with the lowered caps. */
    CHECK(commit_timing(&hpx32[0])==STATUS_SUCCESS && link.Current.Rate==10 && trained_caps_rate==10);balanced();
    /* A new discovery is a new attachment: the ceiling goes and the HBR2 timings are offered again. */
    snapshot.Sink.Targets[0]=hpx32[3];snapshot.Sink.Targets[1]=hpx32[0];snapshot.Sink.TargetCount=2;
    CHECK(ConnectorLinkPrepare(&device,&snapshot.Sink,7)==STATUS_SUCCESS && snapshot.Sink.TargetCount==2);
    CHECK(!link.RateLimit && !link.PixelLimitKhz);balanced();
    /* A failure at HBR leaves no ceiling, as before: the training itself already tried RBR where RBR carries. */
    reset(BC250_SINK_DP);train_fail=1;
    CHECK(commit()==STATUS_IO_DEVICE_ERROR && !link.RateLimit && link.Training.FirstRate==10);balanced();
}
static void dto_check(void)
{
    BC250_TARGET_TIMING previous;unsigned steps;
    hbr2_sink(BC250_SINK_DP);CHECK(commit()==STATUS_SUCCESS);previous=link.Current.Timing;
    /* A DTO that stops at its reference shows 165 Hz as 153 Hz. */
    memset(log_ring,0,sizeof(log_ring));measure_hz=600000000;
    CHECK(commit_timing(&hpx32[3])==STATUS_DEVICE_DATA_ERROR);balanced();
    CHECK(link.Active && Bc250TargetTimingEqual(&link.Current.Timing,&previous) && link.PixelLimitKhz==600000);
    CHECK(measure_calls==1 && !link.RateLimit);
    CHECK(logged("connector-link: dto check 645100 kHz ref 600000 kHz measured 600000000 Hz"));
    CHECK(logged("tries 1 status C000009C"));
    CHECK(logged("connector-link: dp pixel clock above 600000 kHz refused for this sink until replugged"));
    memset(log_ring,0,sizeof(log_ring));steps=boundary_steps;
    CHECK(commit_timing(&hpx32[3])==STATUS_NOT_SUPPORTED && boundary_steps==steps);balanced();
    CHECK(logged("connector-link: commit 2560x1440 645100 kHz refused C00000BB check dto-ref"));
    /* Everything at or under the reference still sets: 144 Hz is 586.6 MHz. */
    CHECK(commit_timing(&hpx32[2])==STATUS_SUCCESS && link.Current.Rate==20);balanced();
    /* No measurement in three tries fails closed too; one within 1% passes after two failed tries. */
    hbr2_sink(BC250_SINK_DP);CHECK(commit()==STATUS_SUCCESS);measure_fail=3;measure_hz=645100000;
    CHECK(commit_timing(&hpx32[3])==STATUS_DEVICE_DATA_ERROR && measure_calls==3 && link.PixelLimitKhz==600000);balanced();
    hbr2_sink(BC250_SINK_DP);CHECK(commit()==STATUS_SUCCESS);measure_fail=2;measure_hz=648000000;
    CHECK(commit_timing(&hpx32[3])==STATUS_SUCCESS && measure_calls==3 && !link.PixelLimitKhz);balanced();
    /* A failed try that ends the transaction's time still counts: the ceiling is set, the old mode recovered. */
    hbr2_sink(BC250_SINK_DP);CHECK(commit()==STATUS_SUCCESS);previous=link.Current.Timing;
    memset(log_ring,0,sizeof(log_ring));measure_expire=1;measure_hz=645100000;
    CHECK(commit_timing(&hpx32[3])==STATUS_DEVICE_DATA_ERROR && measure_calls==1 && link.PixelLimitKhz==600000);
    CHECK(link.Active && Bc250TargetTimingEqual(&link.Current.Timing,&previous));
    CHECK(logged("measured 0 Hz 0 mHz tries 1 status C000009C"));balanced();
}
/* GitHub #45 review: high refresh above the DTO reference on an HBR3 x4 sink. 1080p360, 1440p240 and 1440p165 are
 * measured, each with the clock it asked for (display_timing_test.c runs the real measurement at these rates).
 * 720p600 is above what the measurement checks: the plan refuses it before any hardware step, it is not measured,
 * and it sets no dto-ref ceiling, so the other modes above the reference still set. */
static const BC250_TARGET_TIMING high_refresh[4]={
    {799920,1920,80,8,32,1080,31,3,8,0x1a},     /* 1080p360, total 2000x1111 */
    {966240,2560,80,8,32,1440,85,3,8,0x1a},     /* 1440p240, total 2640x1525 */
    {645100,2560,80,8,32,1440,41,3,8,0x1a},     /* 1440p165, total 2640x1481 (hpx32[3]) */
    {612000,1280,80,48,32,720,30,3,5,0x1a}};    /* 720p600, total 1360x750 */
static void dto_window(void)
{
    BC250_TARGET_TIMING previous;LINK_MODE mode;LINK_PLAN_REFUSAL refusal;unsigned i;
    static const ULONG hz[3]={799920000,966240000,645100000};
    for(i=0;i<3;++i) {
        hbr2_sink(BC250_SINK_DP);snapshot.Sink.Dpcd[DP_MAX_LINK_RATE]=0x1e;snapshot.Sink.MaxPixelClockKhz=1080000;
        CHECK(commit()==STATUS_SUCCESS);measure_hz=hz[i];measure_pixel_khz=0;
        CHECK(commit_timing(&high_refresh[i])==STATUS_SUCCESS && measure_calls==1);
        CHECK(measure_pixel_khz==high_refresh[i].PixelClockKhz && !link.PixelLimitKhz);
        CHECK(link.Current.Rate==(i==2?20u:30u));balanced();
    }
    hbr2_sink(BC250_SINK_DP);snapshot.Sink.Dpcd[DP_MAX_LINK_RATE]=0x1e;snapshot.Sink.MaxPixelClockKhz=1080000;
    device.RuntimeFramebufferLength=10240ull*1440u;
    Begin(&link,snapshot.Generation,FALSE);
    CHECK(Plan(&link,&snapshot.Sink,&high_refresh[3],&mode,&refusal)==STATUS_NOT_SUPPORTED);
    CHECK(refusal.Check==LINK_PLAN_DTO_WINDOW);
    End(&link);balanced();
    CHECK(commit()==STATUS_SUCCESS);previous=link.Current.Timing;
    memset(log_ring,0,sizeof(log_ring));
    CHECK(commit_timing(&high_refresh[3])==STATUS_NOT_SUPPORTED && !measure_calls && !link.PixelLimitKhz);
    CHECK(link.Active && Bc250TargetTimingEqual(&link.Current.Timing,&previous));
    CHECK(logged("connector-link: commit 1280x720 612000 kHz refused C00000BB check dto-window"));balanced();
    measure_hz=966240000;
    CHECK(commit_timing(&high_refresh[1])==STATUS_SUCCESS && measure_calls==1 && link.Current.Rate==30);balanced();
}
static void inherited_cold(unsigned kind,unsigned rate,const BC250_TARGET_TIMING* listed)
{
    hbr2_sink(kind);device.ConnectorLink=NULL;inherited_rate=rate;inherited_timing=hpx32[0];
    /* The sink's own caps stay valid; only the rate the firmware's link reads back with varies. */
    snapshot.Sink.Dpcd[DP_MAX_LINK_RATE]=(unsigned char)(rate==0x1e?0x1e:0x14);
    device.InheritedSignal.PixelRate=304250000;
    device.InheritedSignal.ActiveSize.cx=2560;device.InheritedSignal.ActiveSize.cy=1440;
    device.InheritedSignal.TotalSize.cx=2720;device.InheritedSignal.TotalSize.cy=1492;
    device.Post.Width=2560;device.Post.Height=1440;device.Post.Pitch=10240;
    if(listed) {snapshot.Sink.Targets[0]=*listed;snapshot.Sink.TargetCount=1;}
    memset(log_ring,0,sizeof(log_ring));
}
/* GitHub #45: a Philips 27E1N8900 (DPCD 1.2, HBR2 x4, 4K at 60 Hz) behind a firmware 4K at 30 Hz on an HBR2 link.
 * 1080p60, 4K30 and 4K60 are the CTA-861 VIC 16, 95 and 97 timings; 533.25 MHz is the report's preferred timing
 * (3840x2160 at 59.997 Hz, total 4000x2222, CVT reduced blanking). */
static const BC250_TARGET_TIMING philips[4]={
    {148500,1920,280,88,44,1080,45,4,5,0x1a},{297000,3840,560,176,88,2160,90,8,10,0x1a},
    {533250,3840,160,48,32,2160,62,3,5,0x1a},{594000,3840,560,176,88,2160,90,8,10,0x1a}};
static void philips_cold(unsigned lanes)
{
    unsigned i;
    reset(BC250_SINK_DP);device.ConnectorLink=NULL;
    snapshot.Sink.Dpcd[DP_MAX_LINK_RATE]=0x14;snapshot.Sink.Dpcd[DP_MAX_LANE_COUNT]=0xc4;
    /* connector.c: the HBR3 x4 ceiling, then Bc250DpMaxPixelClock of the sink's HBR2 x4. */
    snapshot.Sink.MaxPixelClockKhz=720000;
    for(i=0;i<4;++i)snapshot.Sink.Targets[i]=philips[i];
    snapshot.Sink.TargetCount=4;
    inherited_rate=0x14;inherited_lanes=lanes;inherited_timing=philips[1];
    device.InheritedSignal.PixelRate=297000000;
    device.InheritedSignal.ActiveSize.cx=3840;device.InheritedSignal.ActiveSize.cy=2160;
    device.InheritedSignal.TotalSize.cx=4400;device.InheritedSignal.TotalSize.cy=2250;
    device.Post.Width=3840;device.Post.Height=2160;device.Post.Pitch=15360;
    device.RuntimeFramebufferLength=15360ull*2160u;
    memset(log_ring,0,sizeof(log_ring));
}
static void philips_hbr2(void)
{
    unsigned lanes,steps,begin=checks,bad=failures;LINK_MODE mode;LINK_PLAN_REFUSAL refusal;
    for(lanes=2;lanes<=4;lanes+=2) {
        /* The start owns the firmware's HBR2 link (tester.32 refused it) and offers all four, 4K60 included. */
        philips_cold(lanes);
        CHECK(ConnectorLinkPrepare(&device,&snapshot.Sink,7)==STATUS_SUCCESS);balanced();
        CHECK(snapshot.Sink.TargetCount==4 && link.Current.Valid && link.Current.Inherited);
        CHECK(link.Rate==0x14 && link.Current.Rate==0x14 && link.Lanes==lanes && link.Current.Lanes==lanes);
        CHECK(logged("connector-link: target 3840x2160 533250 kHz offered rate 20 lanes 4"));
        CHECK(logged("connector-link: target 3840x2160 594000 kHz offered rate 20 lanes 4"));
        CHECK(logged("connector-link: offered 4 of 4 targets fastest 3840x2160 594000 kHz rate 20"));
        CHECK(logged("connector-link: prepare generation 7 targets 4 status 00000000"));
        CHECK(!logged("check link-settings"));
        /* 1080p60: the firmware's link goes off at its own symbol clock and lanes, then HBR trains as today. */
        CHECK(commit_timing(&philips[0])==STATUS_SUCCESS);balanced();
        CHECK(tx_off_khz==540000 && tx_off_lanes==lanes);
        CHECK(link.Active && link.Current.Rate==10 && link.Current.Lanes==4 && link.Training.EqPattern==2);
        /* 4K60 at 533.25 MHz: HBR x4 carries 360 MHz, so the lowest rate that carries it is HBR2, with TPS3. */
        CHECK(commit_timing(&philips[2])==STATUS_SUCCESS);balanced();
        CHECK(tx_off_khz==270000 && link.Current.Rate==20 && link.Training.FirstRate==20 && link.Training.EqPattern==3);
        CHECK(logged("connector-link: enabled generation 7 protocol 0 3840x2160 pitch 15360 pixel 533250 kHz lanes 4 rate 20"));
        CHECK(!measure_calls);      /* under the 600 MHz DP DTO reference: no refresh measurement */
        /* 4K30 at 297 MHz fits HBR: it trains at HBR, not at the HBR2 the firmware used. */
        CHECK(commit_timing(&philips[1])==STATUS_SUCCESS && tx_off_khz==540000 && link.Current.Rate==10);balanced();
        CHECK(ConnectorLinkRelease(&device)==STATUS_SUCCESS);balanced();
    }
    /* HBR2 does not lock on the first commit after the start: the firmware's 4K30 comes back at HBR, never black, and
     * the HBR2 timings are refused before any hardware step from then on. What HBR carries still sets. */
    philips_cold(2);
    CHECK(ConnectorLinkPrepare(&device,&snapshot.Sink,7)==STATUS_SUCCESS);balanced();
    memset(log_ring,0,sizeof(log_ring));train_fail=1;
    CHECK(commit_timing(&philips[2])==STATUS_IO_DEVICE_ERROR);balanced();
    CHECK(link.Active);CHECK(Bc250TargetTimingEqual(&link.Current.Timing,&philips[1]));
    /* The firmware's own surface is scanned again (connector_link.c Apply: no runtime POST for an inherited restore). */
    CHECK(link.Current.Inherited && !device.RuntimePostValid);
    CHECK(link.Current.Rate==10 && link.RateLimit==10 && !train_fail);
    CHECK(logged("connector-link: dp rate 20 failed for 533250 kHz; this sink trains at rate 10 or below until replugged"));
    memset(log_ring,0,sizeof(log_ring));steps=boundary_steps;
    CHECK(commit_timing(&philips[2])==STATUS_NOT_SUPPORTED && boundary_steps==steps);balanced();
    CHECK(logged("connector-link: commit 3840x2160 533250 kHz refused C00000BB check link-rate"));
    CHECK(commit_timing(&philips[3])==STATUS_NOT_SUPPORTED && boundary_steps==steps);balanced();
    CHECK(commit_timing(&philips[0])==STATUS_SUCCESS && link.Current.Rate==10 && trained_caps_rate==10);balanced();
    CHECK(ConnectorLinkRelease(&device)==STATUS_SUCCESS);balanced();
    /* The same refusal after a mode the driver set: 1080p60 stays on the screen. */
    philips_cold(4);
    CHECK(ConnectorLinkPrepare(&device,&snapshot.Sink,7)==STATUS_SUCCESS);
    CHECK(commit_timing(&philips[0])==STATUS_SUCCESS);train_fail=1;
    CHECK(commit_timing(&philips[3])==STATUS_IO_DEVICE_ERROR);balanced();
    CHECK(link.Active && Bc250TargetTimingEqual(&link.Current.Timing,&philips[0]) && link.RateLimit==10);
    CHECK(ConnectorLinkRelease(&device)==STATUS_SUCCESS);balanced();
    /* The same sink behind TMDS keeps the 300 MHz route: 4K30 plans, 4K60 is refused at the ceiling. */
    reset(BC250_SINK_HDMI);snapshot.Sink.MaxPixelClockKhz=300000;device.RuntimeFramebufferLength=15360ull*2160u;
    Begin(&link,snapshot.Generation,FALSE);
    CHECK(Plan(&link,&snapshot.Sink,&philips[0],&mode,&refusal)==STATUS_SUCCESS);
    CHECK(Plan(&link,&snapshot.Sink,&philips[1],&mode,&refusal)==STATUS_SUCCESS);
    CHECK(Plan(&link,&snapshot.Sink,&philips[2],&mode,&refusal)==STATUS_NOT_SUPPORTED && refusal.Check==LINK_PLAN_CLOCK_SINK);
    snapshot.Sink.MaxPixelClockKhz=600000;   /* an adapter that claims HDMI 2.0: the route still ends at 300 MHz */
    CHECK(Plan(&link,&snapshot.Sink,&philips[2],&mode,&refusal)==STATUS_NOT_SUPPORTED && refusal.Check==LINK_PLAN_CLOCK_CEILING);
    CHECK(Plan(&link,&snapshot.Sink,&philips[3],&mode,&refusal)==STATUS_NOT_SUPPORTED && refusal.Check==LINK_PLAN_CLOCK_CEILING);
    End(&link);balanced();
    /* An HBR x4 sink (the Lenovo's caps): 4K30 plans at HBR, 4K60 is the sink's own refusal. */
    reset(BC250_SINK_DP);snapshot.Sink.MaxPixelClockKhz=360000;device.RuntimeFramebufferLength=15360ull*2160u;
    Begin(&link,snapshot.Generation,FALSE);
    CHECK(Plan(&link,&snapshot.Sink,&philips[1],&mode,&refusal)==STATUS_SUCCESS);
    CHECK(Plan(&link,&snapshot.Sink,&philips[2],&mode,&refusal)==STATUS_NOT_SUPPORTED && refusal.Check==LINK_PLAN_CLOCK_SINK);
    snapshot.Sink.MaxPixelClockKhz=720000;   /* a wrong ceiling still meets the sink's own rate */
    CHECK(Plan(&link,&snapshot.Sink,&philips[2],&mode,&refusal)==STATUS_NOT_SUPPORTED && refusal.Check==LINK_PLAN_LINK_RATE);
    End(&link);balanced();
    /* Each refusal of the firmware's link names its check (tester.32 logged none of them). */
    philips_cold(4);inherited_timing_fail=1;
    CHECK(ConnectorLinkPrepare(&device,&snapshot.Sink,7)==STATUS_DEVICE_DATA_ERROR && !device.ConnectorLink);
    CHECK(logged("connector-link: inherited timing 297000000 Hz refused C000009C check otg-read"));balanced();
    philips_cold(4);device.Post.Width=800;device.Post.Height=600;device.Post.Pitch=3328;
    CHECK(ConnectorLinkPrepare(&device,&snapshot.Sink,7)==STATUS_NOT_SUPPORTED && !device.ConnectorLink);
    CHECK(logged("connector-link: inherited 3840x2160 total 4400x2250 post 800x600 pitch 3328 refused check post-size"));
    balanced();
    philips_cold(3);
    CHECK(ConnectorLinkPrepare(&device,&snapshot.Sink,7)==STATUS_NOT_SUPPORTED && !device.ConnectorLink);
    CHECK(logged("connector-link: inherited link rate 20 lanes 3 refused check link-settings"));balanced();
    /* Review: the same monitor behind an active DP-to-HDMI converter that states 340 MHz (sink_test.c decodes it).
     * The DP link is HBR2 x4, but the converter is the limit: 1080p60 and 4K30 are offered, both 4K60 timings are
     * the sink's refusal, and one line names the port and its cap. A native DP sink writes no such line. */
    philips_cold(4);
    CHECK(ConnectorLinkPrepare(&device,&snapshot.Sink,7)==STATUS_SUCCESS && !logged("downstream"));
    CHECK(ConnectorLinkRelease(&device)==STATUS_SUCCESS);balanced();
    philips_cold(4);snapshot.Sink.MaxPixelClockKhz=340000;
    snapshot.Sink.Dpcd[DP_DOWNSTREAMPORT_PRESENT]=0x15;snapshot.Sink.Dpcd[DP_DOWN_STREAM_PORT_COUNT]=1;
    snapshot.Sink.DownstreamRead=1;snapshot.Sink.DownstreamType=BC250_DP_DOWNSTREAM_HDMI;
    snapshot.Sink.DownstreamKhz=340000;
    CHECK(ConnectorLinkPrepare(&device,&snapshot.Sink,7)==STATUS_SUCCESS && snapshot.Sink.TargetCount==2);
    CHECK(logged("connector-link: sink generation 7 downstream hdmi ports 1 read 1 cap 340000 kHz"));
    CHECK(logged("connector-link: target 3840x2160 533250 kHz refused C00000BB check clock-sink"));
    CHECK(logged("connector-link: target 3840x2160 594000 kHz refused C00000BB check clock-sink"));
    CHECK(link.Current.Inherited && link.Current.Rate==0x14);
    memset(log_ring,0,sizeof(log_ring));steps=boundary_steps;
    CHECK(commit_timing(&philips[2])==STATUS_NOT_SUPPORTED && boundary_steps==steps);balanced();
    CHECK(logged("connector-link: commit 3840x2160 533250 kHz refused C00000BB check clock-sink"));
    CHECK(commit_timing(&philips[0])==STATUS_SUCCESS && link.Current.Rate==10);balanced();
    CHECK(ConnectorLinkRelease(&device)==STATUS_SUCCESS);balanced();
    /* A hub (DP downstream port) logs its port with no cap. */
    philips_cold(4);snapshot.Sink.Dpcd[DP_DOWNSTREAMPORT_PRESENT]=0x11;snapshot.Sink.Dpcd[DP_DOWN_STREAM_PORT_COUNT]=2;
    snapshot.Sink.DownstreamRead=1;snapshot.Sink.DownstreamType=BC250_DP_DOWNSTREAM_DP;
    CHECK(ConnectorLinkPrepare(&device,&snapshot.Sink,7)==STATUS_SUCCESS && snapshot.Sink.TargetCount==4);
    CHECK(logged("connector-link: sink generation 7 downstream dp ports 2 read 1 cap 0 kHz"));
    CHECK(ConnectorLinkRelease(&device)==STATUS_SUCCESS);balanced();
    printf("GitHub #45 Philips 4K on HBR2: %u checks, %u failures; inherited HBR2 x2/x4, 1080p60, 4K60, fallback, TMDS\n",
        checks-begin,failures-bad);
}
static void inherited_hbr2(void)
{
    BC250_TARGET_TIMING near=hpx32[0];
    /* BD-122: the tester's firmware timing on an HBR2 link. The connector owns it, and offers it next to the EDID. */
    inherited_cold(BC250_SINK_DP,0x14,&hpx32[3]);
    CHECK(ConnectorLinkPrepare(&device,&snapshot.Sink,7)==STATUS_SUCCESS);
    CHECK(link.Current.Valid && link.Current.Inherited && link.Rate==0x14 && link.Current.Rate==0x14 && link.Lanes==4);
    CHECK(snapshot.Sink.TargetCount==2 && Bc250TargetTimingEqual(&snapshot.Sink.Targets[1],&hpx32[0]));
    CHECK(logged("connector-link: inherited 2560x1440 304250 kHz total 2720x1492 offered, the EDID does not list it"));
    CHECK(ConnectorLinkRelease(&device)==STATUS_SUCCESS);balanced();
    /* The EDID lists it already, here 10 kHz apart: nothing is added. */
    near.PixelClockKhz=304260;inherited_cold(BC250_SINK_DP,0x14,&near);
    CHECK(ConnectorLinkPrepare(&device,&snapshot.Sink,7)==STATUS_SUCCESS && snapshot.Sink.TargetCount==1);
    CHECK(!logged("offered, the EDID does not list it"));
    CHECK(ConnectorLinkRelease(&device)==STATUS_SUCCESS);balanced();
    /* An HBR3 link is admitted as well; a rate the transmitter table does not take is refused by name. */
    inherited_cold(BC250_SINK_DP,0x1e,&hpx32[3]);
    CHECK(ConnectorLinkPrepare(&device,&snapshot.Sink,7)==STATUS_SUCCESS && link.Rate==0x1e);
    CHECK(ConnectorLinkRelease(&device)==STATUS_SUCCESS);balanced();
    inherited_cold(BC250_SINK_DP,0x15,&hpx32[3]);
    CHECK(ConnectorLinkPrepare(&device,&snapshot.Sink,7)==STATUS_NOT_SUPPORTED && !device.ConnectorLink);
    CHECK(logged("connector-link: inherited link rate 21 lanes 4 refused check link-settings"));
    balanced();
    /* On TMDS the 304.25 MHz timing stays above the route: the plan names the check, and the start keeps the
     * static inherited path (connector.c), which presents it as tester.30 did. */
    inherited_cold(BC250_SINK_HDMI,0x14,NULL);snapshot.Sink.MaxPixelClockKhz=600000;
    CHECK(ConnectorLinkPrepare(&device,&snapshot.Sink,7)==STATUS_NOT_SUPPORTED && !device.ConnectorLink);
    CHECK(logged("connector-link: inherited 2560x1440 304250 kHz refused C00000BB check clock-ceiling"));
    balanced();
}
int main(void)
{
    end_failure_propagation();
    // First mode commit is allowed to program a running pipe and surface, but
    // it neither grants OS visibility nor leaves a second DPG blank behind.
    {unsigned kind;for(kind=BC250_SINK_DP;kind<=BC250_SINK_DVI;++kind) {
        reset(kind);device.StartVisibilityPending=1;device.SourceVisible=FALSE;otg_blank=1;
        CHECK(commit()==STATUS_SUCCESS);balanced();
        CHECK(device.StartVisibilityPending && !device.SourceVisible && otg_blank);
        CHECK(link.Active && device.ModeActive && !plane_blank && !output_black);
    }}
    prepare_cleanup_preserved();
    programmed_clock_bound();
    plan_refusal_named();
    psp_session_controls();
    distinct_recovery_target();
    atom_write_decisions();
    failure_detail_scope();
    detached_recovery_record();
    diagnostic_wrap_replay();
    inherited_journal_controls();
    callback_error_retention();
    cold_retry_and_cancel();
    cold_recovery();
    cold_prepare();
    release_then_fresh_start();
    success();refusal();failure_sweep();rollback();cancellation_and_power();disable_suspend_resume();ownership_and_cleanup();retained_aux_journal();
    detach_before_apply();
    keep_breadcrumbs();
    release_handoff();
    hbr2_admission();hbr2_commit();hbr2_training_fallback();dto_check();dto_window();inherited_hbr2();
    philips_hbr2();
    printf("connector transaction: %u checks, %u failures (software boundary model)\n",checks,failures);
    return failures?1:0;
}



