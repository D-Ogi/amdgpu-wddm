// Compile production functions; emulate only kernel mapping/MMIO services.
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"
#include "dcn_translate.h"
typedef unsigned long ULONG,*PULONG;
typedef unsigned long long ULONGLONG;
typedef long long LONGLONG,LONG64;
typedef long LONG,NTSTATUS;
typedef int BOOLEAN;
typedef void *PVOID,*HANDLE;
typedef unsigned char UCHAR;
typedef size_t SIZE_T;
typedef struct {LONGLONG QuadPart;} PHYSICAL_ADDRESS;
#define TRUE 1
#define FALSE 0
#define NT_SUCCESS(x) ((x)>=0)
#define STATUS_SUCCESS 0L
#define STATUS_INVALID_PARAMETER (-1L)
#define STATUS_DEVICE_CONFIGURATION_ERROR (-2L)
#define STATUS_INSUFFICIENT_RESOURCES (-3L)
#define PAGE_READWRITE 4
#define BC250_DCNFLIP_BORDER 64
#define BC250_DCN_LOG_CALLS 8
#define MAXULONG 0xfffffffful
#define GuardLog(...) ((void)0)
#define RtlZeroMemory(p,n) memset(p,0,n)
#define RtlCopyMemory(d,s,n) memcpy(d,s,n)
#define WRITE_REGISTER_ULONG(p,v) (*(p)=(v))
typedef struct {
 struct {ULONG Width,Height,Pitch;} Post;
 PHYSICAL_ADDRESS VramPhysical;
 ULONGLONG VramLength,DcnFirmwareAddress,DcnCurrentAddress,DcnScanoutMapAddress,DcnScanoutSeedAddress;
 ULONG DcnFirmwarePitch,DcnCurrentPitch;
 int VramEnabled,DcnFirmwareKnown,DcnDiverged;
 volatile LONG DcnSurfaceSequence,DcnScanoutMapFailed,DcnScanoutRemaps;
 SIZE_T FramebufferLength,DcnScanoutMapLength;
 PVOID DcnScanoutMap;
} BC250_DEVICE;
static unsigned checks,failures,maps,unmaps;
#define CHECK(x) do{++checks;if(!(x)){++failures;printf("FAIL %d: %s\n",__LINE__,#x);}}while(0)
static LONG InterlockedCompareExchange(volatile LONG *p,LONG v,LONG c){LONG x=*p;if(x==c)*p=v;return x;}
static LONG64 InterlockedCompareExchange64(volatile LONG64 *p,LONG64 v,LONG64 c){LONG64 x=*p;if(x==c)*p=v;return x;}
static LONG InterlockedIncrement(volatile LONG *p){return ++*p;}
static UCHAR *buffer;
static SIZE_T mapped_bytes;
static ULONGLONG fw_address;
static ULONG fw_pitch;
static NTSTATUS MmioDcnRead(const BC250_DEVICE *d,ULONG reg,ULONG *value){
 (void)d;
 if(reg==BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS)*value=(ULONG)fw_address;
 else if(reg==BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH)*value=(ULONG)(fw_address>>32);
 else{CHECK(reg==BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_PITCH);*value=(fw_pitch/4-1)|0xA5000000ul;}
 return STATUS_SUCCESS;
}
static PVOID VramMapCpuRange(const BC250_DEVICE *d,PHYSICAL_ADDRESS p,SIZE_T n,ULONG flags){
 (void)d;(void)p;(void)flags;CHECK(n<=20u*1024*1024);memset(buffer,0xcd,n+128);mapped_bytes=n;maps++;return buffer+64;
}
static void MmUnmapIoSpace(PVOID p,SIZE_T n){
 unsigned i;CHECK(p==buffer+64 && n==mapped_bytes);for(i=0;i<64;i++)CHECK(buffer[i]==0xcd && buffer[64+n+i]==0xcd);unmaps++;
}
typedef struct {ULONG Magic,Version,Width,Height,Pitch,Format;ULONGLONG Size;} BC250_WDDM_ALLOCATION_PRIVATE;
typedef struct {ULONG Width,Height,Pitch,Format;} STANDARD_DATA;
typedef struct {
 ULONG StandardAllocationType,AllocationPrivateDriverDataSize,ResourcePrivateDriverDataSize;
 STANDARD_DATA *pCreateSharedPrimarySurfaceData,*pCreateShadowSurfaceData,*pCreateStagingSurfaceData,*pCreateGdiSurfaceData;
 PVOID pAllocationPrivateDriverData,pResourcePrivateDriverData;
} DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA;
typedef int BC250_WDDM;
#define BC250_WDDM_ALLOCATION_PRIVATE_MAGIC 123
#define D3DKMDT_STANDARDALLOCATION_SHAREDPRIMARYSURFACE 1
#define D3DKMDT_STANDARDALLOCATION_SHADOWSURFACE 2
#define D3DKMDT_STANDARDALLOCATION_STAGINGSURFACE 3
#define D3DKMDT_STANDARDALLOCATION_GDISURFACE 4
#define D3DDDIFMT_A8R8G8B8 21
#define WddmDdiGetStandardAllocationDriverData 0
static BC250_WDDM *WddmOf(HANDLE h){(void)h;return NULL;}
static BOOLEAN WddmFirstCalls(BC250_WDDM *w,int call){(void)w;(void)call;return FALSE;}
/* ACTUAL_SOURCE */
int main(void){
 const ULONG widths[]={1366,1440,1680,1920,2560};
 const ULONG pitches[]={5632,5888,6912,7680,10240};
 const ULONG heights[]={768,900,1050,1200,1440};
 unsigned i;buffer=malloc(20u*1024*1024+128);if(!buffer)return 2;
 for(i=0;i<5;i++){
  BC250_DEVICE d={0};BC250_WDDM_ALLOCATION_PRIVATE a={0};
  STANDARD_DATA data={0};DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA request={0};
  ULONG pitch=0,y;SIZE_T length=0;PVOID mapping=NULL;ULONGLONG bytes=(ULONGLONG)pitches[i]*heights[i];
  data.Width=widths[i];data.Height=heights[i];data.Format=D3DDDIFMT_A8R8G8B8;
  request.StandardAllocationType=D3DKMDT_STANDARDALLOCATION_SHAREDPRIMARYSURFACE;
  request.pCreateSharedPrimarySurfaceData=&data;
  CHECK(Bc250WddmGetStandardAllocationDriverData(NULL,&request)==STATUS_SUCCESS);
  CHECK(request.AllocationPrivateDriverDataSize==sizeof(a));
  request.pAllocationPrivateDriverData=&a;
  CHECK(Bc250WddmGetStandardAllocationDriverData(NULL,&request)==STATUS_SUCCESS);
  CHECK(a.Pitch==pitches[i] && a.Size==bytes);
  request.StandardAllocationType=D3DKMDT_STANDARDALLOCATION_SHADOWSURFACE;request.pCreateShadowSurfaceData=&data;
  CHECK(Bc250WddmGetStandardAllocationDriverData(NULL,&request)==STATUS_SUCCESS);
  CHECK(a.Pitch==widths[i]*4 && data.Pitch==a.Pitch);
  d.Post.Width=widths[i];d.Post.Height=heights[i];d.Post.Pitch=pitches[i]+256;
  d.FramebufferLength=(SIZE_T)d.Post.Pitch*d.Post.Height;d.VramEnabled=1;d.VramPhysical.QuadPart=0x100000000ll;d.VramLength=8ull<<30;
  fw_address=(ULONGLONG)d.VramPhysical.QuadPart;fw_pitch=d.Post.Pitch;
  CHECK(CaptureFirmwareSurface(&d)==STATUS_SUCCESS);
  CHECK(d.DcnFirmwarePitch==fw_pitch && d.DcnFirmwareAddress==fw_address);
  d.DcnCurrentAddress=fw_address+0x1000000;d.DcnCurrentPitch=pitches[i];d.DcnDiverged=1;
  CHECK(AddressAllowed(&d,d.DcnCurrentAddress,pitches[i]));
  CHECK(!AddressAllowed(&d,fw_address+d.VramLength-4096,pitches[i]));
  CHECK(AddressAllowed(&d,fw_address,fw_pitch));CHECK(!AddressAllowed(&d,fw_address,pitches[i]));
  CHECK(FillSurface(&d,d.DcnCurrentAddress,pitches[i],0xff00aa55)==STATUS_SUCCESS);
  CHECK(mapped_bytes==bytes);
  CHECK(*(ULONG*)(buffer+64)==0xfffffffful);
  CHECK(*(ULONG*)(buffer+64+(SIZE_T)(heights[i]-1)*pitches[i]+(widths[i]-1)*4)==0xfffffffful);
  CHECK(*(ULONG*)(buffer+64+(SIZE_T)128*pitches[i]+(widths[i]-128)*4)==0xff00aa55ul);
  if(pitches[i]>widths[i]*4)for(y=0;y<heights[i];y++)CHECK(buffer[64+(SIZE_T)y*pitches[i]+widths[i]*4]==0xcd);
  CHECK(DcnScanoutMapping(&d,&mapping,&length,&pitch));CHECK(length==bytes && pitch==pitches[i]);
  {unsigned count=maps;CHECK(DcnScanoutMapping(&d,&mapping,&length,&pitch));CHECK(count==maps);}
  d.DcnScanoutSeedAddress=d.DcnCurrentAddress;d.DcnCurrentPitch+=256;
  CHECK(DcnScanoutMapping(&d,&mapping,&length,&pitch));
  CHECK(length==(SIZE_T)(pitches[i]+256)*heights[i] && pitch==pitches[i]+256 && !d.DcnScanoutSeedAddress);
  d.DcnSurfaceSequence=1;CHECK(!DcnScanoutMapping(&d,&mapping,&length,&pitch));CHECK(!mapping && !length && !pitch);d.DcnSurfaceSequence=2;
  DcnUnmapScanout(&d);
  // Firmware pitch is wider than destination; last row must still be copied.
  {SIZE_T srcsize=d.FramebufferLength;UCHAR *src=malloc(srcsize);UCHAR *dst=malloc((SIZE_T)bytes+128);int exact=1;
   if(!src || !dst)return 2;
   memset(dst,0xcd,(SIZE_T)bytes+128);
   for(y=0;y<heights[i];y++)memset(src+(SIZE_T)y*d.Post.Pitch,(int)(y%251),(SIZE_T)d.Post.Pitch);
   CHECK(WddmCopyScanoutRows(dst+64,(SIZE_T)bytes,pitches[i],src,srcsize,d.Post.Pitch,widths[i],heights[i]));
   for(y=0;y<heights[i];y++)if(memcmp(dst+64+(SIZE_T)y*pitches[i],src+(SIZE_T)y*d.Post.Pitch,(SIZE_T)widths[i]*4))exact=0;
   CHECK(exact);
   for(y=0;y<64;y++)CHECK(dst[y]==0xcd && dst[64+(SIZE_T)bytes+y]==0xcd);
   if(pitches[i]>widths[i]*4)for(y=0;y<heights[i];y++)CHECK(dst[64+(SIZE_T)y*pitches[i]+widths[i]*4]==0xcd);
   CHECK(!WddmCopyScanoutRows(dst+64,(SIZE_T)bytes-1,pitches[i],src,srcsize,d.Post.Pitch,widths[i],heights[i]));
   free(src);free(dst);
  }
 }
 free(buffer);CHECK(maps==unmaps);
 printf("scanout geometry: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
