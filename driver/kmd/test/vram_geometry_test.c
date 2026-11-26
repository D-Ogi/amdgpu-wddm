#include <stdio.h>
#include <stdarg.h>
#include "display_start.h"     /* BD-341: the display-start state */
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
#define STATUS_CONFLICTING_ADDRESSES (-11L)
#define STATUS_INVALID_PARAMETER (-12L)
typedef unsigned char UCHAR;
#define MAXULONG 0xffffffffUL
#define MAXULONGLONG (~0ull)
#define BC250_VRAM_MAX_LENGTH (16ull<<30)
#define BC250_VRAM_TOP_RESERVED (32ull<<20)
#define BC250_GART_WINDOW (2ull<<20)
#define BC250_GART_SCRATCH_OFFSET 4096ull
#define BC250_VIDMM_GIANT_PAGE_BYTES (1024ull*1024*1024)
#define BC250_VIDMM_HUGE_PAGE_BYTES (2ull*1024*1024)
#define BC250_LARGE_PAGE_LAYOUT_NONE 0u
#define BC250_LARGE_PAGE_LAYOUT_GIANT 1u
#define BC250_LARGE_PAGE_LAYOUT_HUGE 2u
#define BC250_TABLE_BUDGET_CAP (64ull*1024*1024)
/* The log of the extracted code, kept: the start log must name the layout in force, and the test reads it. */
static char log_lines[8][200];
static unsigned log_count;
static void GuardLog(const char* format,...){
 va_list a;va_start(a,format);vsnprintf(log_lines[log_count%8],sizeof(log_lines[0]),format,a);va_end(a);log_count++;
}
static int logged(const char* text){unsigned i;for(i=0;i<8;i++)if(strstr(log_lines[i],text))return 1;return 0;}
static void log_reset(void){memset(log_lines,0,sizeof(log_lines));log_count=0;}
#define RtlZeroMemory(p,n) memset(p,0,n)
/* vram.c publishes the active board memory size for the board memory card while it sets the geometry.
   The host model keeps the written value so that nothing in the extracted code is a stub. */
static LONG64 InterlockedExchange64(volatile LONG64*target,LONG64 value){LONG64 old=*target;*target=value;return old;}
typedef struct {LONGLONG QuadPart;} PHYSICAL_ADDRESS;
typedef struct {
 struct {ULONGLONG bytes;} WddmAperture;
 void* Mmio;BOOLEAN VramEnabled,VramWriteEnabled;
 PHYSICAL_ADDRESS VramPhysical,Bar0Physical;ULONGLONG VramLength,VramMcBase,Bar0Length,DisplayReservationEnd;
 UCHAR LargePageLayout;BOOLEAN LargePages;
 struct {ULONG Pitch,Height;PHYSICAL_ADDRESS PhysicAddress;} Post;
 unsigned long DisplayStart;          /* BD-341: display_start.h */
 unsigned long ColdScanoutBytes;      /* BD-341: what the firmware can still scan out of at VRAM offset 0 */
 ULONGLONG FramebufferLength;         /* display.c: Post.Pitch * Post.Height from the map to the stop, else 0 */
 volatile LONG64 UmaActiveBytes;
} BC250_DEVICE;
/* The members of the shim's translation context that VidMmLargePageContext reads and writes (bc250_pte.h). */
struct bc250_pte_context {ULONGLONG vram_base;unsigned large_pages,huge_pages_only;};
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
    the 32 MiB tail and the capped page-table reservation (64 MiB) are excluded. */
 static const struct {ULONG gib;ULONGLONG local;} cases[]={
  {4,4185063424ull},{8,8480030720ull},{12,12774998016ull},{16,17069965312ull}};
 unsigned i;BC250_DEVICE d;
 for(i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
  init(&d,cases[i].gib);CHECK(VramStart(&d)==0);check_caps(&d,cases[i].local);
 }
 init(&d,8);CHECK(VramStart(&d)==0);
 d.Post.Pitch=8192;d.Post.Height=8192;check_caps(&d,8422162432ull);
 d.WddmAperture.bytes=0;check_caps(&d,0);
 d.WddmAperture.bytes=(384ull<<20)+4096;check_caps(&d,0);
 d.WddmAperture.bytes=448ull<<20;check_caps(&d,0);
 d.WddmAperture.bytes=256ull<<20;check_caps(&d,8422162432ull);
 d.WddmAperture.bytes=447ull<<20;check_caps(&d,8422162432ull);
 d.Post.Height=0;check_caps(&d,0);
 init(&d,8);CHECK(VramStart(&d)==0);d.VramEnabled=FALSE;check_caps(&d,0);
 init(&d,8);CHECK(VramStart(&d)==0);d.Post.PhysicAddress.QuadPart=1;check_caps(&d,0);
}
/* BD-341: the one top-of-VRAM check (VramFirmwareRangeBelowTop) of gart.c, psp.c, gpumem.c and the start. */
#define TOP_GART (2ull<<20)
#define TOP_PSP (8ull<<20)
#define TOP_POOL (32ull<<20)
/* The check of the three windows before BD-341, with the length that display.c maps. An inherited display must get
   the same answer from the new check. */
static NTSTATUS old_check(const BC250_DEVICE* d,ULONGLONG top)
{
 ULONGLONG fbOffset;
 if(!VramFramebufferOffset(d,&fbOffset)) return STATUS_DEVICE_CONFIGURATION_ERROR;
 return fbOffset+d->FramebufferLength>d->VramLength-top?STATUS_CONFLICTING_ADDRESSES:STATUS_SUCCESS;
}
static void range_cases(void)
{
 static const ULONGLONG tops[]={TOP_GART,TOP_PSP,TOP_POOL};
 BC250_DEVICE d;ULONG gib;unsigned t,e,bar;
 /* Inherited: the framebuffer at the bottom (both views), in the middle, at the edge of each window and in it. */
 for(gib=4;gib<=16;gib+=4) for(t=0;t<3;t++) {
  const ULONGLONG vram=(ULONGLONG)gib<<30,bytes=7680ull*1200;
  const ULONGLONG ends[]={bytes,vram/2,vram-tops[t],vram-tops[t]+1,vram-tops[t]+4096,vram};
  for(e=0;e<sizeof(ends)/sizeof(ends[0]);e++) for(bar=0;bar<2;bar++) {
   NTSTATUS expected=ends[e]<=vram-tops[t]?STATUS_SUCCESS:STATUS_CONFLICTING_ADDRESSES;
   init(&d,gib);CHECK(VramStart(&d)==0);
   d.DisplayStart=BC250_DISPLAY_START_INHERITED;d.FramebufferLength=bytes;
   if(e!=0||bar) d.Post.PhysicAddress.QuadPart=d.VramPhysical.QuadPart+(LONGLONG)(ends[e]-bytes);
   CHECK(VramFirmwareRangeBelowTop(&d,tops[t])==expected);
   CHECK(VramFirmwareRangeBelowTop(&d,tops[t])==old_check(&d,tops[t]));
  }
 }
 /* Inherited, but the framebuffer map failed (display.c: FramebufferLength 0, Post still known). The check before
    BD-341 saw a range of 0 bytes and passed every window. The new check uses the range from Post: a framebuffer that
    ends in a window is a conflict, and one below the windows passes. */
 for(t=0;t<3;t++) for(bar=0;bar<2;bar++) {
  const ULONGLONG bytes=7680ull*1200;
  init(&d,8);CHECK(VramStart(&d)==0);d.DisplayStart=BC250_DISPLAY_START_INHERITED;d.FramebufferLength=0;
  if(bar) d.Post.PhysicAddress.QuadPart=d.VramPhysical.QuadPart;
  CHECK(VramFirmwareRangeBelowTop(&d,tops[t])==STATUS_SUCCESS);
  d.Post.PhysicAddress.QuadPart=d.VramPhysical.QuadPart+(LONGLONG)(d.VramLength-tops[t]+4096-bytes);
  CHECK(old_check(&d,tops[t])==STATUS_SUCCESS);
  CHECK(VramFirmwareRangeBelowTop(&d,tops[t])==STATUS_CONFLICTING_ADDRESSES);
 }
 /* Inherited, the address in no known view: unknown, as before. */
 init(&d,8);missing_bar=1;CHECK(VramStart(&d)==0);d.DisplayStart=BC250_DISPLAY_START_INHERITED;
 d.FramebufferLength=7680ull*1200;
 CHECK(VramFirmwareRangeBelowTop(&d,TOP_POOL)==STATUS_DEVICE_CONFIGURATION_ERROR);
 CHECK(old_check(&d,TOP_POOL)==STATUS_DEVICE_CONFIGURATION_ERROR);
 /* A present display of no size has no range: refused, as the memory layout refuses it. */
 init(&d,8);CHECK(VramStart(&d)==0);d.DisplayStart=BC250_DISPLAY_START_INHERITED;d.Post.Height=0;
 CHECK(VramFirmwareRangeBelowTop(&d,TOP_POOL)==STATUS_DEVICE_CONFIGURATION_ERROR);
 /* No firmware display, on the small carve-out (431 MiB): the cold reserve at offset 0 is the range. FramebufferLength
    is 0 here, so the check before BD-341 passed every one of these. */
 {
  static const struct {ULONGLONG cold;NTSTATUS gart,psp,pool;} cases[]={
   {0,STATUS_SUCCESS,STATUS_SUCCESS,STATUS_SUCCESS},
   {9ull<<20,STATUS_SUCCESS,STATUS_SUCCESS,STATUS_SUCCESS},                       /* the VGA allocation */
   {(431ull-32)<<20,STATUS_SUCCESS,STATUS_SUCCESS,STATUS_SUCCESS},                /* ends at the pool window */
   {((431ull-32)<<20)+4096,STATUS_SUCCESS,STATUS_SUCCESS,STATUS_CONFLICTING_ADDRESSES},
   {(431ull-8)<<20,STATUS_SUCCESS,STATUS_SUCCESS,STATUS_CONFLICTING_ADDRESSES},   /* ends at the PSP window */
   {((431ull-8)<<20)+4096,STATUS_SUCCESS,STATUS_SUCCESS,STATUS_SUCCESS},          /* under 8 MiB left: no reserve */
  };
  unsigned i;
  for(i=0;i<sizeof(cases)/sizeof(cases[0]);i++) for(bar=0;bar<2;bar++) {
   init(&d,8);missing_bar=(int)bar;CHECK(VramStart(&d)==0);
   d.VramLength=431ull<<20;d.DisplayStart=BC250_DISPLAY_START_NO_POST;memset(&d.Post,0,sizeof(d.Post));
   d.FramebufferLength=0;d.ColdScanoutBytes=(unsigned long)cases[i].cold;
   CHECK(VramFirmwareRangeBelowTop(&d,TOP_GART)==cases[i].gart);
   CHECK(VramFirmwareRangeBelowTop(&d,TOP_PSP)==cases[i].psp);
   CHECK(VramFirmwareRangeBelowTop(&d,TOP_POOL)==cases[i].pool);
  }
  /* A window larger than VRAM is a conflict, not an unsigned wrap. */
  CHECK(VramFirmwareRangeBelowTop(&d,d.VramLength+1)==STATUS_CONFLICTING_ADDRESSES);
 }
}
/* The 2 MiB layout (LargePagesSmallCarveout), on the numbers of the unit that needs it: a 512 MiB carve-out at
   0x450000000 ("vram: 0x450000000 + 0x20000000" in its tester.38 log). That log gives the plain layout: "vidmm: VRAM
   segment 1 at physical 0x453050000 + 0x1A240000" and "vidmm: table segment 3 at physical 0x46D290000 + 0xD70000". The
   0x3050000 bytes below the segment are the display reservation. The split of them here is the one that gives that
   number: a firmware framebuffer of 2560x1440 at offset 0 (0xE10000), the handoff surface of connector_buffer.c
   (35389440 bytes) and the two pointer surfaces of dcn_cursor.c (256 KiB each). The MC base is not in that log;
   0xF400000000 is a register value shifted by 24, as vram.c makes every MC base. */
#define UNIT_BASE 0x450000000ull
#define UNIT_BYTES 0x20000000ull
#define UNIT_RESERVED_END (0xE10000ull+35389440ull+2ull*262144ull)
#define HUGE_MASK (BC250_VIDMM_HUGE_PAGE_BYTES-1)
#define GIANT_MASK (BC250_VIDMM_GIANT_PAGE_BYTES-1)
static void unit(BC250_DEVICE* d){
 memset(d,0,sizeof(*d));d->WddmAperture.bytes=384ull<<20;d->Mmio=d;d->VramEnabled=TRUE;d->VramWriteEnabled=TRUE;
 d->VramPhysical.QuadPart=(LONGLONG)UNIT_BASE;d->VramLength=UNIT_BYTES;d->VramMcBase=0xF400000000ull;
 d->Post.Pitch=2560*4;d->Post.Height=1440;d->Post.PhysicAddress=d->VramPhysical;
 d->DisplayReservationEnd=UNIT_RESERVED_END;log_reset();
}
/* The request as WddmStart leaves it for LargePages 0, 1 and 2, then the choice of the start. */
static void settle(BC250_DEVICE* d,ULONG largePages,ULONG smallCarveout){
 d->LargePageLayout=(UCHAR)((largePages==1||largePages==2)?BC250_LARGE_PAGE_LAYOUT_GIANT:BC250_LARGE_PAGE_LAYOUT_NONE);
 d->LargePages=(largePages==1);log_reset();
 WddmSettleLargePageLayout(d,smallCarveout);
}
static void small_carveout_cases(void)
{
 BC250_DEVICE d;ULONGLONG off,len,table,tablelen;struct bc250_pte_context pte;
 const ULONGLONG limit=UNIT_BYTES-BC250_VRAM_TOP_RESERVED;
 CHECK(UNIT_RESERVED_END==0x3050000ull);
 /* Positive control: the plain layout of these numbers is the one the unit logged. */
 unit(&d);CHECK(WddmMemoryLayout(&d,&off,&len,&table,&tablelen));
 CHECK(UNIT_BASE+off==0x453050000ull && len==0x1A240000ull);
 CHECK(UNIT_BASE+table==0x46D290000ull && tablelen==0xD70000ull);
 /* No 1 GiB boundary lies in [0x453050000, 0x46E000000): the layout of LargePages 1 and 2 does not exist here. */
 d.LargePageLayout=BC250_LARGE_PAGE_LAYOUT_GIANT;
 CHECK(!WddmMemoryLayout(&d,&off,&len,&table,&tablelen));CHECK(!off && !len && !table && !tablelen);
 /* The 2 MiB layout: the base moves up to 0x453200000, 0x1B0000 bytes (1.6875 MiB) are not offered, the table
    segment keeps its place and size, and the segment holds 208 whole 2 MiB pages and a tail of 576 KiB. */
 d.LargePageLayout=BC250_LARGE_PAGE_LAYOUT_HUGE;
 CHECK(WddmMemoryLayout(&d,&off,&len,&table,&tablelen));
 CHECK(off==0x3200000ull && UNIT_BASE+off==0x453200000ull && len==0x1A090000ull);
 CHECK(UNIT_BASE+table==0x46D290000ull && tablelen==0xD70000ull && table==off+len);
 CHECK(table+tablelen==limit);
 CHECK(((UNIT_BASE+off)&HUGE_MASK)==0 && ((d.VramMcBase+off)&HUGE_MASK)==0);
 CHECK(off-UNIT_RESERVED_END==0x1B0000ull && 0x1A240000ull-len==0x1B0000ull);
 CHECK(len/BC250_VIDMM_HUGE_PAGE_BYTES==208 && len%BC250_VIDMM_HUGE_PAGE_BYTES==0x90000ull);
 CHECK(len<BC250_VIDMM_GIANT_PAGE_BYTES);    /* no allocation of 1 GiB fits: VidMm has nothing to map at level 2 */
 check_caps(&d,len);                         /* the user-mode driver is told the same segment */
 /* A layout value that is none of the three is no layout. */
 d.LargePageLayout=3;
 CHECK(!WddmMemoryLayout(&d,&off,&len,&table,&tablelen));CHECK(!off && !len && !table && !tablelen);

 /* The choice of the start. LargePages 1 without the new value is every earlier driver: large pages off, the
    plain layout, and the line that says why. */
 unit(&d);settle(&d,1,0);
 CHECK(d.LargePageLayout==BC250_LARGE_PAGE_LAYOUT_NONE && !d.LargePages);
 CHECK(logged("wddm: LargePages off: no VRAM segment above the first 1 GiB physical boundary"));
 CHECK(logged("VRAM layout plain (LargePagesSmallCarveout 0, 2 MiB base not asked for): segment 0x453050000 + 0x1A240000"));
 CHECK(WddmMemoryLayout(&d,&off,&len,&table,&tablelen) && off==0x3050000ull && len==0x1A240000ull);
 pte.vram_base=UNIT_BASE+off;pte.large_pages=pte.huge_pages_only=7;
 CHECK(VidMmLargePageContext(&d,off,&pte)==STATUS_SUCCESS && !pte.large_pages && !pte.huge_pages_only);
 /* LargePages 1 with the new value: the 2 MiB layout, large pages reported, level 1 only. */
 unit(&d);settle(&d,1,1);
 CHECK(d.LargePageLayout==BC250_LARGE_PAGE_LAYOUT_HUGE && d.LargePages);
 CHECK(logged("VRAM layout 2 MiB base (no 1 GiB boundary, LargePagesSmallCarveout 1): segment 0x453200000 + 0x1A090000"));
 CHECK(logged("2 MiB base: 0x1B0000 below it not offered, segment 0x1B0000 smaller, 2 MiB pages at level 1, level 2 refused"));
 CHECK(!logged("LargePages off"));
 CHECK(WddmMemoryLayout(&d,&off,&len,&table,&tablelen) && off==0x3200000ull && len==0x1A090000ull);
 pte.vram_base=UNIT_BASE+off;pte.large_pages=pte.huge_pages_only=0;
 CHECK(VidMmLargePageContext(&d,off,&pte)==STATUS_SUCCESS && pte.large_pages==1 && pte.huge_pages_only==1);
 /* LargePages 2 with the new value: the same layout with 4 KB mappings, the control of the page size. */
 unit(&d);settle(&d,2,1);
 CHECK(d.LargePageLayout==BC250_LARGE_PAGE_LAYOUT_HUGE && !d.LargePages);
 CHECK(logged("segment 0x453200000 + 0x1A090000") && logged("4 KB mappings (LargePages 2)"));
 CHECK(WddmMemoryLayout(&d,&off,&len,&table,&tablelen) && off==0x3200000ull && len==0x1A090000ull);
 pte.vram_base=UNIT_BASE+off;pte.large_pages=pte.huge_pages_only=7;
 CHECK(VidMmLargePageContext(&d,off,&pte)==STATUS_SUCCESS && !pte.large_pages && !pte.huge_pages_only);
 /* LargePages 0 stays the master gate, whatever the new value says. */
 unit(&d);settle(&d,0,1);
 CHECK(d.LargePageLayout==BC250_LARGE_PAGE_LAYOUT_NONE && !d.LargePages);
 CHECK(logged("VRAM layout plain (LargePages 0): segment 0x453050000 + 0x1A240000"));
 CHECK(WddmMemoryLayout(&d,&off,&len,&table,&tablelen) && off==0x3050000ull && len==0x1A240000ull);
 /* Only the value 1 asks for the layout. guard.c gives 0 for every other number; this function does not rely on it. */
 {
  static const ULONG others[]={2,3,4,0x80000001ul,MAXULONG};unsigned i;
  for(i=0;i<sizeof(others)/sizeof(others[0]);i++){
   unit(&d);settle(&d,1,others[i]);
   CHECK(d.LargePageLayout==BC250_LARGE_PAGE_LAYOUT_NONE && !d.LargePages && logged("LargePages off"));
  }
 }
 /* A geometry where the GPU address of the base is not on 2 MiB with the physical one gets no 2 MiB layout: "offset
    aligned" would not be "address aligned" for one of the two addresses. The start falls back, and says so. */
 unit(&d);d.VramMcBase+=0x100000ull;d.LargePageLayout=BC250_LARGE_PAGE_LAYOUT_HUGE;
 CHECK(!WddmMemoryLayout(&d,&off,&len,&table,&tablelen));CHECK(!off && !len && !table && !tablelen);
 settle(&d,1,1);
 CHECK(d.LargePageLayout==BC250_LARGE_PAGE_LAYOUT_NONE && !d.LargePages);
 CHECK(logged("LargePages off") && logged("LargePagesSmallCarveout 1, 2 MiB base does not fit"));
 CHECK(WddmMemoryLayout(&d,&off,&len,&table,&tablelen) && off==0x3050000ull);
 /* The translation context fails closed on every state the choice above cannot produce. */
 unit(&d);
 d.LargePageLayout=BC250_LARGE_PAGE_LAYOUT_HUGE;d.LargePages=TRUE;
 pte.vram_base=0x453050000ull;pte.large_pages=pte.huge_pages_only=7;      /* the base is not a 2 MiB address */
 CHECK(VidMmLargePageContext(&d,0x3050000ull,&pte)==STATUS_INVALID_PARAMETER && !pte.large_pages && !pte.huge_pages_only);
 pte.vram_base=0x453200000ull;d.VramMcBase+=0x100000ull;                 /* the GPU address of the base is not */
 CHECK(VidMmLargePageContext(&d,0x3200000ull,&pte)==STATUS_INVALID_PARAMETER && !pte.large_pages);
 d.VramMcBase-=0x100000ull;
 CHECK(VidMmLargePageContext(&d,0x3200000ull,&pte)==STATUS_SUCCESS && pte.large_pages==1 && pte.huge_pages_only==1);
 d.LargePageLayout=BC250_LARGE_PAGE_LAYOUT_GIANT;                         /* 2 MiB aligned is not 1 GiB aligned */
 CHECK(VidMmLargePageContext(&d,0x3200000ull,&pte)==STATUS_INVALID_PARAMETER && !pte.large_pages && !pte.huge_pages_only);
 d.LargePageLayout=BC250_LARGE_PAGE_LAYOUT_NONE;                          /* large pages without a layout */
 CHECK(VidMmLargePageContext(&d,0x3200000ull,&pte)==STATUS_INVALID_PARAMETER && !pte.large_pages);
 d.LargePageLayout=3;d.LargePages=FALSE;                                  /* an unknown layout */
 CHECK(VidMmLargePageContext(&d,0x3200000ull,&pte)==STATUS_INVALID_PARAMETER && !pte.large_pages);
 /* A base on 1 GiB keeps both levels (the context of the existing layout). */
 d.LargePageLayout=BC250_LARGE_PAGE_LAYOUT_GIANT;d.LargePages=TRUE;pte.vram_base=0x480000000ull;
 CHECK(VidMmLargePageContext(&d,0x30000000ull,&pte)==STATUS_SUCCESS && pte.large_pages==1 && pte.huge_pages_only==0);

 /* The reservation end, every 4 KB over 6 MiB around the unit's value: the base is a 2 MiB address at or above the
    reservation and less than 2 MiB above it, and the tables end at or below the reserved top. */
 {
  ULONGLONG r;unsigned fits=0;
  for(r=UNIT_RESERVED_END-0x200000ull;r<=UNIT_RESERVED_END+0x400000ull;r+=0x1000ull){
   unit(&d);d.DisplayReservationEnd=r;d.LargePageLayout=BC250_LARGE_PAGE_LAYOUT_HUGE;
   if(!WddmMemoryLayout(&d,&off,&len,&table,&tablelen)){CHECK(0);continue;}
   fits++;
   CHECK(((UNIT_BASE+off)&HUGE_MASK)==0 && off>=r && off-r<BC250_VIDMM_HUGE_PAGE_BYTES);
   CHECK(len>0 && (len&65535ull)==0 && table==off+len && table+tablelen<=limit);
  }
  CHECK(fits==0x600000u/0x1000u+1);
 }
 /* A reservation that leaves no room after the rounding is refused, with nothing in the outputs; so is a base whose
    rounding would wrap (a start without a firmware display, so that the framebuffer range does not refuse first). */
 unit(&d);d.DisplayReservationEnd=limit-0x1000ull;d.LargePageLayout=BC250_LARGE_PAGE_LAYOUT_HUGE;
 CHECK(!WddmMemoryLayout(&d,&off,&len,&table,&tablelen));CHECK(!off && !len && !table && !tablelen);
 unit(&d);d.DisplayStart=BC250_DISPLAY_START_NO_POST;memset(&d.Post,0,sizeof(d.Post));
 d.VramPhysical.QuadPart=(LONGLONG)(MAXULONGLONG-0x3100000ull);d.VramMcBase=(ULONGLONG)d.VramPhysical.QuadPart;
 d.LargePageLayout=BC250_LARGE_PAGE_LAYOUT_HUGE;
 CHECK(!WddmMemoryLayout(&d,&off,&len,&table,&tablelen));CHECK(!off && !len && !table && !tablelen);

 /* A carve-out with room above a 1 GiB boundary starts as before, whatever the new value says: the 2 MiB layout is
    never taken in place of a 1 GiB layout that fits (unit A, 8 GiB at 0x270000000). */
 {
  ULONGLONG off0,len0,table0,tablelen0;ULONG small;
  init(&d,8);CHECK(VramStart(&d)==0);d.DisplayReservationEnd=64ull*1024*1024;
  settle(&d,1,0);CHECK(d.LargePageLayout==BC250_LARGE_PAGE_LAYOUT_GIANT && d.LargePages);
  CHECK(WddmMemoryLayout(&d,&off0,&len0,&table0,&tablelen0));
  CHECK((((ULONGLONG)d.VramPhysical.QuadPart+off0)&GIANT_MASK)==0 && logged("VRAM layout 1 GiB base: segment 0x280000000"));
  for(small=0;small<=1;small++){
   ULONG pages;
   for(pages=1;pages<=2;pages++){
    settle(&d,pages,small);
    CHECK(d.LargePageLayout==BC250_LARGE_PAGE_LAYOUT_GIANT && d.LargePages==(pages==1));
    CHECK(WddmMemoryLayout(&d,&off,&len,&table,&tablelen));
    CHECK(off==off0 && len==len0 && table==table0 && tablelen==tablelen0 && !logged("2 MiB base"));
    pte.vram_base=(ULONGLONG)d.VramPhysical.QuadPart+off;
    CHECK(VidMmLargePageContext(&d,off,&pte)==STATUS_SUCCESS && pte.large_pages==(unsigned)(pages==1) && !pte.huge_pages_only);
   }
  }
  /* The 2 MiB layout is refused for a segment of 1 GiB or more, also when a caller asks for it by name: offset 0
     is a 1 GiB offset, and VidMm could then ask for a level 2 page that this layout refuses. */
  d.LargePageLayout=BC250_LARGE_PAGE_LAYOUT_HUGE;
  CHECK(!WddmMemoryLayout(&d,&off,&len,&table,&tablelen));CHECK(!off && !len && !table && !tablelen);
  /* The limit is the segment, not the carve-out: 1056 MiB of VRAM leave 1024 MiB below the reserved top, and after
     the reservation and the tables the segment is below 1 GiB; with 1152 MiB of VRAM it is not. */
  unit(&d);d.VramPhysical.QuadPart=0x440000000ll;d.Post.PhysicAddress=d.VramPhysical;d.VramLength=1056ull<<20;
  d.LargePageLayout=BC250_LARGE_PAGE_LAYOUT_HUGE;
  CHECK(WddmMemoryLayout(&d,&off,&len,&table,&tablelen) && len<BC250_VIDMM_GIANT_PAGE_BYTES && len>(900ull<<20));
  d.VramLength=1152ull<<20;
  CHECK(!WddmMemoryLayout(&d,&off,&len,&table,&tablelen));CHECK(!off && !len && !table && !tablelen);
 }
 /* Wherever the start takes the 2 MiB layout, no 1 GiB page of physical memory lies inside the segment: the first
    1 GiB boundary at or above the base has less than 1 GiB of segment after it. So the refusal of level 2 on that
    layout takes nothing away. Bases are 16 MiB aligned, as the register gives them. */
 {
  static const ULONGLONG bases[]={0x450000000ull,0x440000000ull,0x47F000000ull,0x460000000ull,0x43E000000ull,0x401000000ull};
  static const ULONGLONG sizes[]={256ull<<20,512ull<<20,768ull<<20,1024ull<<20,1040ull<<20,1536ull<<20,2048ull<<20,3072ull<<20};
  unsigned b,s,huge=0,giant=0,none=0;
  for(b=0;b<sizeof(bases)/sizeof(bases[0]);b++) for(s=0;s<sizeof(sizes)/sizeof(sizes[0]);s++){
   ULONGLONG start,boundary;
   unit(&d);d.VramPhysical.QuadPart=(LONGLONG)bases[b];d.Post.PhysicAddress=d.VramPhysical;d.VramLength=sizes[s];
   settle(&d,1,1);
   if(d.LargePageLayout==BC250_LARGE_PAGE_LAYOUT_NONE){none++;CHECK(!d.LargePages);continue;}
   CHECK(d.LargePages);
   if(!WddmMemoryLayout(&d,&off,&len,&table,&tablelen)){CHECK(0);continue;}
   start=bases[b]+off;boundary=(start+GIANT_MASK)&~GIANT_MASK;
   if(d.LargePageLayout==BC250_LARGE_PAGE_LAYOUT_GIANT){giant++;CHECK((start&GIANT_MASK)==0);continue;}
   huge++;
   CHECK(d.LargePageLayout==BC250_LARGE_PAGE_LAYOUT_HUGE && (start&HUGE_MASK)==0);
   CHECK(boundary+BC250_VIDMM_GIANT_PAGE_BYTES>start+len);
   CHECK(len<BC250_VIDMM_GIANT_PAGE_BYTES);      /* and no allocation of 1 GiB: VidMm has no level 2 page to ask for */
  }
  CHECK(huge>=6 && giant>=6 && none==0);
 }
}
int main(void){
 ULONG gib;
 caps_cases();
 range_cases();
 small_carveout_cases();
 for(gib=8;gib<=16;gib+=4){
  BC250_DEVICE d;BC250_GART g;ULONGLONG off,len,table,tablelen;int mismatch;
  init(&d,gib);CHECK(VramStart(&d)==0);CHECK(d.VramEnabled && d.VramWriteEnabled && d.VramLength==((ULONGLONG)gib<<30));
  CHECK(os_checks==1);CHECK(WddmMemoryLayout(&d,&off,&len,&table,&tablelen));
  CHECK(off>=((ULONGLONG)d.Post.Pitch*d.Post.Height) && len>0 && table==off+len);
  CHECK(table+tablelen<=d.VramLength-BC250_VRAM_TOP_RESERVED);
  d.DisplayReservationEnd=64ull*1024*1024;
  CHECK(WddmMemoryLayout(&d,&off,&len,&table,&tablelen));
  CHECK(off==64ull*1024*1024 && table==off+len);
  check_caps(&d,len);
  d.DisplayReservationEnd=d.VramLength-BC250_VRAM_TOP_RESERVED;
  CHECK(!WddmMemoryLayout(&d,&off,&len,&table,&tablelen));
  CHECK(!off && !len && !table && !tablelen);
  // Large-page layout: the segment starts on the first 1 GiB physical boundary above the framebuffer and
  // the display reservation, and the offset stays a segment offset (base plus offset is the aligned address).
  d.DisplayReservationEnd=64ull*1024*1024;d.LargePageLayout=TRUE;
  CHECK(WddmMemoryLayout(&d,&off,&len,&table,&tablelen));
  CHECK((((ULONGLONG)d.VramPhysical.QuadPart+off)&(BC250_VIDMM_GIANT_PAGE_BYTES-1))==0);
  CHECK(off>=64ull*1024*1024 && off<64ull*1024*1024+BC250_VIDMM_GIANT_PAGE_BYTES && len>0 && table==off+len);
  CHECK(table+tablelen<=d.VramLength-BC250_VRAM_TOP_RESERVED);
  d.LargePageLayout=FALSE;
  CHECK(WddmMemoryLayout(&d,&off,&len,&table,&tablelen));
  CHECK(off==64ull*1024*1024);
  // A reservation at or past the usable top is refused before the rounding arithmetic, layout on or off; a
  // reservation near the top of the address space must not underflow the overflow guard into a layout.
  {
   const ULONGLONG tops[]={MAXULONGLONG,MAXULONGLONG-BC250_VIDMM_GIANT_PAGE_BYTES,d.VramLength-BC250_VRAM_TOP_RESERVED,d.VramLength};
   unsigned t;
   for(t=0;t<sizeof(tops)/sizeof(tops[0]);t++){
    d.DisplayReservationEnd=tops[t];d.LargePageLayout=TRUE;
    CHECK(!WddmMemoryLayout(&d,&off,&len,&table,&tablelen));CHECK(!off && !len && !table && !tablelen);
    d.LargePageLayout=FALSE;
    CHECK(!WddmMemoryLayout(&d,&off,&len,&table,&tablelen));CHECK(!off && !len && !table && !tablelen);
   }
  }
  d.DisplayReservationEnd=0;
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
  // BD-341: a start without a firmware display. The framebuffer range is at offset 0, in both views and without
  // BAR0. With nothing that the firmware can scan out of it is empty, so the segment starts at the bottom of VRAM, or
  // above the display reservation when there is one. A present display with a zero size stays refused (above).
  {
   ULONGLONG fbOffset=1,fbBytes=1;
   int bar;
   for(bar=0;bar<2;bar++){
    init(&d,gib);missing_bar=bar;CHECK(VramStart(&d)==0 && d.VramEnabled);
    d.DisplayStart=BC250_DISPLAY_START_NO_POST;memset(&d.Post,0,sizeof(d.Post));
    CHECK(VramFramebufferOffset(&d,&fbOffset) && fbOffset==0);
    CHECK(VramFramebufferExtent(&d,&fbOffset,&fbBytes) && fbOffset==0 && fbBytes==0);
    CHECK(WddmMemoryLayout(&d,&off,&len,&table,&tablelen));
    CHECK(off==0 && len>0 && table==off+len && table+tablelen<=d.VramLength-BC250_VRAM_TOP_RESERVED);
    d.DisplayReservationEnd=4096ull*2160*4;
    CHECK(WddmMemoryLayout(&d,&off,&len,&table,&tablelen));
    CHECK(off==((4096ull*2160*4+65535ull)&~65535ull) && len>0 && table==off+len);
    // amdgpu's firmware reservation (gmc_v10_0_get_vbios_fb_size): the segment starts above it, 64 KiB rounded.
    d.DisplayReservationEnd=0;d.ColdScanoutBytes=BC250_DISPLAY_START_VGA_ALLOCATION;
    CHECK(VramFramebufferExtent(&d,&fbOffset,&fbBytes) && fbOffset==0 && fbBytes==BC250_DISPLAY_START_VGA_ALLOCATION);
    CHECK(WddmMemoryLayout(&d,&off,&len,&table,&tablelen) && off==BC250_DISPLAY_START_VGA_ALLOCATION);
    d.ColdScanoutBytes=1080ul*1920ul*4ul+4;
    CHECK(WddmMemoryLayout(&d,&off,&len,&table,&tablelen) && off==((1080ull*1920*4+4+65535ull)&~65535ull));
    // None when less than 8 MiB of VRAM would remain (amdgpu_gmc_get_vbios_allocations).
    {
     ULONGLONG length=d.VramLength;
     d.VramLength=64ull<<20;d.ColdScanoutBytes=(56ul<<20)+1;
     CHECK(VramFramebufferExtent(&d,&fbOffset,&fbBytes) && fbBytes==0);
     d.ColdScanoutBytes=56ul<<20;
     CHECK(VramFramebufferExtent(&d,&fbOffset,&fbBytes) && fbBytes==(56ull<<20));
     d.VramLength=length;d.ColdScanoutBytes=0;
    }
   }
   // The flag clear with the same zero geometry is a present display of no size: refused, as before BD-341.
   init(&d,gib);CHECK(VramStart(&d)==0);memset(&d.Post,0,sizeof(d.Post));
   CHECK(!VramFramebufferExtent(&d,&fbOffset,&fbBytes) && fbBytes==0);
   CHECK(!WddmMemoryLayout(&d,&off,&len,&table,&tablelen));
   // A present display keeps its offset and size.
   init(&d,gib);CHECK(VramStart(&d)==0);
   CHECK(VramFramebufferExtent(&d,&fbOffset,&fbBytes) && fbOffset==0 && fbBytes==7680ull*1200);
  }
 }
 printf("VRAM geometry: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
