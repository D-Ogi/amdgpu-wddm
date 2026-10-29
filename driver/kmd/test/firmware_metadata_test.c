// Actual PSP metadata/cache and UMDRIVERPRIVATE branch with kernel lifetime mocks.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bc250_psp.h"
#include "../umd_caps.h"
#include "../../contract/bc250_umd_firmware.h"
// M15.14 increment 2: the branch now writes a second optional trailer, so the extracted case block needs
// its contract as well. BOOLEAN is the driver's; here it is the one field of the wddm stub below.
#include "../../contract/bc250_scanout_caps.h"
// BD-247: the trailer closes DirectFlip while the path is rotated.
#include "../display_rotation.h"
typedef long NTSTATUS;
typedef unsigned long ULONG;
typedef long LONG;
typedef unsigned char* PUCHAR;
typedef unsigned char BOOLEAN;
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
typedef struct {void* Psp;int GartLock,GpuStopUnconfirmed,Smu;
    struct {struct {unsigned int LowPart; int HighPart;} AdapterLuid;} StartInfo;
    // M15.14 increment 2: the POST mode the scan-out caps trailer publishes. The real member is
    // DXGK_DISPLAY_INFORMATION, of which this branch reads the two geometry fields.
    struct {ULONG Width,Height;} Post;
    // Display modes: the committed source size, 0 for none (the real one is BC250_MODESET.SourceWidth/Height).
    ULONG SourceWidth,SourceHeight;
    // M15.14 (0.7.216.20): whether this start captured the firmware's plane format (dcn.c), which the
    // trailer publishes as BC250_SCANOUT_CAPS_PLANE_FORMATS.
    BOOLEAN DcnPlaneFormats;
    // BD-197: a runtime mode of the connector is on screen, and its surface is valid (the real ones are volatile
    // LONGs of bc250kmd.h).
    volatile long RuntimeDisplayChanged,RuntimePostValid;
    struct {BOOLEAN InheritedFixed;} Modeset;
    ULONG Rotation;     // BD-247: the committed path rotation value (D3DKMDT_VIDPN_PRESENT_PATH_ROTATION)
} BC250_DEVICE;
/* BD-197 step 2: dcn.c's own DcnPlaneFormatsOpen, extracted as DcnPlaneFormatsOpenActual by the generator;
   dcn_flip_test.c holds the same function to the flip path. This wrapper counts the branch's questions. */
#define FALSE 0
#define TRUE 1
static long InterlockedCompareExchange(volatile long* p,long v,long c){long old=*p;if(old==c)*p=v;return old;}
static BOOLEAN DcnPlaneFormatsOpenActual(BC250_DEVICE* Device);
static unsigned planeFormatQuestions;
static BOOLEAN DcnPlaneFormatsOpen(BC250_DEVICE* d){planeFormatQuestions++;return DcnPlaneFormatsOpenActual(d);}
/* bc250kmd.h: the committed source size, else the POST size. */
static ULONG DisplaySourceWidth(const BC250_DEVICE* d){return d->SourceWidth?d->SourceWidth:d->Post.Width;}
static ULONG DisplaySourceHeight(const BC250_DEVICE* d){return d->SourceHeight?d->SourceHeight:d->Post.Height;}
typedef struct {void* pOutputData;ULONG OutputDataSize;} QUERY;
// The adapter's WDDM table, of which the extracted branch reads one start-latched byte: whether this
// start publishes the DirectFlip handshake. WddmStart computes it from the operator's switch ANDed with
// every start-latched fact the flip path needs; here it is set directly, which is what lets the test
// drive both answers of the branch.
// NodeCount is latched at WddmStart; with three nodes the branch opens node 2 in submittable_node_mask.
#define BC250_WDDM_NODE_COMPUTE 2u
// BD-247: ScanoutCapsLogged holds the trailer flags last logged, plus 1, so a line is written only on a change.
typedef struct {BOOLEAN DirectFlipHandshake;unsigned NodeCount;volatile long ScanoutCapsLogged;} BC250_WDDM;
static long InterlockedExchange(volatile long* p,long v){long old=*p;*p=v;return old;}
static unsigned trailerLines;
static void GuardLog(const char* format,...){(void)format;trailerLines++;}
static BC250_WDDM wddmStorage;
static BC250_WDDM* wddm=&wddmStorage;
static NTSTATUS SmuReadFirmwareVersion(int* smu,ULONG* version)
{(void)smu;smuReads++;*version=smuReady?testSmuVersion:0;return smuReady?STATUS_SUCCESS:STATUS_DEVICE_NOT_READY;}
// cumode.c: the CU fields follow this start's registers. The branch must hand it the whole template,
// after the template and the firmware section are in place.
static unsigned cuPatches,cuActive,vramPatches;
static void CuModePatchCaps(BC250_DEVICE* d,void* caps,ULONG bytes)
{(void)d;CHECK(bytes==UMD_CAPS_BYTES);CHECK(cuPatches==vramPatches);cuPatches++;if(cuActive)memcpy((PUCHAR)caps+UMD_CAPS_CU_ACTIVE_OFFSET,&cuActive,4);}
static void VramPatchCaps(const BC250_DEVICE* d,void* caps,ULONG bytes)
{
 ULONG observed=0;(void)d;CHECK(bytes==UMD_CAPS_BYTES);
 CHECK(cuPatches==vramPatches+1);vramPatches++;
 memcpy(&observed,(PUCHAR)caps+UMD_CAPS_CU_ACTIVE_OFFSET,4);
 if(cuActive)CHECK(observed==cuActive);
 CHECK(memcmp((PUCHAR)caps+UMD_CAPS_VRAM_TOTAL_OFFSET,
              umd_caps_blob+UMD_CAPS_VRAM_TOTAL_OFFSET,8)==0);
}
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
    before=cuPatches;cuActive=40;
    CHECK(QueryCaps(&d,&q)==STATUS_SUCCESS && cuPatches==before+1 && vramPatches==cuPatches);
    CHECK(!memcmp(out+UMD_CAPS_CU_ACTIVE_OFFSET,&cuActive,4));
    CHECK(!memcmp(out+UMD_CAPS_FIRMWARE_OFFSET,expected+UMD_CAPS_FIRMWARE_OFFSET,sizeof(fw)));
    cuActive=0;CHECK(QueryCaps(&d,&q)==STATUS_SUCCESS && !memcmp(out,expected,sizeof(out)));
    // Node 2 (compute-node.md section 8): with three nodes the mask gets bit 2 and no other byte changes. With two
    // nodes the answer is the template, as above.
    wddm->NodeCount=3;
    CHECK(QueryCaps(&d,&q)==STATUS_SUCCESS);
    CHECK(out[UMD_CAPS_SUBMITTABLE_NODE_MASK_OFFSET]==(unsigned char)(expected[UMD_CAPS_SUBMITTABLE_NODE_MASK_OFFSET]|4u));
    for(i=0;i<sizeof(out);i++) if(i!=UMD_CAPS_SUBMITTABLE_NODE_MASK_OFFSET) CHECK(out[i]==expected[i]);
    wddm->NodeCount=2;
    CHECK(QueryCaps(&d,&q)==STATUS_SUCCESS && !memcmp(out,expected,sizeof(out)));
    // The actual QueryAdapterInfo branch preserves the old prefix, writes no
    // partial identity, and does not overwrite caller-owned bytes after it.
    {
        unsigned char extended[BC250_ADAPTER_CAPS_BYTES+8];
        struct bc250_adapter_identity identity;
        QUERY extendedQuery={extended,BC250_ADAPTER_CAPS_BYTES};
        d.StartInfo.AdapterLuid.LowPart=0x12345678u;
        d.StartInfo.AdapterLuid.HighPart=-123;
        memset(extended,0xA5,sizeof(extended));
        CHECK(QueryCaps(&d,&extendedQuery)==STATUS_SUCCESS);
        CHECK(!memcmp(extended,expected,UMD_CAPS_BYTES));
        memcpy(&identity,extended+BC250_ADAPTER_IDENTITY_OFFSET,sizeof(identity));
        CHECK(identity.magic==BC250_ADAPTER_IDENTITY_MAGIC && identity.version==1 &&
              identity.size==sizeof(identity) && identity.reserved==0);
        CHECK(identity.luid_low==0x12345678u && identity.luid_high==(unsigned int)-123);
        for(i=BC250_ADAPTER_CAPS_BYTES;i<sizeof(extended);i++) CHECK(extended[i]==0xA5);
        for(unsigned int length=UMD_CAPS_BYTES;length<BC250_ADAPTER_CAPS_BYTES;length++) {
            memset(extended,0xA5,sizeof(extended));extendedQuery.OutputDataSize=length;
            CHECK(QueryCaps(&d,&extendedQuery)==STATUS_SUCCESS);
            CHECK(!memcmp(extended,expected,UMD_CAPS_BYTES));
            for(i=UMD_CAPS_BYTES;i<sizeof(extended);i++) CHECK(extended[i]==0xA5);
        }
        // Exact-size request and a new StartDevice identity must not return
        // a cached identity from the preceding adapter session.
        extendedQuery.OutputDataSize=BC250_ADAPTER_CAPS_BYTES;
        d.StartInfo.AdapterLuid.LowPart=99;d.StartInfo.AdapterLuid.HighPart=12;
        CHECK(QueryCaps(&d,&extendedQuery)==STATUS_SUCCESS);
        memcpy(&identity,extended+BC250_ADAPTER_IDENTITY_OFFSET,sizeof(identity));
        CHECK(identity.magic==BC250_ADAPTER_IDENTITY_MAGIC && identity.luid_low==99 && identity.luid_high==12);
        smuReady=0;memset(extended,0xA5,sizeof(extended));
        extendedQuery.OutputDataSize=sizeof(extended);
        CHECK(QueryCaps(&d,&extendedQuery)==STATUS_DEVICE_NOT_READY);
        for(i=0;i<sizeof(extended);i++) CHECK(extended[i]==0xA5);
        smuReady=1;
    }
    // M15.14 increment 2: the scan-out caps trailer, in the order its three rules can go wrong. This is
    // the only host coverage of the branch, and the first two of these were real defects: the trailer was
    // written with flags 0 while the switch was off, and it needed 1520 bytes while both shells queried
    // 1496, so nothing could ever read it.
    {
        unsigned char extended[BC250_SCANOUT_CAPS_TOTAL+8];
        struct bc250_scanout_caps scanout;
        struct bc250_adapter_identity identity;
        QUERY extendedQuery={extended,BC250_SCANOUT_CAPS_TOTAL};
        unsigned int length,j;
        d.Post.Width=1920;d.Post.Height=1200;
        // (1) With the switch off nothing is written here at all, at any buffer size: a start with the
        // handshake closed is byte for byte the revision before it, and a reader that keys on the magic
        // and forgets the flag has nothing to act on.
        wddm->DirectFlipHandshake=0;
        memset(extended,0xA5,sizeof(extended));
        CHECK(QueryCaps(&d,&extendedQuery)==STATUS_SUCCESS);
        CHECK(!memcmp(extended,expected,UMD_CAPS_BYTES));
        for(j=BC250_ADAPTER_CAPS_BYTES;j<sizeof(extended);j++) CHECK(extended[j]==0xA5);
        // (2) With it on, and a buffer that holds the whole trailer: every header field, the flag, the
        // POST geometry, the identity trailer before it untouched, the caller's bytes after it untouched.
        wddm->DirectFlipHandshake=1;
        memset(extended,0xA5,sizeof(extended));
        CHECK(QueryCaps(&d,&extendedQuery)==STATUS_SUCCESS);
        memcpy(&scanout,extended+BC250_SCANOUT_CAPS_OFFSET,sizeof(scanout));
        CHECK(scanout.magic==BC250_SCANOUT_CAPS_MAGIC && scanout.version==BC250_SCANOUT_CAPS_VERSION &&
              scanout.size==sizeof(scanout) && scanout.flags==BC250_SCANOUT_CAPS_DIRECT_FLIP);
        CHECK(scanout.post_width==1920 && scanout.post_height==1200);
        // BD-247: a rotated path clears the DirectFlip flag, so the shells compose; the identity and the values
        // that pin no rotation keep it.
        // The guard log gets one line when the answer changes and none for a repeated answer.
        trailerLines=0;
        for(j=2;j<=4;j++){
            d.Rotation=(ULONG)j;
            memset(extended,0xA5,sizeof(extended));
            CHECK(QueryCaps(&d,&extendedQuery)==STATUS_SUCCESS);
            memcpy(&scanout,extended+BC250_SCANOUT_CAPS_OFFSET,sizeof(scanout));
            CHECK(scanout.magic==BC250_SCANOUT_CAPS_MAGIC && !(scanout.flags&BC250_SCANOUT_CAPS_DIRECT_FLIP));
        }
        CHECK(trailerLines==1);
        d.Rotation=1;
        memset(extended,0xA5,sizeof(extended));
        CHECK(QueryCaps(&d,&extendedQuery)==STATUS_SUCCESS);
        memcpy(&scanout,extended+BC250_SCANOUT_CAPS_OFFSET,sizeof(scanout));
        CHECK(scanout.flags==BC250_SCANOUT_CAPS_DIRECT_FLIP);
        CHECK(trailerLines==2);
        d.Rotation=0;
        // Display modes: with a smaller source mode committed, the trailer carries that mode, the geometry
        // the kernel's admission uses now. Back at the native mode it carries the POST geometry again.
        d.SourceWidth=1920;d.SourceHeight=1080;
        memset(extended,0xA5,sizeof(extended));
        CHECK(QueryCaps(&d,&extendedQuery)==STATUS_SUCCESS);
        memcpy(&scanout,extended+BC250_SCANOUT_CAPS_OFFSET,sizeof(scanout));
        CHECK(scanout.post_width==1920 && scanout.post_height==1080);
        d.SourceWidth=0;d.SourceHeight=0;
        memset(extended,0xA5,sizeof(extended));
        CHECK(QueryCaps(&d,&extendedQuery)==STATUS_SUCCESS);
        memcpy(&scanout,extended+BC250_SCANOUT_CAPS_OFFSET,sizeof(scanout));
        CHECK(scanout.post_width==1920 && scanout.post_height==1200);
        // (2b) M15.14: the plane formats flag rides along only when the start captured the firmware's
        // plane format, and never without the DirectFlip flag (the branch writes nothing with that off).
        d.DcnPlaneFormats=1;
        memset(extended,0xA5,sizeof(extended));
        CHECK(QueryCaps(&d,&extendedQuery)==STATUS_SUCCESS);
        memcpy(&scanout,extended+BC250_SCANOUT_CAPS_OFFSET,sizeof(scanout));
        CHECK(scanout.flags==(BC250_SCANOUT_CAPS_DIRECT_FLIP|BC250_SCANOUT_CAPS_PLANE_FORMATS));
        // BD-197: the branch asks DcnPlaneFormatsOpen, the flip path's own rule (dcn.c's, extracted), once per
        // query. Whether the connector is enabled is not a question here: one that still shows its inherited mode
        // takes the extended formats (0.7.216.62 to 0.7.216.103 withheld them for every enabled connector, so DWM
        // composed every RGBA8 chain on DP). Step 2: a runtime mode of the connector with a valid surface is the
        // commit's own pipe and takes them too, over a fixed inherited pipe as well; a runtime mode without a valid
        // surface (a commit in progress, a detach) flips nothing and withholds them.
        planeFormatQuestions=0;
        d.RuntimeDisplayChanged=1;d.RuntimePostValid=1;
        memset(extended,0xA5,sizeof(extended));
        CHECK(QueryCaps(&d,&extendedQuery)==STATUS_SUCCESS);
        memcpy(&scanout,extended+BC250_SCANOUT_CAPS_OFFSET,sizeof(scanout));
        CHECK(scanout.flags==(BC250_SCANOUT_CAPS_DIRECT_FLIP|BC250_SCANOUT_CAPS_PLANE_FORMATS) &&
              planeFormatQuestions==1);
        d.Modeset.InheritedFixed=1;
        memset(extended,0xA5,sizeof(extended));
        CHECK(QueryCaps(&d,&extendedQuery)==STATUS_SUCCESS);
        memcpy(&scanout,extended+BC250_SCANOUT_CAPS_OFFSET,sizeof(scanout));
        CHECK(scanout.flags==(BC250_SCANOUT_CAPS_DIRECT_FLIP|BC250_SCANOUT_CAPS_PLANE_FORMATS) &&
              planeFormatQuestions==2);
        d.RuntimePostValid=0;
        memset(extended,0xA5,sizeof(extended));
        CHECK(QueryCaps(&d,&extendedQuery)==STATUS_SUCCESS);
        memcpy(&scanout,extended+BC250_SCANOUT_CAPS_OFFSET,sizeof(scanout));
        CHECK(scanout.flags==BC250_SCANOUT_CAPS_DIRECT_FLIP && planeFormatQuestions==3);
        d.Modeset.InheritedFixed=0;
        memset(extended,0xA5,sizeof(extended));
        CHECK(QueryCaps(&d,&extendedQuery)==STATUS_SUCCESS);
        memcpy(&scanout,extended+BC250_SCANOUT_CAPS_OFFSET,sizeof(scanout));
        CHECK(scanout.flags==BC250_SCANOUT_CAPS_DIRECT_FLIP);
        d.RuntimeDisplayChanged=0;
        memset(extended,0xA5,sizeof(extended));
        CHECK(QueryCaps(&d,&extendedQuery)==STATUS_SUCCESS);
        memcpy(&scanout,extended+BC250_SCANOUT_CAPS_OFFSET,sizeof(scanout));
        CHECK(scanout.flags==(BC250_SCANOUT_CAPS_DIRECT_FLIP|BC250_SCANOUT_CAPS_PLANE_FORMATS) &&
              planeFormatQuestions==5);
        // The fixed inherited pipe alone (a scaled source mode on the inherited timing) is ARGB8888 only.
        d.Modeset.InheritedFixed=1;
        memset(extended,0xA5,sizeof(extended));
        CHECK(QueryCaps(&d,&extendedQuery)==STATUS_SUCCESS);
        memcpy(&scanout,extended+BC250_SCANOUT_CAPS_OFFSET,sizeof(scanout));
        CHECK(scanout.flags==BC250_SCANOUT_CAPS_DIRECT_FLIP);
        d.Modeset.InheritedFixed=0;
        wddm->DirectFlipHandshake=0;
        memset(extended,0xA5,sizeof(extended));
        CHECK(QueryCaps(&d,&extendedQuery)==STATUS_SUCCESS);
        for(j=BC250_ADAPTER_CAPS_BYTES;j<sizeof(extended);j++) CHECK(extended[j]==0xA5);
        wddm->DirectFlipHandshake=1;d.DcnPlaneFormats=0;
        memset(extended,0xA5,sizeof(extended));
        CHECK(QueryCaps(&d,&extendedQuery)==STATUS_SUCCESS);
        memcpy(&scanout,extended+BC250_SCANOUT_CAPS_OFFSET,sizeof(scanout));
        CHECK(scanout.flags==BC250_SCANOUT_CAPS_DIRECT_FLIP);
        // Both together: a committed mode and the captured format.
        d.SourceWidth=1920;d.SourceHeight=1080;d.DcnPlaneFormats=1;
        memset(extended,0xA5,sizeof(extended));
        CHECK(QueryCaps(&d,&extendedQuery)==STATUS_SUCCESS);
        memcpy(&scanout,extended+BC250_SCANOUT_CAPS_OFFSET,sizeof(scanout));
        CHECK(scanout.flags==(BC250_SCANOUT_CAPS_DIRECT_FLIP|BC250_SCANOUT_CAPS_PLANE_FORMATS) &&
              scanout.post_width==1920 && scanout.post_height==1080);
        d.SourceWidth=0;d.SourceHeight=0;d.DcnPlaneFormats=0;
        memcpy(&identity,extended+BC250_ADAPTER_IDENTITY_OFFSET,sizeof(identity));
        CHECK(identity.magic==BC250_ADAPTER_IDENTITY_MAGIC && identity.size==sizeof(identity));
        CHECK(!memcmp(extended,expected,UMD_CAPS_BYTES));
        for(j=BC250_SCANOUT_CAPS_TOTAL;j<sizeof(extended);j++) CHECK(extended[j]==0xA5);
        // (3) Never a partial trailer. One byte short of the whole of it writes none of it, so the
        // 1496-byte query both shells make today sees nothing - which is exactly how the trailer came to
        // be unreachable. Asserted for every length from the identity trailer's end up to the scan-out
        // trailer's, with the switch still on.
        for(length=BC250_ADAPTER_CAPS_BYTES;length<BC250_SCANOUT_CAPS_TOTAL;length++) {
            memset(extended,0xA5,sizeof(extended));
            extendedQuery.OutputDataSize=length;
            CHECK(QueryCaps(&d,&extendedQuery)==STATUS_SUCCESS);
            CHECK(!memcmp(extended,expected,UMD_CAPS_BYTES));
            for(j=BC250_ADAPTER_CAPS_BYTES;j<sizeof(extended);j++) CHECK(extended[j]==0xA5);
        }
        // A geometry of zero never reaches a reader: WddmStart refuses to publish the flag without a POST
        // mode, so the branch is asked only with one. Kept here as the state the rest of the test wants.
        extendedQuery.OutputDataSize=BC250_SCANOUT_CAPS_TOTAL;
        wddm->DirectFlipHandshake=0;
        d.Post.Width=0;d.Post.Height=0;
    }
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
    // Foreign private protocols must receive no BC25 bytes or backend work.
    // In AGS, offset 8 of a successful response is an adapter count, not our size.
    {
        static const unsigned int foreign[]={1521,1528,3008,0x4360,0x5c0*0x3a00+0x960,0xffffffffu};
        unsigned int k;
        q.pOutputData=out;
        for(k=0;k<sizeof(foreign)/sizeof(foreign[0]);k++) {
            memset(out,0xA5,sizeof(out));memcpy(expected,out,sizeof(out));
            q.OutputDataSize=foreign[k];before=smuReads;
            CHECK(QueryCaps(&d,&q)==STATUS_INVALID_PARAMETER);
            CHECK(smuReads==before && !memcmp(out,expected,sizeof(out)));
        }
    }
    for(i=0;i<BC250_FILE_COUNT;i++)free(files.Data[i]);
    printf("firmware metadata/query: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
