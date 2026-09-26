/* Read-only lab observation: no modeset, present, clocks or register writes. */
#include <windows.h>
#include <d3dkmthk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../driver/kmd/bc250kmd_escape.h"
#include "../../driver/kmd/umd_caps.h"
#include "../../driver/contract/bc250_umd_firmware.h"

static NTSTATUS DriverEscape(D3DKMT_HANDLE adapter,void *data,UINT bytes)
{
    D3DKMT_ESCAPE e={0};
    e.hAdapter=adapter;e.Type=D3DKMT_ESCAPE_DRIVERPRIVATE;
    e.pPrivateDriverData=data;e.PrivateDriverDataSize=bytes;
    /* Normal adapter synchronization preserves lifetime during these reads. */
    return D3DKMTEscape(&e);
}

static int Firmware(FILE *log,D3DKMT_HANDLE adapter)
{
    BYTE caps[UMD_CAPS_BYTES];
    struct bc250_umd_firmware firmware;
    D3DKMT_QUERYADAPTERINFO q={0}; NTSTATUS status;
    const char *names[]={"me","pfp","ce","mec","mec2","rlc","sdma"};
    unsigned words[16];
    memset(caps,0xA5,sizeof(caps));
    q.hAdapter=adapter;q.Type=KMTQAITYPE_UMDRIVERPRIVATE;
    q.pPrivateDriverData=caps;q.PrivateDriverDataSize=sizeof(caps);
    status=D3DKMTQueryAdapterInfo(&q);
    fprintf(log,"CAPS status=0x%08lX bytes=%zu\n",(ULONG)status,sizeof(caps));
    if(status<0)return 1;
    memcpy(&firmware,caps+UMD_CAPS_FIRMWARE_OFFSET,sizeof(firmware));
    memcpy(words,&firmware,sizeof(words));
    for(unsigned i=0;i<7;i++)fprintf(log,"FIRMWARE %s version=0x%08X feature=0x%08X\n",names[i],words[2*i],words[2*i+1]);
    fprintf(log,"FIRMWARE smc version=0x%08X reserved=0x%08X\n",firmware.smc_version,firmware.reserved);
    return 0;
}

C_ASSERT(sizeof(BC250_ESCAPE_DCN_OBSERVE)==128);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_DCN_OBSERVE,SequenceBefore)-FIELD_OFFSET(BC250_ESCAPE_DCN_OBSERVE,PrimaryAddressLow)==22*4);

static int Observe(FILE *log,D3DKMT_HANDLE adapter,ULONG sample,const char *side,LARGE_INTEGER frequency)
{
    BC250_ESCAPE_DCN_OBSERVE o={0};D3DKMT_ESCAPE e={0};
    ULONG values[BC250_DCN_OBSERVE_REG_COUNT];LARGE_INTEGER begin,end;NTSTATUS status;
    o.Magic=BC250_ESCAPE_MAGIC;o.Command=BC250_ESCAPE_OBSERVE_DCN;o.AbiVersion=BC250_DCN_OBSERVE_ABI;
    e.hAdapter=adapter;e.Type=D3DKMT_ESCAPE_DRIVERPRIVATE;e.Flags.HardwareAccess=1;
    e.pPrivateDriverData=&o;e.PrivateDriverDataSize=sizeof(o);
    QueryPerformanceCounter(&begin);status=D3DKMTEscape(&e);QueryPerformanceCounter(&end);
    memcpy(values,&o.PrimaryAddressLow,sizeof(values));
    fprintf(log,"OBS sample=%lu side=%s begin=%lld end=%lld frequency=%lld nt=0x%08lX result=%lu reason=0x%08lX version=0x%08lX valid=0x%08lX count=%lu before=%lu after=%lu raw=",
        sample,side,begin.QuadPart,end.QuadPart,frequency.QuadPart,(ULONG)status,o.Status,o.NtStatus,o.Version,o.ValidMask,o.RegisterCount,o.SequenceBefore,o.SequenceAfter);
    for(unsigned i=0;i<BC250_DCN_OBSERVE_REG_COUNT;i++)fprintf(log,"%s%08lX",i?",":"",values[i]);
    fprintf(log,"\n");
    return status<0 || o.Status!=BC250_ESCAPE_STATUS_DONE || o.NtStatus!=0 ||
        o.AbiVersion!=BC250_DCN_OBSERVE_ABI || o.RegisterCount!=BC250_DCN_OBSERVE_REG_COUNT || o.ValidMask!=BC250_DCN_OBSERVE_VALID_ALL;
}


int wmain(int argc,wchar_t **argv)
{
    UINT32 pathCount=0,modeCount=0;
    DISPLAYCONFIG_PATH_INFO *paths=NULL;DISPLAYCONFIG_MODE_INFO *modes=NULL;
    D3DKMT_OPENADAPTERFROMLUID open={0};D3DKMT_CLOSEADAPTER close={0};
    BC250_ESCAPE info={0};
    LARGE_INTEGER frequency,start,now;
    ULONG observationFailures=0,active=0,blank=0,changes=0,failures=0,minLine=~0u,maxLine=0,previous=~0u;
    LONG error;NTSTATUS status;FILE *log;int result=1;
    if(argc!=2 || _wfopen_s(&log,argv[1],L"wx"))return 2;
    error=GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS,&pathCount,&modeCount);
    if(error || pathCount!=1){fprintf(log,"PATH_ERROR error=%ld paths=%u\n",error,pathCount);goto done;}
    paths=(DISPLAYCONFIG_PATH_INFO*)calloc(pathCount,sizeof(*paths));
    modes=(DISPLAYCONFIG_MODE_INFO*)calloc(modeCount,sizeof(*modes));
    if(!paths || !modes)goto done;
    error=QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS,&pathCount,paths,&modeCount,modes,NULL);
    if(error || pathCount!=1){fprintf(log,"CONFIG_ERROR error=%ld paths=%u\n",error,pathCount);goto done;}
    open.AdapterLuid=paths[0].sourceInfo.adapterId;
    status=D3DKMTOpenAdapterFromLuid(&open);
    fprintf(log,"OPEN status=0x%08lX source=%u target=%u\n",(ULONG)status,paths[0].sourceInfo.id,paths[0].targetInfo.id);
    if(status<0)goto done;
    fprintf(log,"QDC_REFRESH numerator=%u denominator=%u\n",paths[0].targetInfo.refreshRate.Numerator,paths[0].targetInfo.refreshRate.Denominator);
    for(UINT32 i=0;i<modeCount;i++)if(modes[i].infoType==DISPLAYCONFIG_MODE_INFO_TYPE_TARGET){
        DISPLAYCONFIG_VIDEO_SIGNAL_INFO *s=&modes[i].targetMode.targetVideoSignalInfo;
        fprintf(log,"QDC_SIGNAL pixel=%llu active=%ux%u total=%ux%u h=%u/%u v=%u/%u\n",s->pixelRate,s->activeSize.cx,s->activeSize.cy,s->totalSize.cx,s->totalSize.cy,s->hSyncFreq.Numerator,s->hSyncFreq.Denominator,s->vSyncFreq.Numerator,s->vSyncFreq.Denominator);
    }
    info.Magic=BC250_ESCAPE_MAGIC;info.Command=BC250_ESCAPE_GET_INFO;
    status=DriverEscape(open.hAdapter,&info,sizeof(info));
    fprintf(log,"INFO nt=0x%08lX result=%lu version=0x%08lX width=%lu height=%lu pitch=%lu flags=0x%lX\n",(ULONG)status,info.Status,info.Version,info.Width,info.Height,info.Pitch,info.Flags);
    if(status<0 || info.Status!=BC250_ESCAPE_STATUS_DONE || !(info.Flags&BC250_ESCAPE_FLAG_FULL_WDDM))goto close;
    if(Firmware(log,open.hAdapter))goto close;
    QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&start);
    for(ULONG i=0;i<1024;i++){
        D3DKMT_GETSCANLINE get={0};get.hAdapter=open.hAdapter;get.VidPnSourceId=paths[0].sourceInfo.id;
        if(i%128==0)observationFailures+=Observe(log,open.hAdapter,i,"before",frequency);
        status=D3DKMTGetScanLine(&get);QueryPerformanceCounter(&now);
        fprintf(log,"RASTER sample=%lu us=%.3f nt=0x%08lX blank=%u line=%u\n",i,1e6*(double)(now.QuadPart-start.QuadPart)/(double)frequency.QuadPart,(ULONG)status,get.InVerticalBlank?1u:0u,get.ScanLine);
        if(i%128==0)observationFailures+=Observe(log,open.hAdapter,i,"after",frequency);
        if(status<0)failures++;
        else if(get.InVerticalBlank)blank++;
        else {active++;if(get.ScanLine<minLine)minLine=get.ScanLine;if(get.ScanLine>maxLine)maxLine=get.ScanLine;if(previous!=~0u && previous!=get.ScanLine)changes++;previous=get.ScanLine;}
        Sleep(1);
    }
    fprintf(log,"RASTER_RESULT samples=1024 failures=%lu active=%lu blank=%lu active_changes=%lu min_line=%lu max_line=%lu\n",failures,active,blank,changes,minLine,maxLine);
    fprintf(log,"OBS_RESULT samples=16 failures=%lu scheduling_idled=1\n",observationFailures);
    result=(failures || observationFailures)?1:0;
close:
    close.hAdapter=open.hAdapter;status=D3DKMTCloseAdapter(&close);
    fprintf(log,"CLOSE status=0x%08lX\n",(ULONG)status);if(status<0)result=1;
done:
    fclose(log);free(paths);free(modes);return result;
}
