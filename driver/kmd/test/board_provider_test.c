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
    struct {BOOLEAN BoardAllowed,Online;} Hwmon;
    struct {BOOLEAN BoardAllowed,Online;volatile LONG OcTier;} Smu;
    ULONG OcTier,OcStoredTier,OcReason,OcBoard;
} BC250_DEVICE;
static unsigned checks,failures;
#define CHECK(x) do{++checks;if(!(x)){++failures;printf("FAIL CHECK %u: %s\n",__LINE__,#x);}}while(0)
/* ACTUAL_PROVIDER */
static void query_abi(BC250_DEVICE* device,BC250_ESCAPE_BOARD_CAPS* caps,ULONG abi)
{
    D3DDDI_ESCAPEFLAGS flags={0};flags.NoAdapterSynchronization=1;
    memset(caps,0,sizeof(*caps));caps->Magic=BC250_ESCAPE_MAGIC;
    caps->Command=BC250_ESCAPE_RUN_BOARD_CAPS;caps->AbiVersion=abi;
    caps->Status=BC250_ESCAPE_STATUS_UNKNOWN_COMMAND;
    BoardProviderCaps(device,caps,flags.Value);
}
static void query(BC250_DEVICE* device,BC250_ESCAPE_BOARD_CAPS* caps){query_abi(device,caps,BC250_BOARD_CAPS_ABI);}
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
    CHECK(caps.GpuPoints[15].Mv==1000 && caps.GpuDefaultMaxMHz==2000);
    CHECK(caps.CpuStockCoreCount==6 && caps.CpuCoreCount==8 && caps.CpuRefuseMv==1300);
    CHECK(caps.FanPresetCounts[0]==5 && caps.FanPresetCounts[1]==5 && caps.FanPresetCounts[2]==4);
    CHECK(caps.FanPresets[0][3].TempC==76 && caps.FanPresets[0][3].DutyPct==95);
    CHECK(caps.FanPresets[1][4].TempC==85 && caps.FanPresets[2][3].DutyPct==100);
    for(i=0;i<ARRAYSIZE(caps.Reserved);++i)CHECK(!caps.Reserved[i]);
    /* ABI 2: the overclock envelopes, and no tier on a start that decided none. */
    CHECK(caps.AbiVersion==2 && caps.OcTierCount==3 && !caps.OcTier && !caps.OcStoredTier && !caps.OcReason && !caps.OcBoard);
    CHECK(caps.OcTiers[0].GpuMaxMHz==2000 && caps.OcTiers[0].GpuMaxMv==1000 && caps.OcTiers[0].CpuMaxMHz==3500 &&
          caps.OcTiers[0].CpuUvMaxSteps==16);
    CHECK(caps.OcTiers[1].GpuMaxMHz==2200 && caps.OcTiers[1].GpuMaxMv==1050 && caps.OcTiers[1].CpuMaxMHz==3900 &&
          caps.OcTiers[1].CpuUvMaxSteps==40);
    CHECK(caps.OcTiers[2].GpuMaxMHz==2300 && caps.OcTiers[2].GpuMaxMv==1075 && caps.OcTiers[2].CpuMaxMHz==4100 &&
          caps.OcTiers[2].CpuUvMaxSteps==40);
    CHECK(caps.OcPoints[0].MHz==2100 && caps.OcPoints[0].Mv==1050 && caps.OcPoints[0].Vid==80);
    CHECK(caps.OcPoints[1].MHz==2200 && caps.OcPoints[1].Mv==1050 && caps.OcPoints[1].Vid==80);
    CHECK(caps.OcPoints[2].MHz==2300 && caps.OcPoints[2].Mv==1075 && caps.OcPoints[2].Vid==76);
    CHECK(caps.OcCpuMinUvSteps[0]==8 && caps.OcCpuMinUvSteps[1]==16 && caps.OcCpuMinUvSteps[4]==36 &&
          caps.OcCpuMinUvSteps[5]==40);
    if(argc==2){FILE* f=NULL;CHECK(fopen_s(&f,argv[1],"wb")==0);if(f){CHECK(fwrite(&caps,1,sizeof(caps),f)==sizeof(caps));CHECK(fclose(f)==0);}}
    /* An older reader asks for ABI 1 and gets the ABI 1 reply: every overclock word stays zero. */
    device.OcTier=2;device.OcStoredTier=2;device.OcReason=1;device.OcBoard=0x1234;
    query_abi(&device,&caps,1);CHECK(caps.Status==BC250_ESCAPE_STATUS_DONE && caps.AbiVersion==1 && caps.Flags==31);
    {const ULONG* w=(const ULONG*)&caps;for(i=139;i<192;++i)CHECK(!w[i]);}
    query(&device,&caps);CHECK(caps.OcTier==2 && caps.OcStoredTier==2 && caps.OcReason==1 && caps.OcBoard==0x1234);
    query_abi(&device,&caps,3);CHECK(caps.Status==BC250_ESCAPE_STATUS_REFUSED);
    device.Hwmon.Online=device.Smu.Online=FALSE;
    query(&device,&caps);CHECK(caps.Flags==31);
    device.RetainedPowerPhase=1;query(&device,&caps);CHECK(caps.Flags==1);
    device.RetainedPowerPhase=0;device.Started=FALSE;query(&device,&caps);CHECK(caps.Flags==1);
    device.Smu.OcTier=2;
    BoardProviderUnbind(&device);CHECK(!device.Hwmon.BoardAllowed && !device.Smu.BoardAllowed);
    CHECK(!device.Smu.OcTier && !device.OcTier);
    memset(&caps,0,sizeof(caps));BoardProviderCaps(&device,&caps,0);CHECK(caps.Status==BC250_ESCAPE_STATUS_REFUSED);
    printf("%u checks, %u failures\n",checks,failures);return failures?1:0;
}
