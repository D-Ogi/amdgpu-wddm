/* Complete CLI -> KMT capture -> miniport dispatch model. Windows' thunk and
 * token/hardware endpoints are substituted; no dispatcher branch is removed. */
#include "bc250kmd_escape.h"
#ifdef BC250_TEST_OLD_REPLUG_COMMAND
#undef BC250_ESCAPE_CONNECTOR_REPLUG
#define BC250_ESCAPE_CONNECTOR_REPLUG 35u
#endif
#ifndef BC250_TEST_SKIP_COMMAND_CHECKS
#include "escape_commands.h"
#else
#include "wave_snapshot.h"
#include "../shim/include/bc250_hip_journal.h"
static void Bc250EscapeCommandsUnique(void) { }
#endif
#define main ConnectorHelperTests
#include "connector_replug_actual.c"
#undef main
#include <wchar.h>
typedef void* HANDLE;
typedef wchar_t WCHAR;
typedef unsigned int D3DKMT_HANDLE;
typedef struct {
    HANDLE hDevice;
    D3DDDI_ESCAPEFLAGS Flags;
    void* pPrivateDriverData;
    ULONG PrivateDriverDataSize;
    HANDLE hContext;
} DXGKARG_ESCAPE;
typedef struct {
    D3DKMT_HANDLE hAdapter,hDevice;
    int Type;
    D3DDDI_ESCAPEFLAGS Flags;
    void* pPrivateDriverData;
    unsigned PrivateDriverDataSize;
    D3DKMT_HANDLE hContext;
} D3DKMT_ESCAPE;
typedef struct { D3DKMT_HANDLE hAdapter; } D3DKMT_CLOSEADAPTER;
#define D3DKMT_ESCAPE_DRIVERPRIVATE 0
#define BC250_DEFAULT_HWID L"host fixture"
#define STATUS_DEVICE_BUSY ((NTSTATUS)0x80000011L)
#define LabEscapeMemory 0
#define LabEscapeRegister 1
#define LabEscapeGart 2
#define LabEscapePsp 3
#define LabEscapeGfx 4
#define LabEscapeFence 5
#define LabEscapeIh 6
#define LabEscapeDcn 7
#define LabEscapeDcnFlip 8
#define LabEscapeFbdump 9
#define LabEscapeSdma 10
static ULONG g_Presents;
static unsigned UnexpectedCalls,HipCalls,Opens,Closes,KmtCalls,CheckCliPayload;
static NTSTATUS LastKmtResult;
static void Unexpected(void) { ++UnexpectedCalls;CHECK(0); }
/* These endpoints must never run for the commands tested here. */
#define BoardProviderCaps(...) Unexpected()
#define WaveEscape(...) Unexpected()
#define BoardMemoryProbeRequest(...) Unexpected()
#define BoardMemoryRequest(...) Unexpected()
#define StartHealthRequest(...) Unexpected()
#define CuModeRequest(...) Unexpected()
#define DpmRequest(...) Unexpected()
#define DpmTuneRequest(...) Unexpected()
#define DpmCurveRequest(...) Unexpected()
#define HwmonRequest(...) Unexpected()
#define FanRequest(...) Unexpected()
#define InteropRequest(...) Unexpected()
#define DcnObserve(...) Unexpected()
#define DpAudioRequest(...) Unexpected()
#define CpuRequest(...) Unexpected()
#define SmuClockRequest(...) Unexpected()
#define VramEscape(...) Unexpected()
#define GartEscape(...) Unexpected()
#define PspEscape(...) Unexpected()
#define GfxEscape(...) Unexpected()
#define GfxFenceEscape(...) Unexpected()
#define IhEscape(...) Unexpected()
#define DcnEscape(...) Unexpected()
#define DcnFlipEscape(...) Unexpected()
#define FbdumpEscape(...) Unexpected()
#define SdmaCopyEscape(...) Unexpected()
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
/* BD-223: the lab latch probe has a fixed 480-byte packet (dcn_latch_escape.h). Only its size matters to the
 * dispatcher, and its handler never runs for the commands tested here. */
typedef struct { unsigned char Bytes[480]; } BC250_ESCAPE_LATCH_PROBE;
#define LatchProbeEscape(...) Unexpected()
#endif
#define MmioWrite(...) (Unexpected(),STATUS_ACCESS_DENIED)
#define MmioRead(...) (Unexpected(),STATUS_ACCESS_DENIED)
#define GuardLastStage() (Unexpected(),0)
#define WddmCounters(...) Unexpected()
#define CallerIsAdmin() Admin
static BOOLEAN SoftwareReadEscape(ULONG command)
{ return command==BC250_ESCAPE_GET_LOG || command==BC250_ESCAPE_GET_PAGING_JOURNAL; }
static NTSTATUS LogEscape(BC250_DEVICE* d,const DXGKARG_ESCAPE* e,BOOLEAN summary)
{ (void)d;(void)e;(void)summary;Unexpected();return STATUS_ACCESS_DENIED; }
static NTSTATUS PagingJournalEscape(const BC250_DEVICE* d,const DXGKARG_ESCAPE* e)
{ (void)d;(void)e;Unexpected();return STATUS_ACCESS_DENIED; }
static NTSTATUS HipJournalEscape(void* d,const DXGKARG_ESCAPE* e)
{
    const BC250_HIP_JOURNAL_UPLOAD* p=e->pPrivateDriverData;
    CHECK(d==DeviceSeen && p!=NULL && p->Command==BC250_HIP_JOURNAL_COMMAND);
    ++HipCalls;
    /* The production HIP entry rejects a request shorter than its 80-byte
     * header before it changes the payload. Later context admission is mocked. */
    if(e->PrivateDriverDataSize<sizeof(*p))return STATUS_INVALID_PARAMETER;
    return STATUS_ACCESS_DENIED;
}

/* ACTUAL_DISPATCH */

static NTSTATUS D3DKMTEscape(const D3DKMT_ESCAPE* e)
{
    DXGKARG_ESCAPE k={0};
    CHECK(e->hAdapter==17 && e->Type==D3DKMT_ESCAPE_DRIVERPRIVATE);
    CHECK(e->hDevice==0 && e->hContext==0);
    ++KmtCalls;
    if(CheckCliPayload) {
        const BC250_ESCAPE_CONNECTOR_REPLUG_DATA* p=e->pPrivateDriverData;
        unsigned i;
        CHECK(e->PrivateDriverDataSize==sizeof(*p) && e->Flags.Value==8);
        CHECK(p->Magic==BC250_ESCAPE_MAGIC && p->Command==BC250_ESCAPE_CONNECTOR_REPLUG);
        CHECK(p->AbiVersion==BC250_CONNECTOR_REPLUG_ABI && p->Status==BC250_ESCAPE_STATUS_UNKNOWN_COMMAND);
        CHECK(!p->Version && !p->NtStatus && !p->Generation && !p->DpcQueued);
        for(i=0;i<RTL_NUMBER_OF(p->Reserved);++i)CHECK(!p->Reserved[i]);
    }
    k.pPrivateDriverData=e->pPrivateDriverData;k.PrivateDriverDataSize=e->PrivateDriverDataSize;
    k.Flags=e->Flags;
    LastKmtResult=Bc250Escape(DeviceSeen,&k);
    return LastKmtResult;
}
static int OpenAdapterById(const WCHAR* id,D3DKMT_HANDLE* handle,NTSTATUS* result)
{ CHECK(!wcscmp(id,BC250_DEFAULT_HWID));++Opens;*handle=17;*result=STATUS_SUCCESS;return 0; }
static void D3DKMTCloseAdapter(const D3DKMT_CLOSEADAPTER* p) { CHECK(p->hAdapter==17);++Closes; }
static void PrintStatus(const char* what,NTSTATUS status) { (void)what;(void)status; }

/* ACTUAL_CLI */

static void ResetDispatch(BC250_DEVICE* d,BC250_ESCAPE_CONNECTOR_REPLUG_DATA* p)
{
    Reset(d,p);HipCalls=UnexpectedCalls=Opens=Closes=KmtCalls=CheckCliPayload=0;
    LastKmtResult=STATUS_DEVICE_BUSY;
}
static NTSTATUS Send(BC250_ESCAPE_CONNECTOR_REPLUG_DATA* p,unsigned size,unsigned flags)
{
    D3DKMT_ESCAPE e={0};
    e.hAdapter=17;e.Type=D3DKMT_ESCAPE_DRIVERPRIVATE;e.Flags.Value=flags;
    e.pPrivateDriverData=p;e.PrivateDriverDataSize=size;
    return D3DKMTEscape(&e);
}
static void NoReplug(const BC250_DEVICE* d)
{
    CHECK(!Queues && !WorkQueues && !Markers && d->Connector.Generation==19 && !d->Connector.Pending);
    CHECK(!UnexpectedCalls && !HipCalls);
}
int main(void)
{
    BC250_DEVICE d;BC250_ESCAPE_CONNECTOR_REPLUG_DATA p;unsigned i;int cli;
    (void)ConnectorHelperTests();
    ResetDispatch(&d,&p);CheckCliPayload=1;cli=CliConnectorReplug();
    CHECK(Opens==1 && Closes==1 && KmtCalls==1 && !HipCalls && !UnexpectedCalls);
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
    CHECK(cli==0 && LastKmtResult==STATUS_SUCCESS && Queues==1 && Markers==1);
    CHECK(d.Connector.Pending==1 && d.Connector.Generation==20);
    ConnectorDpc(&d);CHECK(WorkQueues==1);
    ResetDispatch(&d,&p);CHECK(EscapeOn(17,&p,sizeof(p),1)==STATUS_SUCCESS);
    CHECK(p.Status==BC250_ESCAPE_STATUS_DONE && Queues==1 && !HipCalls);
    ResetDispatch(&d,&p);Admin=FALSE;
    CHECK(Send(&p,sizeof(p),8)==STATUS_SUCCESS && (NTSTATUS)p.NtStatus==STATUS_ACCESS_DENIED);NoReplug(&d);
    ResetDispatch(&d,&p);LabEnabled=FALSE;
    CHECK(Send(&p,sizeof(p),8)==STATUS_SUCCESS && (NTSTATUS)p.NtStatus==STATUS_ACCESS_DENIED);NoReplug(&d);
    ResetDispatch(&d,&p);p.AbiVersion++;
    CHECK(Send(&p,sizeof(p),8)==STATUS_SUCCESS && (NTSTATUS)p.NtStatus==STATUS_INVALID_PARAMETER);NoReplug(&d);
    for(i=0;i<8;++i) {
        ResetDispatch(&d,&p);p.Reserved[i]=1;
        CHECK(Send(&p,sizeof(p),8)==STATUS_SUCCESS && (NTSTATUS)p.NtStatus==STATUS_INVALID_PARAMETER);NoReplug(&d);
    }
    for(i=0;i<32;++i)if(i!=8) {
        ResetDispatch(&d,&p);
        CHECK(Send(&p,sizeof(p),i)==STATUS_SUCCESS && (NTSTATUS)p.NtStatus==STATUS_INVALID_PARAMETER);NoReplug(&d);
    }
    ResetDispatch(&d,&p);CHECK(Send(&p,65,8)==STATUS_INVALID_PARAMETER);NoReplug(&d);
#else
    CHECK(cli!=0 && LastKmtResult==STATUS_DEVICE_NOT_READY);NoReplug(&d);
    for(i=0;i<32;++i) {
        ResetDispatch(&d,&p);CHECK(!NT_SUCCESS(Send(&p,sizeof(p),i)));NoReplug(&d);
    }
#endif
    ResetDispatch(&d,&p);p.Magic^=1;CHECK(Send(&p,sizeof(p),8)==STATUS_INVALID_PARAMETER);NoReplug(&d);
    ResetDispatch(&d,&p);CHECK(Send(&p,63,8)==STATUS_INVALID_PARAMETER);NoReplug(&d);
    ResetDispatch(&d,&p);CHECK(Send(NULL,64,8)==STATUS_INVALID_PARAMETER);NoReplug(&d);
    {
        BC250_HIP_JOURNAL_UPLOAD h={0};
        ResetDispatch(&d,&p);h.Magic=BC250_HIP_JOURNAL_MAGIC;h.Command=BC250_HIP_JOURNAL_COMMAND;
        CHECK(EscapeOn(17,&h,sizeof(h),1)==STATUS_ACCESS_DENIED && HipCalls==1 && !Queues && !UnexpectedCalls);
        /* The legacy collision remains a concrete negative control: the HIP
         * command with replug-sized bytes must still reach HIP and be refused. */
        h.Command=BC250_HIP_JOURNAL_COMMAND;
        CHECK(EscapeOn(17,&h,64,1)==STATUS_INVALID_PARAMETER && HipCalls==2 && !Queues);
    }
    printf("connector dispatch: %u checks, %u failures\n",Checks,Failures);
    return Failures ? 1 : 0;
}
