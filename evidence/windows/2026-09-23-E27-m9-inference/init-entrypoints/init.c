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
 if(p->Op!=BC250_GFX_OP_RUN || p->LastStage!=8 || p->Magic!=BC250_ESCAPE_MAGIC || p->Command!=BC250_ESCAPE_RUN_GFX || p->WriteCount)badInput++;
 p->WriteCount=19;p->StagesDone=mode==4?7:8;p->FailedStage=mode==7?6:0;
 result(&p->Status,&p->NtStatus,&p->Result);
}
static void check(int yes,const char*name){checks++;if(!yes){failures++;printf("FAIL mode=%d %s\n",mode,name);}}
void GartEscape(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_ESCAPE_GART* Data)
{
    GartExecute(Device,Data);
}
NTSTATUS GartInitializeHardware(BC250_DEVICE* Device, BC250_ESCAPE_GART* Report)
{
    NTSTATUS status;
    if (!Report) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(Report,sizeof(*Report));
    Report->Magic=BC250_ESCAPE_MAGIC;Report->Command=BC250_ESCAPE_RUN_GART;
    Report->Op=BC250_GART_OP_ENABLE;
    if (!Device) status=STATUS_INVALID_PARAMETER;
    else if (KeGetCurrentIrql()!=PASSIVE_LEVEL) status=STATUS_INVALID_DEVICE_STATE;
    else if (Device->GpuStopUnconfirmed) status=STATUS_DEVICE_HARDWARE_ERROR;
    else {
        GartExecute(Device,Report);
        status=(NTSTATUS)Report->NtStatus;
        if (NT_SUCCESS(status) && (Report->Status!=BC250_ESCAPE_STATUS_DONE || Report->Result!=0 ||
            (Report->State&BC250_GART_STATE_ENABLED)==0)) status=STATUS_IO_DEVICE_ERROR;
    }
    Report->NtStatus=(unsigned long)status;
    Report->Status=NT_SUCCESS(status)?BC250_ESCAPE_STATUS_DONE:BC250_ESCAPE_STATUS_REFUSED;
    return status;
}
void PspEscape(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_ESCAPE_PSP* Data)
{
    PspExecute(Device,Data);
}
NTSTATUS PspInitializeHardware(BC250_DEVICE* Device, BC250_ESCAPE_PSP* Report)
{
    NTSTATUS status;
    if (!Report) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(Report,sizeof(*Report));
    Report->Magic=BC250_ESCAPE_MAGIC;Report->Command=BC250_ESCAPE_RUN_PSP;
    Report->Op=BC250_PSP_OP_LOAD;
    if (!Device) status=STATUS_INVALID_PARAMETER;
    else if (KeGetCurrentIrql()!=PASSIVE_LEVEL) status=STATUS_INVALID_DEVICE_STATE;
    else if (Device->GpuStopUnconfirmed) status=STATUS_DEVICE_HARDWARE_ERROR;
    else {
        PspExecute(Device,Report);
        status=(NTSTATUS)Report->NtStatus;
        if (NT_SUCCESS(status) && (Report->Status!=BC250_ESCAPE_STATUS_DONE || Report->Result!=0 ||
            (Report->State&(BC250_PSP_STATE_RING|BC250_PSP_STATE_TMR|BC250_PSP_STATE_GART))!=(BC250_PSP_STATE_RING|BC250_PSP_STATE_TMR|BC250_PSP_STATE_GART) || !Report->CommandCount || Report->CommandsDone!=Report->CommandCount)) status=STATUS_IO_DEVICE_ERROR;
    }
    Report->NtStatus=(unsigned long)status;
    Report->Status=NT_SUCCESS(status)?BC250_ESCAPE_STATUS_DONE:BC250_ESCAPE_STATUS_REFUSED;
    return status;
}
void IhEscape(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_ESCAPE_IH* Data)
{
    IhExecute(Device,Data);
}
NTSTATUS IhInitializeHardware(BC250_DEVICE* Device, BC250_ESCAPE_IH* Report)
{
    NTSTATUS status;
    if (!Report) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(Report,sizeof(*Report));
    Report->Magic=BC250_ESCAPE_MAGIC;Report->Command=BC250_ESCAPE_RUN_IH;
    Report->Op=BC250_IH_OP_INIT;
    if (!Device) status=STATUS_INVALID_PARAMETER;
    else if (KeGetCurrentIrql()!=PASSIVE_LEVEL) status=STATUS_INVALID_DEVICE_STATE;
    else if (Device->GpuStopUnconfirmed) status=STATUS_DEVICE_HARDWARE_ERROR;
    else {
        IhExecute(Device,Report);
        status=(NTSTATUS)Report->NtStatus;
        if (NT_SUCCESS(status) && (Report->Status!=BC250_ESCAPE_STATUS_DONE || Report->Result!=0 ||
            !Report->Active)) status=STATUS_IO_DEVICE_ERROR;
    }
    Report->NtStatus=(unsigned long)status;
    Report->Status=NT_SUCCESS(status)?BC250_ESCAPE_STATUS_DONE:BC250_ESCAPE_STATUS_REFUSED;
    return status;
}
void GfxEscape(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_ESCAPE_GFX* Data)
{
    GfxExecute(Device,Data);
}
NTSTATUS GfxInitializeHardware(BC250_DEVICE* Device, BC250_ESCAPE_GFX* Report)
{
    NTSTATUS status;
    if (!Report) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(Report,sizeof(*Report));
    Report->Magic=BC250_ESCAPE_MAGIC;Report->Command=BC250_ESCAPE_RUN_GFX;
    Report->Op=BC250_GFX_OP_RUN;
    Report->LastStage=BC250_GFX_STAGE_INTERRUPTS;
    if (!Device) status=STATUS_INVALID_PARAMETER;
    else if (KeGetCurrentIrql()!=PASSIVE_LEVEL) status=STATUS_INVALID_DEVICE_STATE;
    else if (Device->GpuStopUnconfirmed) status=STATUS_DEVICE_HARDWARE_ERROR;
    else {
        GfxExecute(Device,Report);
        status=(NTSTATUS)Report->NtStatus;
        if (NT_SUCCESS(status) && (Report->Status!=BC250_ESCAPE_STATUS_DONE || Report->Result!=0 ||
            Report->FailedStage || Report->StagesDone<BC250_GFX_STAGE_INTERRUPTS)) status=STATUS_IO_DEVICE_ERROR;
    }
    Report->NtStatus=(unsigned long)status;
    Report->Status=NT_SUCCESS(status)?BC250_ESCAPE_STATUS_DONE:BC250_ESCAPE_STATUS_REFUSED;
    return status;
}

#define CHECK_INIT(Name,Upper,storage) do { \
 NTSTATUS status,expected; \
 memset(&(storage),0xCC,sizeof(storage));calls=badInput=0; \
 status=Name##InitializeHardware(&d,&(storage)); \
 expected=(mode==1||mode==6)?STATUS_DEVICE_NOT_READY:(mode==5?1:((mode==0)?0:STATUS_IO_DEVICE_ERROR)); \
 check(status==expected,#Name " propagates failures and rejects incomplete success"); \
 check(calls==1&&!badInput,#Name " initializes command once through shared core"); \
 check((NTSTATUS)(storage).NtStatus==expected && (storage).Status==(NT_SUCCESS(expected)?BC250_ESCAPE_STATUS_DONE:BC250_ESCAPE_STATUS_REFUSED),#Name " report and return agree"); \
 check((storage).WriteCount==19,#Name " partial diagnostic progress retained"); \
} while(0)
#define CHECK_GUARDS(Name,storage) do { \
 calls=0;mode=0;d.GpuStopUnconfirmed=1; \
 check(Name##InitializeHardware(&d,&(storage))==STATUS_DEVICE_HARDWARE_ERROR&&!calls,#Name " quarantined device does no hardware work"); \
 d.GpuStopUnconfirmed=0;irql=1; \
 check(Name##InitializeHardware(&d,&(storage))==STATUS_INVALID_DEVICE_STATE&&!calls,#Name " wrong IRQL does no hardware work"); \
 irql=0; \
 check(Name##InitializeHardware(NULL,&(storage))==STATUS_INVALID_PARAMETER&&!calls,#Name " absent device refuses"); \
 check(Name##InitializeHardware(&d,NULL)==STATUS_INVALID_PARAMETER&&!calls,#Name " absent report refuses"); \
} while(0)
int main(void)
{
 BC250_DEVICE d={0};
 static BC250_ESCAPE_GART gart;static BC250_ESCAPE_PSP psp;
 static BC250_ESCAPE_IH ih;static BC250_ESCAPE_GFX gfx;
 for(mode=0;mode<=6;mode++){
  CHECK_INIT(Gart,GART,gart);CHECK_INIT(Psp,PSP,psp);CHECK_INIT(Ih,IH,ih);CHECK_INIT(Gfx,GFX,gfx);
 }
 mode=7;CHECK_INIT(Psp,PSP,psp);CHECK_INIT(Gfx,GFX,gfx);
 mode=8;CHECK_INIT(Psp,PSP,psp);
 CHECK_GUARDS(Gart,gart);CHECK_GUARDS(Psp,psp);CHECK_GUARDS(Ih,ih);CHECK_GUARDS(Gfx,gfx);
 // Diagnostic entry points remain simple forwarding: supplied reports are not zeroed.
 mode=0;calls=0;gart.WriteCount=77;GartEscape(&d,&gart);
 psp.WriteCount=77;PspEscape(&d,&psp);ih.WriteCount=77;IhEscape(&d,&ih);gfx.WriteCount=77;GfxEscape(&d,&gfx);
 check(calls==4 && badInput==4,"diagnostic wrappers forward caller input unchanged");
 printf("%d checks, %d failures\n",checks,failures);
 return failures?1:0;
}
