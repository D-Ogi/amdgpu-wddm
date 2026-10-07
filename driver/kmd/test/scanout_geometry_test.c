// Compile production functions; emulate only kernel mapping/MMIO services.
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"
#include "dcn_translate.h"
#include "gdi_private.h"    /* the LB7A blob and its GDI trailer, as wddm.c includes them: never a copy here */
#include "gdi_admission.h"  /* the request, the answer and the per-adapter counters of the DDI, likewise */
typedef unsigned long ULONG,*PULONG;
typedef unsigned int UINT;
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
/* Display modes (bc250kmd.h): the committed source size, else the inherited one. No mode is committed here. */
static ULONG DisplaySourceWidth(const BC250_DEVICE *d){return d->Post.Width;}
static ULONG DisplaySourceHeight(const BC250_DEVICE *d){return d->Post.Height;}
static unsigned checks,failures,maps,unmaps;
#define CHECK(x) do{++checks;if(!(x)){++failures;printf("FAIL %d: %s\n",__LINE__,#x);}}while(0)
static LONG InterlockedCompareExchange(volatile LONG *p,LONG v,LONG c){LONG x=*p;if(x==c)*p=v;return x;}
static LONG64 InterlockedCompareExchange64(volatile LONG64 *p,LONG64 v,LONG64 c){LONG64 x=*p;if(x==c)*p=v;return x;}
static LONG InterlockedIncrement(volatile LONG *p){return ++*p;}
static LONG InterlockedOr(volatile LONG *p,LONG v){LONG x=*p;*p=x|v;return x;}
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
/* One shape for the four D3DKMDT_*SURFACEDATA structs; Type and Flags are the GDI one's. */
typedef struct {ULONG Width,Height,Pitch,Format,Type;struct {ULONG Value;} Flags;} STANDARD_DATA;
typedef struct {
 ULONG StandardAllocationType,AllocationPrivateDriverDataSize,ResourcePrivateDriverDataSize;
 STANDARD_DATA *pCreateSharedPrimarySurfaceData,*pCreateShadowSurfaceData,*pCreateStagingSurfaceData,*pCreateGdiSurfaceData;
 PVOID pAllocationPrivateDriverData,pResourcePrivateDriverData;
} DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA;
typedef struct {volatile LONG GdiSurfaceTypesLogged;BC250_STDALLOC_COUNTERS StdAlloc;} BC250_WDDM;
#define D3DKMDT_STANDARDALLOCATION_SHAREDPRIMARYSURFACE 1
#define D3DKMDT_STANDARDALLOCATION_SHADOWSURFACE 2
#define D3DKMDT_STANDARDALLOCATION_STAGINGSURFACE 3
#define D3DKMDT_STANDARDALLOCATION_GDISURFACE 4
#define D3DDDIFMT_A8R8G8B8 21
#define WddmDdiGetStandardAllocationDriverData 0
static BC250_WDDM adapter;
static BC250_WDDM *WddmOf(HANDLE h){return h ? &adapter : NULL;}
static BOOLEAN WddmFirstCalls(BC250_WDDM *w,int call){(void)w;(void)call;return FALSE;}
/* ACTUAL_SOURCE */
/* Standard GDI surfaces: the DDI's blob is the LB7A prefix plus the GDI trailer, laid out by gdi_private.h, and it
   must be exactly what the KMD's own admission (WddmSurfaceAdmitted) later accepts from the UMD's OpenResource. */
static NTSTATUS GdiRequest(STANDARD_DATA *data,BC250_GDI_PRIVATE *gdi,ULONG bytes){
 DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA request={0};
 NTSTATUS status;
 request.StandardAllocationType=D3DKMDT_STANDARDALLOCATION_GDISURFACE;request.pCreateGdiSurfaceData=data;
 status=Bc250WddmGetStandardAllocationDriverData((HANDLE)&adapter,&request);
 if(!NT_SUCCESS(status))return status;
 CHECK(request.AllocationPrivateDriverDataSize==sizeof(*gdi) && !request.pResourcePrivateDriverData);
 request.pAllocationPrivateDriverData=gdi;request.AllocationPrivateDriverDataSize=bytes;
 return Bc250WddmGetStandardAllocationDriverData((HANDLE)&adapter,&request);
}
static void GdiChecks(void){
 const ULONG types[]={1,2,3,4};
 const ULONG sizes[][2]={{1366,768},{257,3}};
 unsigned i,j;
 static_assert(sizeof(BC250_WDDM_ALLOCATION_PRIVATE)==32 && sizeof(BC250_GDI_PRIVATE)==48,"LB7A v1 prefix and GDI trailer");
 for(i=0;i<4;i++)for(j=0;j<2;j++){
  STANDARD_DATA data={0};BC250_GDI_PRIVATE gdi;ULONG pitch=0,t=0;ULONGLONG size=0;
  ULONG format=types[i]==4 ? AMDGPU_WDDM_D3DDDI_A8 : D3DDDIFMT_A8R8G8B8;
  memset(&gdi,0xcc,sizeof(gdi));
  data.Width=sizes[j][0];data.Height=sizes[j][1];data.Format=format;data.Type=types[i];
  CHECK(WddmGdiLayout(data.Width,data.Height,types[i],types[i]==4 ? 1 : 4,&pitch,&size));
  CHECK(GdiRequest(&data,&gdi,sizeof(gdi))==STATUS_SUCCESS);
  CHECK(gdi.Surface.Magic==BC250_WDDM_ALLOCATION_PRIVATE_MAGIC && gdi.Surface.Version==1);
  CHECK(gdi.Surface.Width==data.Width && gdi.Surface.Height==data.Height && gdi.Surface.Format==format);
  CHECK(gdi.Surface.Pitch==pitch && gdi.Surface.Size==size && data.Pitch==pitch);
  CHECK(gdi.Magic==BC250_GDI_PRIVATE_MAGIC && gdi.Type==types[i] && gdi.Flags==0 && gdi.Reserved==0);
  CHECK(WddmGdiPrivate(&gdi,sizeof(gdi),&t) && t==types[i]);
  CHECK(WddmSurfaceAdmitted(&gdi.Surface,types[i]));
  CHECK((adapter.GdiSurfaceTypesLogged>>(types[i]*2)&3)==3);   /* the size query and the fill, each logged once */
  CHECK(GdiRequest(&data,&gdi,sizeof(gdi.Surface))==STATUS_INVALID_PARAMETER);   /* the v1 prefix alone is too small */
 }
 {
  STANDARD_DATA data={0};BC250_GDI_PRIVATE gdi;
  data.Width=64;data.Height=64;data.Format=D3DDDIFMT_A8R8G8B8;data.Type=2;data.Flags.Value=1;
  CHECK(GdiRequest(&data,&gdi,sizeof(gdi))==STATUS_INVALID_PARAMETER);           /* no GDI flag is implemented */
  data.Flags.Value=0;data.Type=0;CHECK(GdiRequest(&data,&gdi,sizeof(gdi))==STATUS_INVALID_PARAMETER);
  data.Type=5;CHECK(GdiRequest(&data,&gdi,sizeof(gdi))==STATUS_INVALID_PARAMETER); /* existing system memory */
  data.Type=4;CHECK(GdiRequest(&data,&gdi,sizeof(gdi))==STATUS_INVALID_PARAMETER); /* a lookup table is A8 only */
  data.Type=1;data.Format=AMDGPU_WDDM_D3DDDI_A8;CHECK(GdiRequest(&data,&gdi,sizeof(gdi))==STATUS_INVALID_PARAMETER);
  data.Type=2;data.Format=0;CHECK(GdiRequest(&data,&gdi,sizeof(gdi))==STATUS_INVALID_PARAMETER);
 }
}
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
  CHECK(AddressAllowed(&d,d.DcnCurrentAddress,pitches[i],widths[i],heights[i]));
  CHECK(!AddressAllowed(&d,fw_address+d.VramLength-4096,pitches[i],widths[i],heights[i]));
  CHECK(AddressAllowed(&d,fw_address,fw_pitch,widths[i],heights[i]));CHECK(!AddressAllowed(&d,fw_address,pitches[i],widths[i],heights[i]));
  /* Display modes: the size checked is the source mode's. A one-row surface fits in the last page of VRAM, where
   * the full size does not; a zero size is no surface. */
  CHECK(AddressAllowed(&d,fw_address+d.VramLength-4096,1024,256,1));
  CHECK(!AddressAllowed(&d,fw_address+d.VramLength-4096,1024,256,2+4096/1024));
  CHECK(!AddressAllowed(&d,d.DcnCurrentAddress,pitches[i],0,heights[i]));
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
 GdiChecks();
 free(buffer);CHECK(maps==unmaps);
 printf("scanout geometry: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
