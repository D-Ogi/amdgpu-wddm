#include <windows.h>
#include <d3dkmthk.h>
#include <stdio.h>
#include <stdlib.h>

int wmain(int argc, wchar_t **argv)
{
    UINT32 pathCount=0,modeCount=0;
    DISPLAYCONFIG_PATH_INFO *paths;
    DISPLAYCONFIG_MODE_INFO *modes;
    D3DKMT_OPENADAPTERFROMLUID open={0};
    D3DKMT_CLOSEADAPTER close={0};
    LARGE_INTEGER frequency,start,now;
    ULONG active=0,blank=0,changes=0,failures=0,minLine=~0u,maxLine=0,previous=~0u;
    LONG error; NTSTATUS status; FILE *log;
    if(argc!=2 || _wfopen_s(&log,argv[1],L"w"))return 2;
    error=GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS,&pathCount,&modeCount);
    if(error || pathCount!=1){fprintf(log,"DISPLAY_PATH_CONTROL error=%ld paths=%u\n",error,pathCount);fclose(log);return 3;}
    paths=(DISPLAYCONFIG_PATH_INFO*)calloc(pathCount,sizeof(*paths));
    modes=(DISPLAYCONFIG_MODE_INFO*)calloc(modeCount,sizeof(*modes));
    if(!paths || !modes){fclose(log);free(paths);free(modes);return 4;}
    error=QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS,&pathCount,paths,&modeCount,modes,NULL);
    if(error || pathCount!=1){fprintf(log,"DISPLAY_CONFIG error=%ld paths=%u\n",error,pathCount);fclose(log);free(paths);free(modes);return 5;}
    open.AdapterLuid=paths[0].sourceInfo.adapterId;
    status=D3DKMTOpenAdapterFromLuid(&open);
    fprintf(log,"open_status=0x%08lX source=%u target=%u\n",(ULONG)status,paths[0].sourceInfo.id,paths[0].targetInfo.id);
    if(status<0){fclose(log);free(paths);free(modes);return 6;}
    QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&start);
    fprintf(log,"sample,elapsed_us,status,blank,line\n");
    for(ULONG i=0;i<1024;i++){
        D3DKMT_GETSCANLINE get={0};get.hAdapter=open.hAdapter;get.VidPnSourceId=paths[0].sourceInfo.id;
        status=D3DKMTGetScanLine(&get);QueryPerformanceCounter(&now);
        fprintf(log,"%lu,%.3f,0x%08lX,%u,%u\n",i,1e6*(double)(now.QuadPart-start.QuadPart)/(double)frequency.QuadPart,(ULONG)status,get.InVerticalBlank?1u:0u,get.ScanLine);
        if(status<0)failures++;
        else if(get.InVerticalBlank)blank++;
        else {active++;if(get.ScanLine<minLine)minLine=get.ScanLine;if(get.ScanLine>maxLine)maxLine=get.ScanLine;if(previous!=~0u && previous!=get.ScanLine)changes++;previous=get.ScanLine;}
        Sleep(1);
    }
    close.hAdapter=open.hAdapter;status=D3DKMTCloseAdapter(&close);
    fprintf(log,"RASTER_RESULT samples=1024 failures=%lu active=%lu blank=%lu active_changes=%lu min_line=%lu max_line=%lu close=0x%08lX\n",failures,active,blank,changes,minLine,maxLine,(ULONG)status);
    fclose(log);free(paths);free(modes);
    /* Baseline may legitimately expose its existing stale timer defect; raw
       observations, not the probe, decide whether hardware behavior improved. */
    return failures || status<0 ? 1 : 0;
}
