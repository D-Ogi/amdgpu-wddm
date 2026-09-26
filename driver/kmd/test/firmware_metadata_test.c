// Actual PSP metadata/cache and UMDRIVERPRIVATE branch with kernel lifetime mocks.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bc250_psp.h"
#include "../umd_caps.h"
#include "../../contract/bc250_umd_firmware.h"
typedef long NTSTATUS;
typedef unsigned long ULONG;
typedef unsigned char* PUCHAR;
#define STATUS_SUCCESS 0L
#define STATUS_DEVICE_NOT_READY (-1L)
#define STATUS_INVALID_DEVICE_STATE (-2L)
#define STATUS_INVALID_IMAGE_FORMAT (-3L)
#define STATUS_INVALID_PARAMETER (-4L)
#define STATUS_BUFFER_TOO_SMALL (-5L)
#define NT_SUCCESS(s) ((s)>=0)
#define RtlZeroMemory(p,n) memset(p,0,n)
#define RtlCopyMemory(p,s,n) memcpy(p,s,n)
#define RTL_NUMBER_OF(a) (sizeof(a)/sizeof((a)[0]))
#define PASSIVE_LEVEL 0
#define DXGKQAITYPE_UMDRIVERPRIVATE 1
static unsigned checks,failures,locks,smuReads;
static int currentIrql,smuReady=1;
static ULONG testSmuVersion=0x00580600u;
#define CHECK(x) do {checks++;if(!(x)){failures++;printf("FAIL line %u: %s\n",__LINE__,#x);}} while(0)
static int KeGetCurrentIrql(void){return currentIrql;}
static void ExAcquireFastMutex(int* lock){CHECK(!*lock);*lock=1;locks++;}
static void ExReleaseFastMutex(int* lock){CHECK(*lock==1);*lock=0;}
typedef struct {int Loaded,PowerSuspended;struct bc250_umd_firmware Firmware;struct {NTSTATUS Fault;} Sequence;} BC250_PSP;
typedef struct {PUCHAR Data[BC250_FILE_COUNT];ULONG Size[BC250_FILE_COUNT];} BC250_PSP_FILES;
typedef struct {void* Psp;int GartLock,GpuStopUnconfirmed,Smu;} BC250_DEVICE;
typedef struct {void* pOutputData;ULONG OutputDataSize;} QUERY;
static NTSTATUS SmuReadFirmwareVersion(int* smu,ULONG* version)
{(void)smu;smuReads++;*version=smuReady?testSmuVersion:0;return smuReady?STATUS_SUCCESS:STATUS_DEVICE_NOT_READY;}
#include "firmware_metadata_actual.inc"
// Linking the real parser also links other shim functions. None may reach hardware.
unsigned int bc250_shim_rreg(struct amdgpu_device* adev,unsigned int index)
{(void)adev;(void)index;CHECK(0);return 0;}
void bc250_shim_wreg(struct amdgpu_device* adev,unsigned int index,unsigned int value)
{(void)adev;(void)index;(void)value;CHECK(0);}
void bc250_shim_udelay(unsigned int usec){(void)usec;CHECK(0);}
void bc250_shim_log(int level,void*dev,const char*format,...)
{(void)level;(void)dev;(void)format;CHECK(0);}
static int ReadFiles(const char* dir,BC250_PSP_FILES* files)
{
    unsigned i;
    for(i=0;i<BC250_FILE_COUNT;i++) {
        char name[1024];FILE* f;long size;
        snprintf(name,sizeof(name),"%s/%s",dir,bc250_fw_file_name((enum bc250_fw_file)i));
        f=fopen(name,"rb");if(!f)return 0;
        if(fseek(f,0,SEEK_END)||(size=ftell(f))<=0||size>1048576||fseek(f,0,SEEK_SET)){fclose(f);return 0;}
        files->Data[i]=malloc((size_t)size);files->Size[i]=(ULONG)size;
        if(!files->Data[i]||fread(files->Data[i],1,(size_t)size,f)!=(size_t)size){fclose(f);return 0;}
        fclose(f);
    }
    return 1;
}
int main(int argc,char**argv)
{
    BC250_PSP_FILES files={0};BC250_PSP psp={0};BC250_DEVICE d={0};
    struct bc250_umd_firmware fw,historical;
    unsigned char out[UMD_CAPS_BYTES],expected[UMD_CAPS_BYTES];
    QUERY q={out,sizeof(out)};unsigned i,before;
    struct gfx_firmware_header_v1_0* me;
    struct gfx_firmware_header_v1_0* mec2;
    if(argc!=2||!ReadFiles(argv[1],&files)){puts("cannot read firmware inputs");return 2;}
    memcpy(&historical,umd_caps_blob+UMD_CAPS_FIRMWARE_OFFSET,sizeof(historical));
    historical.smc_version=0;
    CHECK(ReadFirmwareMetadata(&files,&fw)==STATUS_SUCCESS);
    CHECK(!memcmp(&historical,&fw,sizeof(fw))); // real known firmware, all seven pairs
    me=(struct gfx_firmware_header_v1_0*)files.Data[BC250_FILE_ME];
    mec2=(struct gfx_firmware_header_v1_0*)files.Data[BC250_FILE_MEC2];
    me->header.ucode_version+=17;mec2->ucode_feature_version+=3;
    CHECK(ReadFirmwareMetadata(&files,&fw)==STATUS_SUCCESS);
    CHECK(fw.me_version==historical.me_version+17 && fw.mec2_feature==historical.mec2_feature+3);
    CHECK(fw.mec_feature==historical.mec_feature && !fw.smc_version && !fw.reserved);
    d.Psp=&psp;
    CompleteLoad(&psp,-1,10,11,fw);
    CHECK(!psp.Loaded && !psp.Firmware.me_version);
    memset(out,0xA5,sizeof(out));memcpy(expected,out,sizeof(out));
    CHECK(QueryCaps(&d,&q)==STATUS_DEVICE_NOT_READY && !memcmp(out,expected,sizeof(out)) && !smuReads);
    CompleteLoad(&psp,0,11,11,fw);
    CHECK(psp.Loaded && !memcmp(&psp.Firmware,&fw,sizeof(fw)));
    CHECK(QueryCaps(&d,&q)==STATUS_SUCCESS);
    memcpy(expected,umd_caps_blob,sizeof(expected));fw.smc_version=testSmuVersion;
    memcpy(expected+UMD_CAPS_FIRMWARE_OFFSET,&fw,sizeof(fw));
    CHECK(!memcmp(out,expected,sizeof(out)) && smuReads==1);
    testSmuVersion=0x00580701u;
    CHECK(QueryCaps(&d,&q)==STATUS_SUCCESS);
    fw.smc_version=testSmuVersion;memcpy(expected+UMD_CAPS_FIRMWARE_OFFSET,&fw,sizeof(fw));
    CHECK(!memcmp(out,expected,sizeof(out)) && smuReads==2);
    // A failed/absent owner never substitutes the historic firmware value.
    smuReady=0;memset(out,0xA5,sizeof(out));memcpy(expected,out,sizeof(out));
    CHECK(QueryCaps(&d,&q)==STATUS_DEVICE_NOT_READY && !memcmp(out,expected,sizeof(out)));
    smuReady=1;psp.Loaded=0;psp.PowerSuspended=1;
    CHECK(QueryCaps(&d,&q)==STATUS_SUCCESS); // retained image identity survives hardware suspension
    psp.PowerSuspended=0;memset(out,0xA5,sizeof(out));memcpy(expected,out,sizeof(out));
    before=smuReads;psp.Loaded=0;
    CHECK(QueryCaps(&d,&q)==STATUS_DEVICE_NOT_READY && smuReads==before);
    psp.Loaded=1;d.GpuStopUnconfirmed=1;
    CHECK(QueryCaps(&d,&q)==STATUS_DEVICE_NOT_READY && smuReads==before);
    d.GpuStopUnconfirmed=0;d.Psp=NULL;
    CHECK(QueryCaps(&d,&q)==STATUS_DEVICE_NOT_READY && smuReads==before);
    CHECK(!memcmp(out,expected,sizeof(out)));
    d.Psp=&psp;currentIrql=1;before=locks;
    CHECK(QueryCaps(&d,&q)==STATUS_INVALID_DEVICE_STATE && locks==before);
    currentIrql=0;q.OutputDataSize=sizeof(out)-1;
    CHECK(QueryCaps(&d,&q)==STATUS_BUFFER_TOO_SMALL && locks==before);
    q.OutputDataSize=sizeof(out);q.pOutputData=NULL;
    CHECK(QueryCaps(&d,&q)==STATUS_INVALID_PARAMETER && locks==before);
    for(i=0;i<BC250_FILE_COUNT;i++)free(files.Data[i]);
    printf("firmware metadata/query: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
