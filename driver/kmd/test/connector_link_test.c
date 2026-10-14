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
static unsigned apply_count, tx_enabled, plane_blank, otg_blank, crtc_enabled, output_black, aux_open;
static unsigned persistent_aux_fail, firmware_frees, pool_frees, audio_debt;
static unsigned observed_generation, fault_triggered;
/* 1 = inherited firmware surface; 2 = reserved runtime surface. */
static unsigned scanned_surface, original_surface_restores;
static BC250_TARGET_TIMING inherited_timing;
static unsigned inherited_timing_fail;
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
    if(primary_end_fail)return STATUS_IO_DEVICE_ERROR;
    last_invalidate=changed;if(changed)scanout_object=0;--primary_locks;++primary_exits;
    return STATUS_SUCCESS;
}
static LONG InterlockedExchange(volatile LONG* p,LONG v) { LONG old=*p;*p=v;return old; }
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
static NTSTATUS DcnSetVisibility(BC250_DEVICE*,BOOLEAN);
static NTSTATUS DcnRuntimeSurface(BC250_DEVICE*,BOOLEAN);
NTSTATUS ConnectorLinkRelease(BC250_DEVICE*);
static NTSTATUS DcnRestorePostDisplay(BC250_DEVICE*);
static long boundary_step(void*,const char*,int);
static BC250_LINK_SURFACE last_restored;
static ULONG captured_visible;
NTSTATUS DcnLinkSurfaceCapture(BC250_DEVICE* d,ULONG w,ULONG h,BC250_LINK_SURFACE* surface) {
    long status=boundary_step(d->ConnectorLink,"capture-surface",0);
    CHECK(primary_locks && scanned_surface==2 && w==1920 && h==1080);
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
    CHECK(in->HActive==1920 && in->PitchPixels==1920);
    CHECK(in->DchubRefClockKhz==48000 && in->DentistVcoKhz==3000000);
    CHECK(in->CursorWidth==0 && in->AudioSampleRateKhz==192);
    CHECK(in->Output==(unsigned)(snapshot.Sink.Kind==BC250_SINK_DP?BC250_DML201_DP:BC250_DML201_HDMI));
    memset(p,0,sizeof(*p));if(plan_fail)return -1;
    p->Valid=1;p->VReady=340;p->VStartup=20;p->VUpdateOffset=550;p->VUpdateWidth=255;return 0;
}
int Bc250OptcTimingValid(const BC250_TARGET_TIMING* t,const BC250_OPTC_SYNC* s)
{ CHECK(t!=NULL && s->VStartup==20);return 1; }
long Bc250ScalerPlan(unsigned long sw,unsigned long sh,unsigned long tw,unsigned long th,unsigned long scaling,BC250_SCALER_PLAN* p)
{ CHECK(sw==tw && sh==th && scaling==BC250_SCALING_IDENTITY);memset(p,0,sizeof(*p));p->SourceWidth=sw;p->SourceHeight=sh;return 0; }
long Bc250Dcn201ReadClocks(BC250_DISP_IO* io,unsigned crystal,BC250_DCN201_CLOCKS* c)
{
    long s=io_step(io,"clocks");++clock_reads;if(s)return s;CHECK(crystal==27000);
    memset(c,0,sizeof(*c));c->CrystalClockKhz=crystal;c->DchubRefClockKhz=48000;
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
{ long s=io_step(io,"pipe");CHECK(plane_blank && !crtc_enabled && p->Valid && c->DentistVcoKhz==3000000);++apply_count;return s; }
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
{ CHECK(aux_open && address && length);memset(data,0,length);if(address==DP_LINK_BW_SET && length==2){data[0]=10;data[1]=4;}return io_step(a->Io,"aux-read"); }
long Bc250AuxTransfer(BC250_AUX_SESSION* a,unsigned long action,unsigned long address,unsigned char* data,unsigned long length,unsigned long* got)
{ CHECK(aux_open && action==BC250_AUX_DP_WRITE && address && data && length);*got=length;return io_step(a->Io,"aux-write"); }
long Bc250DpPhyConfigure(BC250_DISP_IO* io,unsigned lanes,int enhanced)
{ CHECK(lanes==4 && enhanced==1);return io_step(io,"phy"); }
long Bc250DpPhyPattern(BC250_DISP_IO* io,unsigned pattern,int enhanced)
{ CHECK(pattern<=2 && enhanced==1);return io_step(io,"pattern"); }
long Bc250DpPhyDisableTraining(BC250_DISP_IO* io) { return io_step(io,"phy-off"); }
enum BC250_DP_TRAIN_STATUS Bc250DpTrain(const BC250_DP_TRAIN_IO* io,const uint8_t caps[16],uint32_t khz,BC250_DP_TRAIN_RESULT* r)
{
    unsigned char data[6]={0};
    CHECK(aux_open && caps[DP_MAX_LANE_COUNT]==4 && khz);
    memset(r,0,sizeof(*r));r->Rate=10;r->Lanes=4;r->Enhanced=1;
    if(!io->Current(io->Context) || io->Enable(io->Context,10,4,1) ||
       io->Write(io->Context,DP_LINK_BW_SET,data,2) || io->Pattern(io->Context,1) ||
       io->Drive(io->Context,0,0) || io->DelayUs(io->Context,400) ||
       io->Read(io->Context,DP_LANE0_1_STATUS,data,6) || io->Pattern(io->Context,0)) {
        (void)io->Disable(io->Context);r->Status=BC250_DP_TRAIN_IO_ERROR;return r->Status;
    }
    return BC250_DP_TRAIN_OK;
}
BC250_ATOM_STATUS Bc250LinkAtomStream(BC250_LINK_ATOM* a,unsigned protocol,unsigned khz,unsigned lanes,unsigned rate)
{ CHECK(khz && lanes==4 && (protocol==0?rate==10:rate==0));return step(a->Io.Context,"atom-stream")?BC250_ATOM_IO:BC250_ATOM_OK; }
int Bc250HdmiStreamProgram(BC250_HDMI_LINK* h,const BC250_ATOM_CALLBACKS* io,uint32_t khz,unsigned w,unsigned height,unsigned ht,unsigned vt)
{ long s;h->Io=*io;h->Dirty=1;CHECK(khz && w<ht && height<vt && !tx_enabled && !crtc_enabled);s=step(io->Context,"hdmi-program");return s?1:0; }
long Bc250OptcEnable(BC250_DISP_IO* io)
{ long s=io_step(io,"otg-on");CHECK(plane_blank && output_black && tx_enabled);if(!s)crtc_enabled=1;return s; }
static NTSTATUS DcnRuntimeSurface(BC250_DEVICE* d,BOOLEAN unblank)
{ long s=boundary_step(d->ConnectorLink,unblank?"surface":"suspend-surface",0);CHECK(primary_locks);if(unblank)CHECK(crtc_enabled && d->RuntimePostValid);if(!s) {scanned_surface=2;plane_blank=0;d->RuntimePlaneBlank=0;if(unblank)otg_blank=0;}return s; }
static NTSTATUS DcnRestorePostDisplay(BC250_DEVICE* d)
{
    long status=boundary_step(d->ConnectorLink,"original-surface",0);
    CHECK(primary_locks && crtc_enabled);
    if(!status) {scanned_surface=1;plane_blank=0;otg_blank=0;++original_surface_restores;}
    return status;
}
long Bc250DpStreamUnblank(BC250_DISP_IO* io,unsigned khz,unsigned rate)
{ CHECK(crtc_enabled && !plane_blank && khz && rate==10);return io_step(io,"dp-unblank"); }
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
    apply_count=plane_blank=otg_blank=output_black=aux_open=event_count=fault_triggered=0;
    persistent_aux_fail=firmware_frees=pool_frees=audio_debt=0;
    scanned_surface=1;original_surface_restores=0;
    tx_enabled=crtc_enabled=1;observed_generation=7;clock_us=100;thread=(PKTHREAD)(uintptr_t)1;
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
    reset(BC250_SINK_HDMI);CHECK(ConnectorLinkSuspend(&device)==0 && link.Suspended);balanced();
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
static void end_failure_propagation(void)
{
    unsigned arm,begin=checks,bad=failures;
    // The production End preserves its writer state when synchronization
    // fails. Each real caller must surface that failure and release only its
    // unrelated PASSIVE locks. It must not start audio after a failed End.
    for(arm=0;arm<6;++arm) {
        reset(BC250_SINK_HDMI);
        if(arm==3 || arm==4)CHECK(commit()==STATUS_SUCCESS);
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
    LINK_MODE mode;BC250_TARGET_TIMING timing;
    reset(BC250_SINK_HDMI);snapshot.Sink.MaxPixelClockKhz=600000;
    Begin(&link,snapshot.Generation,FALSE);
    timing=snapshot.Sink.Targets[0];timing.PixelClockKhz=304250;
    CHECK(Plan(&link,&snapshot.Sink,&timing,&mode)==STATUS_NOT_SUPPORTED);
    CHECK(!mode.Valid && !plan_calls && !clock_reads);
    timing.PixelClockKhz=300001;
    CHECK(Plan(&link,&snapshot.Sink,&timing,&mode)==STATUS_NOT_SUPPORTED);
    CHECK(!mode.Valid && !plan_calls && !clock_reads);
    timing.PixelClockKhz=300000;
    CHECK(Plan(&link,&snapshot.Sink,&timing,&mode)==STATUS_SUCCESS && mode.Valid);
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
int main(void)
{
    end_failure_propagation();
    prepare_cleanup_preserved();
    programmed_clock_bound();
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
    printf("connector transaction: %u checks, %u failures (software boundary model)\n",checks,failures);
    return failures?1:0;
}



