#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "regs.generated.h"
#include "umd_caps.h"
#include "paging_window.h"
typedef void* PVOID;
typedef unsigned char* PUCHAR;
#define RtlCopyMemory(p,s,n) memcpy(p,s,n)
typedef unsigned long ULONG;
typedef unsigned long long ULONGLONG,u64;
typedef long long LONGLONG,LONG64;
typedef long NTSTATUS;
typedef int BOOLEAN;
#define TRUE 1
#define FALSE 0
#define true 1
#define NT_SUCCESS(x) ((x)>=0)
#define STATUS_SUCCESS 0L
#define STATUS_DEVICE_CONFIGURATION_ERROR (-10L)
#define MAXULONG 0xffffffffUL
#define MAXULONGLONG (~0ull)
#define BC250_VRAM_MAX_LENGTH (16ull<<30)
#define BC250_VRAM_TOP_RESERVED (32ull<<20)
#define BC250_GART_WINDOW (2ull<<20)
#define BC250_GART_SCRATCH_OFFSET 4096ull
#define GuardLog(...) ((void)0)
#define RtlZeroMemory(p,n) memset(p,0,n)
/* vram.c publishes the active board memory size for the board memory card while it sets the geometry.
   The host model keeps the written value so that nothing in the extracted code is a stub. */
static LONG64 InterlockedExchange64(volatile LONG64*target,LONG64 value){LONG64 old=*target;*target=value;return old;}
typedef struct {LONGLONG QuadPart;} PHYSICAL_ADDRESS;
typedef struct {
 struct {ULONGLONG bytes;} WddmAperture;
 void* Mmio;BOOLEAN VramEnabled,VramWriteEnabled;
 PHYSICAL_ADDRESS VramPhysical,Bar0Physical;ULONGLONG VramLength,VramMcBase,Bar0Length;
 struct {ULONG Pitch,Height;PHYSICAL_ADDRESS PhysicAddress;} Post;
 volatile LONG64 UmaActiveBytes;
} BC250_DEVICE;
struct fake_adev {void *dev,*backend;struct {ULONGLONG mc_vram_size,real_vram_size,vram_start;} gmc;};
struct bc250_gmc_inputs {u64 gart_table_mc,mem_scratch_mc,dummy_page_dma;int noretry;};
typedef struct {const BC250_DEVICE* Device;struct fake_adev Adev;int TableBo,Sequence,SetUp;PHYSICAL_ADDRESS DummyPhysical;} BC250_GART;
static unsigned checks,failures,os_checks;
static ULONG gc_gib,nbio_mib;
static int missing_bar,shim_mismatch;
#define CHECK(x) do {checks++;if(!(x)){failures++;printf("FAIL line%d: %s\n",__LINE__,#x);}}while(0)
static ULONG GuardReadSetting(const wchar_t* name,ULONG fallback){(void)name;(void)fallback;return 1;}
static NTSTATUS MmioRead(const BC250_DEVICE* d,ULONG offset,ULONG* value){
 (void)d;
 switch(offset){
 case BC250_REG_GC_GCMC_VM_FB_OFFSET:*value=0x270;break;
 case BC250_REG_GC_GCMC_VM_FB_LOCATION_BASE:*value=0x8000;break;
 case BC250_REG_GC_GCMC_VM_FB_LOCATION_TOP:*value=0x8000+gc_gib*64-1;break;
 case BC250_REG_NBIO_RCC_DEV0_EPF0_RCC_CONFIG_MEMSIZE:*value=nbio_mib;break;
 default:CHECK(0);return -1;
 }return 0;
}
static BOOLEAN IsOutsideOsMemory(ULONGLONG start,ULONGLONG length){(void)start;(void)length;os_checks++;return TRUE;}
static NTSTATUS FindBar0(BC250_DEVICE* d,PHYSICAL_ADDRESS* start,ULONGLONG* length){
 (void)d;if(missing_bar)return -1;start->QuadPart=0xc0000000ull;*length=256ull<<20;return 0;
}
static int bc250_gmc_setup(struct fake_adev* adev,const struct bc250_gmc_inputs* in,int* bo){
 const BC250_DEVICE* d=adev->dev;(void)bo;
 CHECK(in->gart_table_mc==d->VramMcBase+d->VramLength-BC250_GART_WINDOW);
 adev->gmc.mc_vram_size=d->VramLength;adev->gmc.real_vram_size=d->VramLength;adev->gmc.vram_start=d->VramMcBase;
 if(shim_mismatch==1)adev->gmc.mc_vram_size+=1ull<<30;
 if(shim_mismatch==2)adev->gmc.real_vram_size+=1ull<<30;
 if(shim_mismatch==3)adev->gmc.vram_start+=1ull<<24;
 return 0;
}
/* ACTUAL_SOURCE */
static void init(BC250_DEVICE* d,ULONG gib){
 memset(d,0,sizeof(*d));d->WddmAperture.bytes=384ull<<20;d->Mmio=d;d->Post.Pitch=7680;d->Post.Height=1200;
 d->Post.PhysicAddress.QuadPart=0xc0000000ull;gc_gib=gib;nbio_mib=gib*1024;missing_bar=0;shim_mismatch=0;os_checks=0;
}
static void check_caps(BC250_DEVICE* d, ULONGLONG expected)
{
 unsigned char bytes[UMD_CAPS_BYTES+17], original[sizeof(bytes)];
 ULONGLONG local=0,visible=0;unsigned i;
 memset(bytes,0xA5,sizeof(bytes));memcpy(original,bytes,sizeof(bytes));
 VramPatchCaps(d,bytes,UMD_CAPS_BYTES);
 memcpy(&local,bytes+UMD_CAPS_VRAM_TOTAL_OFFSET,8);
 memcpy(&visible,bytes+UMD_CAPS_VISIBLE_VRAM_TOTAL_OFFSET,8);
 CHECK(local==expected && visible==expected);
 CHECK((local>>32)==(expected>>32));
 for(i=0;i<sizeof(bytes);i++)
  if(!((i>=464 && i<472)||(i>=496 && i<504))) CHECK(bytes[i]==original[i]);
 for(i=0;i<UMD_CAPS_BYTES;i++) {
  memcpy(bytes,original,sizeof(bytes));VramPatchCaps(d,bytes,i);
  CHECK(memcmp(bytes,original,sizeof(bytes))==0);
 }
 VramPatchCaps(d,NULL,UMD_CAPS_BYTES);
}
static void caps_cases(void)
{
 /* Independent fixtures: POST occupies 9,216,000 bytes, rounded to 9,240,576;
    the 32 MiB tail and rounded 1/32 page-table reservation are excluded. */
 static const struct {ULONG gib;ULONGLONG local;} cases[]={
  {4,4119330816ull},{8,8280080384ull},{12,12440829952ull},{16,16601579520ull}};
 unsigned i;BC250_DEVICE d;
 for(i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
  init(&d,cases[i].gib);CHECK(VramStart(&d)==0);check_caps(&d,cases[i].local);
 }
 init(&d,8);CHECK(VramStart(&d)==0);
 d.Post.Pitch=8192;d.Post.Height=8192;check_caps(&d,8223981568ull);
 d.WddmAperture.bytes=0;check_caps(&d,0);
 d.WddmAperture.bytes=(384ull<<20)+4096;check_caps(&d,0);
 d.WddmAperture.bytes=448ull<<20;check_caps(&d,0);
 d.WddmAperture.bytes=256ull<<20;check_caps(&d,8223981568ull);
 d.WddmAperture.bytes=447ull<<20;check_caps(&d,8223981568ull);
 d.Post.Height=0;check_caps(&d,0);
 init(&d,8);CHECK(VramStart(&d)==0);d.VramEnabled=FALSE;check_caps(&d,0);
 init(&d,8);CHECK(VramStart(&d)==0);d.Post.PhysicAddress.QuadPart=1;check_caps(&d,0);
}
int main(void){
 ULONG gib;
 caps_cases();
 for(gib=8;gib<=16;gib+=4){
  BC250_DEVICE d;BC250_GART g;ULONGLONG off,len,table,tablelen;int mismatch;
  init(&d,gib);CHECK(VramStart(&d)==0);CHECK(d.VramEnabled && d.VramWriteEnabled && d.VramLength==((ULONGLONG)gib<<30));
  CHECK(os_checks==1);CHECK(WddmMemoryLayout(&d,&off,&len,&table,&tablelen));
  CHECK(off>=((ULONGLONG)d.Post.Pitch*d.Post.Height) && len>0 && table==off+len);
  CHECK(table+tablelen<=d.VramLength-BC250_VRAM_TOP_RESERVED);
  memset(&g,0,sizeof(g));g.Device=&d;CHECK(RunSetup(&g)==0 && g.SetUp);
  for(mismatch=1;mismatch<=3;mismatch++) {shim_mismatch=mismatch;CHECK(RunSetup(&g)!=0 && !g.SetUp);}
  init(&d,gib);nbio_mib=(gib==8?12:8)*1024;
  CHECK(VramStart(&d)==STATUS_DEVICE_CONFIGURATION_ERROR);
  CHECK(!d.VramEnabled && !d.VramWriteEnabled && !d.VramLength && !os_checks);
  CHECK(!WddmMemoryLayout(&d,&off,&len,&table,&tablelen));
  CHECK(!off && !len && !table && !tablelen);
  init(&d,gib);nbio_mib=0;CHECK(VramStart(&d)==STATUS_DEVICE_CONFIGURATION_ERROR && !d.VramEnabled);
  init(&d,gib);missing_bar=1;CHECK(VramStart(&d)==0 && d.VramEnabled);
  CHECK(!WddmMemoryLayout(&d,&off,&len,&table,&tablelen));
  CHECK(!off && !len && !table && !tablelen);
  d.Post.PhysicAddress=d.VramPhysical;CHECK(WddmMemoryLayout(&d,&off,&len,&table,&tablelen));
  d.Post.Height=0;CHECK(!WddmMemoryLayout(&d,&off,&len,&table,&tablelen));
 }
 printf("VRAM geometry: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
