// Software-only provider query; actual shipping function and policy tables.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "bc250kmd_escape.h"
#include "bc250_board_envelope.h"
#include "bc250_clock.h"
#include "bc250_cpu.h"
#include "bc250_dpm.h"
#include "bc250_fan.h"
#define STATUS_SUCCESS 0
#define RtlCompareMemory(a,b,n) (memcmp(a,b,n)==0?(n):0)
typedef union { struct { unsigned HardwareAccess:1,DeviceStatusQuery:1,ChangeFrameLatency:1,NoAdapterSynchronization:1; }; ULONG Value; } D3DDDI_ESCAPEFLAGS;
typedef struct {
    LONG BoardMemoryProviderId,RetainedPowerPhase;
    BOOLEAN Started;
    struct {BOOLEAN BoardAllowed,Online;} Hwmon,Smu;
} BC250_DEVICE;
static unsigned checks,failures;
#define CHECK(x) do{++checks;if(!(x)){++failures;printf("FAIL CHECK %u: %s\n",__LINE__,#x);}}while(0)
/* ACTUAL_PROVIDER */
static void query(BC250_DEVICE* device,BC250_ESCAPE_BOARD_CAPS* caps)
{
    D3DDDI_ESCAPEFLAGS flags={0};flags.NoAdapterSynchronization=1;
    memset(caps,0,sizeof(*caps));caps->Magic=BC250_ESCAPE_MAGIC;
    caps->Command=BC250_ESCAPE_RUN_BOARD_CAPS;caps->AbiVersion=1;
    caps->Status=BC250_ESCAPE_STATUS_UNKNOWN_COMMAND;
    BoardProviderCaps(device,caps,flags.Value);
}
int main(int argc,char** argv)
{
    BC250_DEVICE device={0};BC250_ESCAPE_BOARD_CAPS caps;unsigned i;
    const LONG denied[]={0,2,-1,0x7fffffff};
    for(i=0;i<sizeof(denied)/sizeof(denied[0]);++i){
        device.BoardMemoryProviderId=denied[i];device.Started=TRUE;
        BoardProviderBind(&device);CHECK(!device.Hwmon.BoardAllowed && !device.Smu.BoardAllowed);
        query(&device,&caps);CHECK(caps.Status==BC250_ESCAPE_STATUS_DONE);
        CHECK(!caps.ProviderId && !caps.Flags && !caps.GpuPointCount && !caps.CpuMaxMHz && !caps.FanPointMax);
    }
    device.BoardMemoryProviderId=1;BoardProviderBind(&device);
    CHECK(device.Hwmon.BoardAllowed && device.Smu.BoardAllowed);
    device.Hwmon.Online=device.Smu.Online=TRUE;
    query(&device,&caps);CHECK(caps.Flags==31 && caps.ProviderId==1);
    CHECK(caps.GpuPointCount==16 && caps.GpuMinMHz==500 && caps.GpuMaxMHz==2000);
    CHECK(caps.GpuPoints[5].MHz==1000 && caps.GpuPoints[5].Mv==820 && caps.GpuPoints[5].Vid==116);
    CHECK(caps.GpuPoints[15].Mv==1000 && caps.GpuDefaultMaxMHz==1500);
    CHECK(caps.CpuStockCoreCount==6 && caps.CpuCoreCount==8 && caps.CpuRefuseMv==1300);
    CHECK(caps.FanPresetCounts[0]==5 && caps.FanPresetCounts[1]==5 && caps.FanPresetCounts[2]==4);
    CHECK(caps.FanPresets[0][3].TempC==76 && caps.FanPresets[0][3].DutyPct==95);
    CHECK(caps.FanPresets[1][4].TempC==85 && caps.FanPresets[2][3].DutyPct==100);
    for(i=0;i<53;++i)CHECK(!caps.Reserved[i]);
    if(argc==2){FILE* f=NULL;CHECK(fopen_s(&f,argv[1],"wb")==0);if(f){CHECK(fwrite(&caps,1,sizeof(caps),f)==sizeof(caps));CHECK(fclose(f)==0);}}
    device.Hwmon.Online=device.Smu.Online=FALSE;
    query(&device,&caps);CHECK(caps.Flags==31);
    device.RetainedPowerPhase=1;query(&device,&caps);CHECK(caps.Flags==1);
    device.RetainedPowerPhase=0;device.Started=FALSE;query(&device,&caps);CHECK(caps.Flags==1);
    BoardProviderUnbind(&device);CHECK(!device.Hwmon.BoardAllowed && !device.Smu.BoardAllowed);
    memset(&caps,0,sizeof(caps));BoardProviderCaps(&device,&caps,0);CHECK(caps.Status==BC250_ESCAPE_STATUS_REFUSED);
    printf("%u checks, %u failures\n",checks,failures);return failures?1:0;
}
