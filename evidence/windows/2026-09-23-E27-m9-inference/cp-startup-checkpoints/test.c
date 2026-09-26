#include <stdio.h>
#include <string.h>
#include <stdint.h>
typedef int32_t NTSTATUS;
typedef unsigned long ULONG;
typedef int BOOLEAN;
#define PASSIVE_LEVEL 0
#define STATUS_SUCCESS 0
#define STATUS_INVALID_PARAMETER (-1)
#define STATUS_INVALID_DEVICE_STATE (-2)
#define STATUS_DEVICE_HARDWARE_ERROR (-3)
#define STATUS_IO_DEVICE_ERROR (-4)
#define NT_SUCCESS(s) ((NTSTATUS)(s)>=0)
#define RtlZeroMemory(p,n) memset(p,0,n)
#define BC250_ESCAPE_MAGIC 1
#define BC250_ESCAPE_RUN_GFX 2
#define BC250_GFX_OP_RUN 1
#define BC250_ESCAPE_STATUS_DONE 0
#define BC250_ESCAPE_STATUS_REFUSED 1
#define BC250_GFX_STAGE_CP 6
#define BC250_GFX_STAGE_INTERRUPTS 8
typedef struct {int GpuStopUnconfirmed;} BC250_DEVICE;
typedef struct {ULONG Magic,Command,Op,LastStage,NtStatus,Status,FailedStage,StagesDone;long Result;} BC250_ESCAPE_GFX;
static int capture,failed,mode,bad,calls,keeps;
static ULONG seen[32],last_cp;
static int KeGetCurrentIrql(void) {return 0;}
static ULONG GuardReadSetting(const void *name,ULONG value) {(void)name;(void)value;return (ULONG)capture;}
#define GuardLog(...) ((void)0)
static void GuardLogKeep(void) {keeps++;}
static void GfxExecute(BC250_DEVICE *d,BC250_ESCAPE_GFX *r,ULONG cp) {
 (void)d;
 seen[calls++]=r->LastStage*100+cp;
 r->NtStatus=0;r->Result=0;r->FailedStage=0;r->Status=0;r->StagesDone=r->LastStage;
 if(cp) {
  if(cp!=last_cp+1)bad++;
  last_cp=cp;
  if((int)cp==failed) {
   if(mode==0)r->NtStatus=(ULONG)STATUS_IO_DEVICE_ERROR;
   else {r->Result=-62;r->Status=BC250_ESCAPE_STATUS_REFUSED;r->FailedStage=6;}
  }
 }
}
enum bc250_cp_step {
	BC250_CP_KIQ_INIT = 1,
	BC250_CP_COMPUTE_INIT,
	BC250_CP_KCQ_ENABLE,
	BC250_CP_GFX_QUEUE_INIT,
	BC250_CP_KGQ_ENABLE,
	BC250_CP_GFX_START,
	BC250_CP_GFX_TEST,
	BC250_CP_COMPUTE_TEST,
};
NTSTATUS GfxInitializeHardware(BC250_DEVICE* Device, BC250_ESCAPE_GFX* Report)
{
    NTSTATUS status;
    ULONG stage, first, cp;
    BOOLEAN trace;
    if (!Report) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(Report,sizeof(*Report));
    Report->Magic=BC250_ESCAPE_MAGIC;Report->Command=BC250_ESCAPE_RUN_GFX;
    Report->Op=BC250_GFX_OP_RUN;
    Report->LastStage=BC250_GFX_STAGE_INTERRUPTS;
    if (!Device) status=STATUS_INVALID_PARAMETER;
    else if (KeGetCurrentIrql()!=PASSIVE_LEVEL) status=STATUS_INVALID_DEVICE_STATE;
    else if (Device->GpuStopUnconfirmed) status=STATUS_DEVICE_HARDWARE_ERROR;
    else {
        trace=GuardReadSetting(L"KeepLog",0)!=0;
        first=trace?1:BC250_GFX_STAGE_INTERRUPTS;
        status=STATUS_SUCCESS;
        // Startup owns the unpublished device. Incremental RUN already preserves
        // setup and completed stages. Snapshot only outside GfxExecute's locks,
        // at PASSIVE_LEVEL. In trace mode Report describes the last attempted
        // stage; the persisted snapshots retain earlier detailed stage reports.
        for (stage=first;stage<=BC250_GFX_STAGE_INTERRUPTS;stage++) {
            Report->LastStage=stage;
            if (trace) {
                GuardLog("startup: entering GFX stage %lu",stage);
                GuardLogKeep();
            }
            if (trace && stage == BC250_GFX_STAGE_CP) {
                for (cp=BC250_CP_KIQ_INIT;cp<=BC250_CP_COMPUTE_TEST;cp++) {
                    GuardLog("startup: entering CP step %lu",cp);
                    GuardLogKeep();
                    GfxExecute(Device,Report,cp);
                    GuardLog("startup: CP step %lu returned 0x%08X result %ld",cp,Report->NtStatus,Report->Result);
                    GuardLogKeep();
                    if (!NT_SUCCESS((NTSTATUS)Report->NtStatus) || Report->Status!=BC250_ESCAPE_STATUS_DONE ||
                        Report->Result!=0 || Report->FailedStage) break;
                }
            } else GfxExecute(Device,Report,0);
            status=(NTSTATUS)Report->NtStatus;
            if (NT_SUCCESS(status) && (Report->Status!=BC250_ESCAPE_STATUS_DONE || Report->Result!=0 ||
                Report->FailedStage || Report->StagesDone<stage)) status=STATUS_IO_DEVICE_ERROR;
            if (trace) GuardLogKeep();
            if (!NT_SUCCESS(status)) break;
        }
    }
    Report->NtStatus=(unsigned long)status;
    Report->Status=NT_SUCCESS(status)?BC250_ESCAPE_STATUS_DONE:BC250_ESCAPE_STATUS_REFUSED;
    return status;
}

int main(void) {
 BC250_DEVICE d={0};BC250_ESCAPE_GFX r;int m,k;NTSTATUS status;
 for(m=0;m<2;m++)for(k=0;k<=8;k++) {
  capture=1;failed=k;mode=m;calls=keeps=0;last_cp=0;
  status=GfxInitializeHardware(&d,&r);
  if(k==0) {
   if(status!=0||calls!=15||last_cp!=8||seen[13]!=700||seen[14]!=800||keeps!=32)bad++;
  } else if(status>=0||calls!=5+k||last_cp!=(ULONG)k||seen[calls-1]!=600+(ULONG)k||r.LastStage!=6)bad++;
 }
 capture=0;failed=0;calls=keeps=0;last_cp=0;
 status=GfxInitializeHardware(&d,&r);
 if(status!=0||calls!=1||seen[0]!=800||keeps!=0)bad++;
 printf("CP startup coordinator: 19 scenarios, %d failures\n",bad);
 return bad?1:0;
}
