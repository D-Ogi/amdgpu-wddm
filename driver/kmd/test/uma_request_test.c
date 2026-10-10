/* CPU-only test: compile the unchanged UmaRequest with a minimal kernel boundary. */
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
typedef struct { volatile LONG64 UmaActiveBytes; } BC250_DEVICE;
/* ACTUAL_UMA */
static unsigned checks, failures;
static void check(int ok,unsigned line,const char* text) { ++checks;if(!ok){++failures;printf("FAIL CHECK line %u: %s\n",line,text);} }
#define CHECK(x) check(!!(x),__LINE__,#x)
static BC250_ESCAPE_UMA request(ULONG op)
{
 BC250_ESCAPE_UMA q; memset(&q,0,sizeof(q)); q.Magic=BC250_ESCAPE_MAGIC;
 q.Command=BC250_ESCAPE_RUN_UMA;q.Status=BC250_ESCAPE_STATUS_UNKNOWN_COMMAND;
 q.AbiVersion=BC250_UMA_ABI;q.Op=op;return q;
}
static void expected(BC250_DEVICE* d, BC250_ESCAPE_UMA* q, BOOLEAN admin, ULONG flags, NTSTATUS status, ULONG result)
{
 BC250_ESCAPE_UMA want; ULONG op=q->Op;memset(&want,0,sizeof(want));
 want.Magic=BC250_ESCAPE_MAGIC;want.Command=BC250_ESCAPE_RUN_UMA;want.AbiVersion=BC250_UMA_ABI;
 want.Op=op;want.Status=result;want.NtStatus=(ULONG)status;want.Reason=BC250_UMA_REASON_NO_TRANSPORT;
 if(status==0){want.ActiveBytes=(ULONGLONG)d->UmaActiveBytes;if(want.ActiveBytes)want.Flags=BC250_UMA_ACTIVE_VALID;}
 UmaRequest(d,q,admin,flags);CHECK(memcmp(q,&want,sizeof(want))==0);
}
int main(void)
{
 BC250_DEVICE d={0};BC250_ESCAPE_UMA q;D3DDDI_ESCAPEFLAGS sw={0},hw={0};unsigned i;
 sw.NoAdapterSynchronization=1;hw.HardwareAccess=1;
 CHECK(sizeof(q)==128);CHECK(offsetof(BC250_ESCAPE_UMA,ActiveBytes)==32);
 CHECK(offsetof(BC250_ESCAPE_UMA,RequestedMiB)==40);CHECK(offsetof(BC250_ESCAPE_UMA,ObservedBlock)==48);
 CHECK(offsetof(BC250_ESCAPE_UMA,ResultCode)==76);CHECK(offsetof(BC250_ESCAPE_UMA,Reserved)==80);
 q=request(0);expected(&d,&q,FALSE,sw.Value,0,BC250_ESCAPE_STATUS_DONE);
 d.UmaActiveBytes=12ll<<30;q=request(0);expected(&d,&q,FALSE,sw.Value,0,BC250_ESCAPE_STATUS_DONE);
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
