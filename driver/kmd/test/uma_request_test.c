/* CPU-only test: compile the unchanged BoardMemoryRequest with a minimal kernel boundary. */
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "bc250kmd_escape.h"
#define STATUS_SUCCESS ((NTSTATUS)0)
#ifndef STATUS_INVALID_PARAMETER
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000DL)
#endif
#define STATUS_ACCESS_DENIED ((NTSTATUS)0xC0000022L)
#define STATUS_NOT_SUPPORTED ((NTSTATUS)0xC00000BBL)
typedef struct {
    volatile LONG64 UmaActiveBytes;
    volatile LONG RetainedPowerPhase;
    volatile LONG BoardMemoryProviderId;
    ULONG BoardMemoryReason;
} BC250_DEVICE;
typedef struct BC250_BOARD_MEMORY_PROVIDER {
    void (*Query)(BC250_DEVICE*, BC250_ESCAPE_BOARD_MEMORY*);
    NTSTATUS (*Set)(BC250_DEVICE*, const BC250_ESCAPE_BOARD_MEMORY*);
    NTSTATUS (*Restore)(BC250_DEVICE*, const BC250_ESCAPE_BOARD_MEMORY*);
    void (*Probe)(BC250_DEVICE*, BC250_ESCAPE_BOARD_MEMORY_PROBE*, BOOLEAN, ULONG);
} BC250_BOARD_MEMORY_PROVIDER;
const BC250_BOARD_MEMORY_PROVIDER* BoardMemoryProvider(BC250_DEVICE* Device);
void BoardMemoryIdentityClear(BC250_DEVICE* Device);
void BoardMemoryIdentityCapture(BC250_DEVICE* Device);
void AblBoardMemoryProbeRequest(BC250_DEVICE*, BC250_ESCAPE_BOARD_MEMORY_PROBE*, BOOLEAN, ULONG);

void AblBoardMemoryProbeRequest(BC250_DEVICE* d, BC250_ESCAPE_BOARD_MEMORY_PROBE* q, BOOLEAN a, ULONG f)
{ (void)d; (void)q; (void)a; (void)f; }
/* ACTUAL_UMA */
static unsigned checks, failures;
static void check(int ok,unsigned line,const char* text) { ++checks;if(!ok){++failures;printf("FAIL CHECK line %u: %s\n",line,text);} }
#define CHECK(x) check(!!(x),__LINE__,#x)
static BC250_ESCAPE_BOARD_MEMORY request(ULONG op)
{
 BC250_ESCAPE_BOARD_MEMORY q; memset(&q,0,sizeof(q)); q.Magic=BC250_ESCAPE_MAGIC;
 q.Command=BC250_ESCAPE_RUN_BOARD_MEMORY;q.Status=BC250_ESCAPE_STATUS_UNKNOWN_COMMAND;
 q.AbiVersion=BC250_BOARD_MEMORY_ABI;q.Op=op;return q;
}
static void expected(BC250_DEVICE* d, BC250_ESCAPE_BOARD_MEMORY* q, BOOLEAN admin, ULONG flags, NTSTATUS status, ULONG result)
{
 BC250_ESCAPE_BOARD_MEMORY want; ULONG op=q->Op;memset(&want,0,sizeof(want));
 want.Magic=BC250_ESCAPE_MAGIC;want.Command=BC250_ESCAPE_RUN_BOARD_MEMORY;want.AbiVersion=BC250_BOARD_MEMORY_ABI;
 want.Op=op;want.Status=result;want.NtStatus=(ULONG)status;want.Reason=d->BoardMemoryProviderId ? BC250_BOARD_MEMORY_REASON_NO_TRANSPORT : d->BoardMemoryReason;
 if(status==0 && d->BoardMemoryProviderId){
 want.ProviderId=1;want.AllowedMiB[0]=8192;want.AllowedMiB[1]=12288;want.NeedsRestart=1;
 want.Flags=BC250_BOARD_MEMORY_SUPPORTED;want.ActiveBytes=(ULONGLONG)d->UmaActiveBytes;
 if(want.ActiveBytes)want.Flags|=BC250_BOARD_MEMORY_ACTIVE_VALID;}
 BoardMemoryRequest(d,q,admin,flags);CHECK(memcmp(q,&want,sizeof(want))==0);
}
int main(void)
{
 BC250_DEVICE d={0};BC250_ESCAPE_BOARD_MEMORY q;D3DDDI_ESCAPEFLAGS sw={0},hw={0};unsigned i;
 sw.NoAdapterSynchronization=1;hw.HardwareAccess=1;
 CHECK(sizeof(q)==128);CHECK(offsetof(BC250_ESCAPE_BOARD_MEMORY,ActiveBytes)==32);
 CHECK(offsetof(BC250_ESCAPE_BOARD_MEMORY,RequestedMiB)==40);CHECK(offsetof(BC250_ESCAPE_BOARD_MEMORY,ObservedBlock)==48);
 CHECK(offsetof(BC250_ESCAPE_BOARD_MEMORY,ResultCode)==76);CHECK(offsetof(BC250_ESCAPE_BOARD_MEMORY,Reserved)==96);
 q=request(0);expected(&d,&q,FALSE,sw.Value,0,BC250_ESCAPE_STATUS_DONE);
 d.UmaActiveBytes=12ll<<30;
 d.BoardMemoryReason=BC250_BOARD_MEMORY_REASON_BOARD;q=request(0);expected(&d,&q,FALSE,sw.Value,0,BC250_ESCAPE_STATUS_DONE);
 d.BoardMemoryReason=BC250_BOARD_MEMORY_REASON_FIRMWARE;q=request(0);expected(&d,&q,FALSE,sw.Value,0,BC250_ESCAPE_STATUS_DONE);
 d.BoardMemoryProviderId=1;q=request(0);expected(&d,&q,FALSE,sw.Value,0,BC250_ESCAPE_STATUS_DONE);
 /* Every output-only field, reserved word and observed byte is rejected and cleared. */
 for(i=0;i<128;i++) {
  if(i<24 || (i>=40 && i<44))continue;
  q=request(0);((unsigned char*)&q)[i]=0xA5;
  expected(&d,&q,TRUE,sw.Value,STATUS_INVALID_PARAMETER,BC250_ESCAPE_STATUS_REFUSED);
 }
 q=request(0);q.NtStatus=1;expected(&d,&q,TRUE,sw.Value,STATUS_INVALID_PARAMETER,BC250_ESCAPE_STATUS_REFUSED);
 q=request(0);q.Magic^=1;expected(&d,&q,TRUE,sw.Value,STATUS_INVALID_PARAMETER,BC250_ESCAPE_STATUS_REFUSED);
 q=request(0);q.Command++;expected(&d,&q,TRUE,sw.Value,STATUS_INVALID_PARAMETER,BC250_ESCAPE_STATUS_REFUSED);
 q=request(0);q.AbiVersion++;expected(&d,&q,TRUE,sw.Value,STATUS_INVALID_PARAMETER,BC250_ESCAPE_STATUS_REFUSED);
 q=request(0);q.Status=BC250_ESCAPE_STATUS_DONE;expected(&d,&q,TRUE,sw.Value,STATUS_INVALID_PARAMETER,BC250_ESCAPE_STATUS_REFUSED);
 q=request(0);q.RequestedMiB=8192;expected(&d,&q,TRUE,sw.Value,STATUS_INVALID_PARAMETER,BC250_ESCAPE_STATUS_REFUSED);
 q=request(3);expected(&d,&q,TRUE,hw.Value,STATUS_INVALID_PARAMETER,BC250_ESCAPE_STATUS_REFUSED);
 for(i=0;i<32;i++){ULONG f=1u<<i;if(f==sw.Value)continue;q=request(0);expected(&d,&q,TRUE,f,STATUS_INVALID_PARAMETER,BC250_ESCAPE_STATUS_REFUSED);}
 q=request(0);expected(&d,&q,TRUE,0,STATUS_INVALID_PARAMETER,BC250_ESCAPE_STATUS_REFUSED);
 q=request(0);expected(&d,&q,TRUE,sw.Value|hw.Value,STATUS_INVALID_PARAMETER,BC250_ESCAPE_STATUS_REFUSED);
 for(i=1;i<=2;i++) {
  q=request(i);q.RequestedMiB=i==1?8192:0;memset(q.ObservedBlock,0x52,28);
  expected(&d,&q,TRUE,hw.Value,STATUS_NOT_SUPPORTED,BC250_ESCAPE_STATUS_REFUSED);
  q=request(i);q.RequestedMiB=i==1?12288:0;
  expected(&d,&q,FALSE,hw.Value,STATUS_ACCESS_DENIED,BC250_ESCAPE_STATUS_NOT_ADMIN);
  q=request(i);q.RequestedMiB=14336;expected(&d,&q,TRUE,hw.Value,STATUS_INVALID_PARAMETER,BC250_ESCAPE_STATUS_REFUSED);
  q=request(i);q.RequestedMiB=i==1?8192:0;expected(&d,&q,TRUE,sw.Value,STATUS_INVALID_PARAMETER,BC250_ESCAPE_STATUS_REFUSED);
 }
 printf("UMA request: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
