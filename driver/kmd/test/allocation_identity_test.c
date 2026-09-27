// Executes the production identity helpers with controlled callback/list state.
// This models ordering, not the Windows kernel ABI or scheduler.
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef void* HANDLE;
typedef unsigned int ULONG,UINT;
typedef int BOOLEAN,KIRQL;
#define TRUE 1
#define FALSE 0
#define PASSIVE_LEVEL 0
#define APC_LEVEL 1
#define FIELD_OFFSET(t,f) offsetof(t,f)
#define CONTAINING_RECORD(p,t,f) ((t*)((char*)(p)-offsetof(t,f)))
#define BC250_WDDM_MAGIC_ALLOCATION 1
#define BC250_WDDM_MAGIC_OPENED 2
#define DXGK_HANDLE_ALLOCATION 1
#define REQUIRE(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static unsigned checks;
typedef struct LIST_ENTRY { struct LIST_ENTRY *Flink,*Blink; } LIST_ENTRY;
typedef struct { HANDLE hObject; ULONG Type; struct { ULONG Value; } Flags; } DXGKARGCB_GETHANDLEDATA;
typedef struct { ULONG Type; HANDLE ReleaseHandle; } DXGKARGCB_RELEASEHANDLEDATA;
typedef struct { HANDLE hAllocation; } DXGK_OPENALLOCATIONINFO;
typedef struct { size_t Size; void* (*DxgkCbGetHandleData)(const DXGKARGCB_GETHANDLEDATA*); void* (*DxgkCbAcquireHandleData)(const DXGKARGCB_GETHANDLEDATA*,HANDLE*); void (*DxgkCbReleaseHandleData)(DXGKARGCB_RELEASEHANDLEDATA); } DXGKRNL_INTERFACE;
typedef struct { void* Wddm; DXGKRNL_INTERFACE Dxgk; } BC250_DEVICE;
typedef struct { ULONG Width,Height,Pitch,Format; uint64_t Size; } BC250_WDDM_ALLOCATION_PRIVATE;
typedef struct { LIST_ENTRY Link; ULONG Magic; BC250_DEVICE* Device; HANDLE OwnerDevice,BackingAllocation; BOOLEAN UmdAlloc; BC250_WDDM_ALLOCATION_PRIVATE Allocation; } BC250_WDDM_OBJECT;
typedef struct { int Lock; BOOLEAN Stopping,GpuPresentGate,HandleIdentityProbe; int HandleIdentityProbeCalls[2]; LIST_ENTRY Objects; } BC250_WDDM;
static KIRQL level;
static int locked,acquires,releases,gets,logs;
static void *data,*token;
static void (*onAcquire)(void),(*onRelease)(void);
static KIRQL KeGetCurrentIrql(void) { return level; }
static void KeAcquireSpinLock(int* lock,KIRQL* old) { (void)lock;REQUIRE(!locked);locked=1;*old=level;level=2; }
static void KeReleaseSpinLock(int* lock,KIRQL old) { (void)lock;REQUIRE(locked);locked=0;level=old; }
static int InterlockedIncrement(int* p) { return ++*p; }
static size_t RtlCompareMemory(const void* a,const void* b,size_t n) { REQUIRE(locked);return memcmp(a,b,n)?0:n; }
static void GuardLog(const char* format,...) { (void)format;++logs; }
static void* get(const DXGKARGCB_GETHANDLEDATA* q) { REQUIRE(!locked && level==0);REQUIRE(q->Type==1);++gets;return NULL; }
static void* acquire(const DXGKARGCB_GETHANDLEDATA* q,HANDLE* release) { REQUIRE(!locked && level<=1);REQUIRE(q->Type==1 && q->Flags.Value==0);++acquires;*release=token;if(onAcquire)onAcquire();return data; }
static void release(DXGKARGCB_RELEASEHANDLEDATA r) { REQUIRE(!locked && level<=1);REQUIRE(r.Type==1 && r.ReleaseHandle==token);++releases;if(onRelease)onRelease(); }
#include "../wddm_allocation_identity.inc"
static BC250_WDDM w;
static BC250_DEVICE dev;
static BC250_WDDM_OBJECT a,b,o,p;
static DXGK_OPENALLOCATIONINFO info;
static void insert(BC250_WDDM_OBJECT* obj,ULONG magic) {
 obj->Magic=magic;obj->Device=&dev;obj->OwnerDevice=&dev;
 obj->Link.Flink=w.Objects.Flink;obj->Link.Blink=&w.Objects;w.Objects.Flink->Blink=&obj->Link;w.Objects.Flink=&obj->Link;
 obj->Allocation.Width=64;obj->Allocation.Height=32;obj->Allocation.Pitch=256;obj->Allocation.Size=8192;obj->Allocation.Format=21;
}
static void unlinkA(void) { a.Link.Blink->Flink=a.Link.Flink;a.Link.Flink->Blink=a.Link.Blink; }
static void destroyOnRelease(void) { REQUIRE(o.BackingAllocation==&a);o.BackingAllocation=NULL;unlinkA(); }
static void reset(void) {
 memset(&w,0,sizeof(w));memset(&dev,0,sizeof(dev));memset(&a,0,sizeof(a));memset(&b,0,sizeof(b));memset(&o,0,sizeof(o));memset(&p,0,sizeof(p));
 w.Objects.Flink=w.Objects.Blink=&w.Objects;w.GpuPresentGate=1;w.HandleIdentityProbe=1;
 dev.Wddm=&w;dev.Dxgk.Size=sizeof(dev.Dxgk);dev.Dxgk.DxgkCbAcquireHandleData=acquire;dev.Dxgk.DxgkCbReleaseHandleData=release;dev.Dxgk.DxgkCbGetHandleData=get;
 insert(&a,1);insert(&b,1);insert(&o,2);insert(&p,2);
 info.hAllocation=&a;data=&a;token=&b;onAcquire=onRelease=NULL;level=0;locked=acquires=releases=gets=logs=0;
}
static void bind(void) { WddmBindHandleIdentity(&dev,&info,&o,1);REQUIRE(!locked); }
static BOOLEAN snapshot(BC250_WDDM_ALLOCATION_PRIVATE out[2]) { HANDLE h[2]={&o,&p};return WddmSnapshotPresentAllocations(&w,&dev,h,out); }
int main(void) {
 BC250_WDDM_ALLOCATION_PRIVATE out[2],before[2];
 reset();bind();REQUIRE(o.BackingAllocation==&a && acquires==1 && releases==1 && gets==1 && logs==1);
 reset();w.HandleIdentityProbe=0;bind();REQUIRE(o.BackingAllocation==&a && acquires==1 && releases==1 && !gets && !logs);
 reset();w.GpuPresentGate=0;bind();REQUIRE(o.BackingAllocation==&a && acquires==1 && releases==1);
 reset();w.GpuPresentGate=w.HandleIdentityProbe=0;bind();REQUIRE(!o.BackingAllocation && !acquires && !releases);
 reset();w.HandleIdentityProbeCalls[0]=16;bind();REQUIRE(o.BackingAllocation==&a && acquires==1 && releases==1 && !logs && !gets);
 reset();level=1;bind();REQUIRE(o.BackingAllocation==&a && acquires==1 && releases==1 && !gets);
 reset();level=2;bind();REQUIRE(!o.BackingAllocation && !acquires && !releases && !gets);
 reset();dev.Dxgk.Size=FIELD_OFFSET(DXGKRNL_INTERFACE,DxgkCbReleaseHandleData);bind();REQUIRE(!o.BackingAllocation && !acquires);
 reset();dev.Dxgk.DxgkCbAcquireHandleData=NULL;bind();REQUIRE(!o.BackingAllocation && !acquires);
 reset();dev.Dxgk.DxgkCbReleaseHandleData=NULL;bind();REQUIRE(!o.BackingAllocation && !acquires);
 reset();data=NULL;bind();REQUIRE(!o.BackingAllocation && acquires==1 && releases==1);
 reset();data=token=NULL;bind();REQUIRE(!o.BackingAllocation && acquires==1 && !releases);
 reset();token=NULL;bind();REQUIRE(o.BackingAllocation==&a && releases==1);
 reset();data=(void*)(uintptr_t)1;bind();REQUIRE(!o.BackingAllocation && releases==1);
 reset();a.Magic=2;bind();REQUIRE(!o.BackingAllocation && releases==1);
 reset();a.Device=NULL;bind();REQUIRE(!o.BackingAllocation && releases==1);
 reset();w.Stopping=1;bind();REQUIRE(!o.BackingAllocation && releases==1);
 reset();onAcquire=unlinkA;bind();REQUIRE(!o.BackingAllocation && releases==1);
 reset();onRelease=destroyOnRelease;bind();REQUIRE(!o.BackingAllocation && releases==1);
 reset();o.BackingAllocation=&a;p.BackingAllocation=&b;REQUIRE(snapshot(out));REQUIRE(out[0].Width==64 && out[1].Size==8192);
 // Snapshots remain values when the formerly matched objects change.
 a.Allocation.Width=o.Allocation.Width=99;REQUIRE(out[0].Width==64);
 reset();o.BackingAllocation=&a;p.BackingAllocation=&a;memset(out,0x5a,sizeof(out));memcpy(before,out,sizeof(out));REQUIRE(!snapshot(out));REQUIRE(!memcmp(out,before,sizeof(out)));
 reset();o.BackingAllocation=&a;p.BackingAllocation=&b;p.OwnerDevice=NULL;REQUIRE(!snapshot(out));
 reset();o.BackingAllocation=&a;p.BackingAllocation=&b;b.Allocation.Width++;REQUIRE(!snapshot(out));
 reset();o.BackingAllocation=&a;p.BackingAllocation=&b;o.UmdAlloc=1;REQUIRE(!snapshot(out));
 reset();o.BackingAllocation=&a;p.BackingAllocation=&b;b.UmdAlloc=1;REQUIRE(!snapshot(out));
 reset();o.BackingAllocation=&a;p.BackingAllocation=&b;unlinkA();REQUIRE(!snapshot(out));
 reset();o.BackingAllocation=&a;p.BackingAllocation=&b;w.Stopping=1;REQUIRE(!snapshot(out));
 reset();o.BackingAllocation=&a;p.BackingAllocation=&b;b.Allocation.Format=p.Allocation.Format=22;REQUIRE(!snapshot(out));
 REQUIRE(!locked);printf("allocation_identity PASS checks=%u\n",checks);return 0;
}
