// Executes the production identity helpers with controlled callback/index state.
// This models ordering, not the Windows kernel ABI or scheduler.
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../object_index.h"
typedef void* HANDLE;
typedef unsigned int ULONG,UINT;
typedef unsigned long long ULONGLONG;
typedef int BOOLEAN,KIRQL;
#define TRUE 1
#define FALSE 0
#define PASSIVE_LEVEL 0
#define APC_LEVEL 1
#define FIELD_OFFSET(t,f) offsetof(t,f)
#define BC250_WDDM_MAGIC_ALLOCATION 1
#define BC250_WDDM_MAGIC_OPENED 2
#define DXGK_HANDLE_ALLOCATION 1
#define REQUIRE(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static unsigned checks;
typedef struct { HANDLE hObject; ULONG Type; struct { ULONG Value; } Flags; } DXGKARGCB_GETHANDLEDATA;
typedef struct { ULONG Type; HANDLE ReleaseHandle; } DXGKARGCB_RELEASEHANDLEDATA;
typedef struct { HANDLE hAllocation; } DXGK_OPENALLOCATIONINFO;
typedef struct { size_t Size; void* (*DxgkCbGetHandleData)(const DXGKARGCB_GETHANDLEDATA*); void* (*DxgkCbAcquireHandleData)(const DXGKARGCB_GETHANDLEDATA*,HANDLE*); void (*DxgkCbReleaseHandleData)(DXGKARGCB_RELEASEHANDLEDATA); } DXGKRNL_INTERFACE;
typedef struct { void* Wddm; DXGKRNL_INTERFACE Dxgk; } BC250_DEVICE;
typedef struct { ULONG Width,Height,Pitch,Format; uint64_t Size; } BC250_WDDM_ALLOCATION_PRIVATE;
typedef struct { void* HashNext; ULONGLONG Serial; ULONG Magic; BC250_DEVICE* Device; HANDLE OwnerDevice,BackingAllocation; ULONGLONG BackingSerial; BOOLEAN UmdAlloc; BC250_WDDM_ALLOCATION_PRIVATE Allocation; } BC250_WDDM_OBJECT;
typedef struct { int Lock; BOOLEAN Stopping,GpuPresentGate,HandleIdentityProbe; int HandleIdentityProbeCalls[2]; BC250_OBJECT_INDEX ObjectIndex; ULONGLONG ObjectSerial; } BC250_WDDM;
static KIRQL level;
static int locked,acquires,releases,gets,logs;
static void *data,*token;
static void (*onAcquire)(void),(*onRelease)(void),(*onUnlock)(void);
static KIRQL KeGetCurrentIrql(void) { return level; }
static void KeAcquireSpinLock(int* lock,KIRQL* old) { (void)lock;REQUIRE(!locked);locked=1;*old=level;level=2; }
static void KeReleaseSpinLock(int* lock,KIRQL old) { (void)lock;REQUIRE(locked);locked=0;level=old;if(onUnlock)onUnlock(); }
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
static void* buckets[BC250_OBJECT_INDEX_BUCKETS];
// Addresses no test object has: dereferencing either would fault on the host (kernel half, and page 0).
#define UNMAPPED_KERNEL ((HANDLE)(uintptr_t)0xFFFFF78000001230ull)
#define UNMAPPED_LOW ((HANDLE)(uintptr_t)0x10ull)
// WddmNewObjectPrepared's critical section: serial, then the index.
static void insert(BC250_WDDM_OBJECT* obj,ULONG magic) {
 obj->Magic=magic;obj->Device=&dev;obj->OwnerDevice=&dev;
 obj->Serial=++w.ObjectSerial;Bc250ObjectIndexInsert(&w.ObjectIndex,obj);
 obj->Allocation.Width=64;obj->Allocation.Height=32;obj->Allocation.Pitch=256;obj->Allocation.Size=8192;obj->Allocation.Format=21;
}
// WddmFreeObject's critical section and the Magic clear after it; the pool block stays readable on the host.
static void destroy(BC250_WDDM_OBJECT* obj) { REQUIRE(Bc250ObjectIndexRemove(&w.ObjectIndex,obj));obj->Magic=0; }
// A new object at a freed address: ExAllocatePool2 zeroes it, then the same critical section.
static void reuse(BC250_WDDM_OBJECT* obj,ULONG magic) { destroy(obj);memset(obj,0,sizeof(*obj));insert(obj,magic); }
static void unlinkA(void) { destroy(&a); }
static void destroyOnRelease(void) { REQUIRE(o.BackingAllocation==&a && o.BackingSerial==a.Serial);unlinkA(); }
static void back(BC250_WDDM_OBJECT* opened,BC250_WDDM_OBJECT* alloc) { opened->BackingAllocation=alloc;opened->BackingSerial=alloc->Serial; }
static void changeAtUnlock(void) {
 REQUIRE(!locked);
 a.Allocation.Width=o.Allocation.Width=99;
 b.Allocation.Size=p.Allocation.Size=7;
}
static void reset(void) {
 memset(&w,0,sizeof(w));memset(&dev,0,sizeof(dev));memset(&a,0,sizeof(a));memset(&b,0,sizeof(b));memset(&o,0,sizeof(o));memset(&p,0,sizeof(p));
 memset(buckets,0,sizeof(buckets));Bc250ObjectIndexInit(&w.ObjectIndex,buckets,(unsigned long)offsetof(BC250_WDDM_OBJECT,HashNext));
 w.GpuPresentGate=1;w.HandleIdentityProbe=1;
 dev.Wddm=&w;dev.Dxgk.Size=sizeof(dev.Dxgk);dev.Dxgk.DxgkCbAcquireHandleData=acquire;dev.Dxgk.DxgkCbReleaseHandleData=release;dev.Dxgk.DxgkCbGetHandleData=get;
 insert(&a,1);insert(&b,1);insert(&o,2);insert(&p,2);
 info.hAllocation=&a;data=&a;token=&b;onAcquire=onRelease=onUnlock=NULL;level=0;locked=acquires=releases=gets=logs=0;
}
static void bind(void) { WddmBindHandleIdentity(&dev,&info,&o,1);REQUIRE(!locked); }
static BOOLEAN snapshot(BC250_WDDM_ALLOCATION_PRIVATE out[2]) { HANDLE h[2]={&o,&p};return WddmSnapshotPresentAllocations(&w,&dev,h,out); }
static BOOLEAN refused(void) {
 BC250_WDDM_ALLOCATION_PRIVATE out[2],before[2];
 memset(out,0x5a,sizeof(out));memcpy(before,out,sizeof(out));
 if (snapshot(out)) return FALSE;
 REQUIRE(!memcmp(out,before,sizeof(out)) && !locked);
 return TRUE;
}
int main(void) {
 BC250_WDDM_ALLOCATION_PRIVATE out[2],before[2];
 unsigned variant;
 reset();bind();REQUIRE(o.BackingAllocation==&a && o.BackingSerial==a.Serial && acquires==1 && releases==1 && gets==1 && logs==1);
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
 // Acquired values that are no indexed object are compared, never read.
 reset();data=(void*)(uintptr_t)1;bind();REQUIRE(!o.BackingAllocation && releases==1);
 reset();data=UNMAPPED_KERNEL;bind();REQUIRE(!o.BackingAllocation && releases==1);
 reset();data=UNMAPPED_LOW;bind();REQUIRE(!o.BackingAllocation && releases==1);
 reset();data=&o;bind();REQUIRE(!o.BackingAllocation && releases==1);
 reset();a.Magic=2;bind();REQUIRE(!o.BackingAllocation && releases==1);
 reset();a.Device=NULL;bind();REQUIRE(!o.BackingAllocation && releases==1);
 reset();w.Stopping=1;bind();REQUIRE(!o.BackingAllocation && releases==1);
 reset();onAcquire=unlinkA;bind();REQUIRE(!o.BackingAllocation && releases==1);
 // Destroyed after the bind: the stored value stays, the binding is dead.
 reset();onRelease=destroyOnRelease;bind();REQUIRE(o.BackingAllocation==&a && releases==1);
 back(&p,&b);REQUIRE(refused());
 reset();back(&o,&a);back(&p,&b);REQUIRE(snapshot(out));REQUIRE(out[0].Width==64 && out[1].Size==8192);
 // Bound through the real bind, then snapshotted.
 reset();bind();back(&p,&b);REQUIRE(snapshot(out));REQUIRE(out[0].Width==64);
 // Mutate at the unlock boundary, before the helper returns. A descriptor
 // read/copy moved out of the critical section must not pass this witness.
 reset();back(&o,&a);back(&p,&b);onUnlock=changeAtUnlock;
 REQUIRE(snapshot(out));REQUIRE(out[0].Width==64 && out[1].Size==8192);
 REQUIRE(o.Allocation.Width==99 && p.Allocation.Size==7);
 for(variant=0;variant<11;variant++) {
  HANDLE h[2];reset();back(&o,&a);back(&p,&b);h[0]=&o;h[1]=&p;
  switch(variant) {
   case 0:o.BackingAllocation=NULL;break;
   case 1:o.Magic=BC250_WDDM_MAGIC_ALLOCATION;break;
   case 2:destroy(&o);break;
   case 3:h[0]=NULL;break;
   case 4:a.Magic=BC250_WDDM_MAGIC_OPENED;break;
   case 5:h[0]=(HANDLE)(uintptr_t)1;break;
   case 6:h[1]=UNMAPPED_KERNEL;break;
   case 7:o.BackingAllocation=UNMAPPED_KERNEL;break;
   case 8:o.BackingSerial++;break;
   case 9:o.BackingSerial=0;break;
   default:h[1]=&b;break;            // an allocation where an opened object belongs
  }
  memset(out,0x5a,sizeof(out));memcpy(before,out,sizeof(out));
  REQUIRE(!WddmSnapshotPresentAllocations(&w,&dev,h,out));
  REQUIRE(!memcmp(out,before,sizeof(out)) && !locked);
 }
 reset();back(&o,&a);back(&p,&a);REQUIRE(refused());
 reset();back(&o,&a);back(&p,&b);p.OwnerDevice=NULL;REQUIRE(refused());
 reset();back(&o,&a);back(&p,&b);b.Allocation.Width++;REQUIRE(refused());
 reset();back(&o,&a);back(&p,&b);o.UmdAlloc=1;REQUIRE(refused());
 reset();back(&o,&a);back(&p,&b);b.UmdAlloc=1;REQUIRE(refused());
 reset();back(&o,&a);back(&p,&b);unlinkA();REQUIRE(refused());
 reset();back(&o,&a);back(&p,&b);w.Stopping=1;REQUIRE(refused());
 reset();back(&o,&a);back(&p,&b);b.Allocation.Format=p.Allocation.Format=22;REQUIRE(refused());
 // Free and reuse: the pool hands the destroyed allocation's address to a new
 // allocation with identical geometry. Only the serial tells them apart; the
 // free did not touch the opened object (no scan), and the binding is refused.
 reset();bind();back(&p,&b);REQUIRE(snapshot(out));
 {
  ULONGLONG old=a.Serial;
  destroy(&a);REQUIRE(o.BackingAllocation==&a && o.BackingSerial==old);REQUIRE(refused());
  memset(&a,0,sizeof(a));insert(&a,BC250_WDDM_MAGIC_ALLOCATION);
  REQUIRE(a.Serial!=old && Bc250ObjectIndexFind(&w.ObjectIndex,&a)==&a);
  REQUIRE(!memcmp(&a.Allocation,&o.Allocation,sizeof(a.Allocation)));
  REQUIRE(refused());
  // Reused as another kind of object: refused as well.
  reuse(&a,BC250_WDDM_MAGIC_OPENED);REQUIRE(refused());
  // A fresh bind to the successor is a new binding and passes.
  reuse(&a,BC250_WDDM_MAGIC_ALLOCATION);bind();REQUIRE(o.BackingSerial==a.Serial);REQUIRE(snapshot(out));
 }
 // The opened object itself reused at the same address: its new incarnation has no binding.
 reset();back(&o,&a);back(&p,&b);reuse(&p,BC250_WDDM_MAGIC_OPENED);REQUIRE(refused());
 REQUIRE(w.ObjectIndex.Misses==0);
 REQUIRE(!locked);printf("allocation_identity PASS checks=%u\n",checks);return 0;
}
