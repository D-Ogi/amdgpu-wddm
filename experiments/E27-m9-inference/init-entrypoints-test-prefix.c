#include <stdio.h>
#include <string.h>
#include "bc250kmd_escape.h"
#define BC250_GFX_STAGE_INTERRUPTS 8
#define PASSIVE_LEVEL 0
#define STATUS_SUCCESS 0L
#define STATUS_INVALID_PARAMETER (-1L)
#define STATUS_INVALID_DEVICE_STATE (-2L)
#define STATUS_DEVICE_HARDWARE_ERROR (-3L)
#define STATUS_IO_DEVICE_ERROR (-4L)
#define STATUS_DEVICE_NOT_READY (-5L)
#define NT_SUCCESS(x) ((x)>=0)
#define RtlZeroMemory(p,n) memset(p,0,n)
typedef long NTSTATUS;
typedef unsigned long ULONG;
typedef int BOOLEAN;
static int keepLog,snapshots;
static ULONG GuardReadSetting(const void*n,ULONG d){(void)n;(void)d;return keepLog;}
static void GuardLog(const char*f,...){(void)f;}
static void GuardLogKeep(void){snapshots++;}
typedef struct {int GpuStopUnconfirmed;} BC250_DEVICE;
static int mode,irql,calls,badInput,checks,failures;
static int KeGetCurrentIrql(void){return irql;}
static void result(unsigned long*s,unsigned long*n,long*r)
{
 *s=BC250_ESCAPE_STATUS_DONE;*n=0;*r=0;
 if(mode==1){*n=(unsigned long)STATUS_DEVICE_NOT_READY;*s=BC250_ESCAPE_STATUS_REFUSED;}
 if(mode==2){*r=-62;*s=BC250_ESCAPE_STATUS_REFUSED;}
 if(mode==3)*s=BC250_ESCAPE_STATUS_REFUSED;
 if(mode==5)*n=1;
 if(mode==6)*n=(unsigned long)STATUS_DEVICE_NOT_READY;
}
static void GartExecute(BC250_DEVICE*d,BC250_ESCAPE_GART*p)
{
 (void)d;calls++;
 if(p->Op!=BC250_GART_OP_ENABLE || p->Magic!=BC250_ESCAPE_MAGIC || p->Command!=BC250_ESCAPE_RUN_GART || p->WriteCount)badInput++;
 p->WriteCount=19;p->State=mode==4?0:BC250_GART_STATE_ENABLED;result(&p->Status,&p->NtStatus,&p->Result);
}
static void PspExecute(BC250_DEVICE*d,BC250_ESCAPE_PSP*p)
{
 (void)d;calls++;
 if(p->Op!=BC250_PSP_OP_LOAD || p->Magic!=BC250_ESCAPE_MAGIC || p->Command!=BC250_ESCAPE_RUN_PSP || p->WriteCount)badInput++;
 p->WriteCount=19;p->State=mode==4?0:7;p->CommandCount=2;p->CommandsDone=2;
 if(mode==7)p->CommandsDone=1;
 if(mode==8)p->CommandCount=p->CommandsDone=0;
 result(&p->Status,&p->NtStatus,&p->Result);
}
static void IhExecute(BC250_DEVICE*d,BC250_ESCAPE_IH*p)
{
 (void)d;calls++;
 if(p->Op!=BC250_IH_OP_INIT || p->Magic!=BC250_ESCAPE_MAGIC || p->Command!=BC250_ESCAPE_RUN_IH || p->WriteCount)badInput++;
 p->WriteCount=19;p->Active=mode==4?0:1;result(&p->Status,&p->NtStatus,&p->Result);
}
static void GfxExecute(BC250_DEVICE*d,BC250_ESCAPE_GFX*p)
{
 (void)d;calls++;
 if(p->Op!=BC250_GFX_OP_RUN || p->LastStage!=(keepLog?(unsigned long)calls:8) || p->Magic!=BC250_ESCAPE_MAGIC || p->Command!=BC250_ESCAPE_RUN_GFX || (!keepLog && p->WriteCount))badInput++;
 p->WriteCount=19;p->StagesDone=mode==4?7:p->LastStage;p->FailedStage=mode==7?6:0;
 result(&p->Status,&p->NtStatus,&p->Result);
}
static void check(int yes,const char*name){checks++;if(!yes){failures++;printf("FAIL mode=%d %s\n",mode,name);}}
