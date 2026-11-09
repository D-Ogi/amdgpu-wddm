// Actual Check3 metadata snapshot with object-index removal/reuse at unlock.
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../object_index.h"
#include "../wddm_mpo_storage.h"
typedef void* HANDLE;
typedef unsigned UINT;
typedef int BOOLEAN,KIRQL;
#define TRUE 1
#define FALSE 0
#define BC250_WDDM_MAGIC_ALLOCATION 1
typedef struct {int Unused;} BC250_DEVICE;
typedef struct {
 void* HashNext;unsigned long long Serial;unsigned Magic;BC250_DEVICE* Device;
 int UmdAlloc,ScanoutRequested;unsigned ScanoutWidth,ScanoutHeight,ScanoutPitch,ScanoutFormat;
 unsigned long long UmdBytes;BC250_WDDM_ALLOCATION_PRIVATE Allocation;BC250_WDDM_NV12_PRIVATE Nv12;
} BC250_WDDM_OBJECT;
typedef struct {int Lock,Stopping;BC250_OBJECT_INDEX ObjectIndex;} BC250_WDDM;
static unsigned checks;static int locked;static void (*onUnlock)(void);
#define CHECK(x) do {++checks;if(!(x)){printf("FAIL %u: %s\n",__LINE__,#x);exit(1);}}while(0)
static void KeAcquireSpinLock(int* p,KIRQL* q){(void)p;CHECK(!locked);locked=1;*q=0;}
static void KeReleaseSpinLock(int* p,KIRQL q){(void)p;(void)q;CHECK(locked);locked=0;if(onUnlock)onUnlock();}
#define RtlZeroMemory(p,n) memset(p,0,n)
static size_t RtlCompareMemory(const void* a,const void* b,size_t n){CHECK(locked);return memcmp(a,b,n)?0:n;}
#include "../wddm_mpo_storage.inc"
static BC250_DEVICE device,other;
static BC250_WDDM w;
static BC250_WDDM_OBJECT a,b;
static void* buckets[BC250_OBJECT_INDEX_BUCKETS];
static void Reset(void){
 memset(&w,0,sizeof(w));memset(&a,0,sizeof(a));memset(&b,0,sizeof(b));memset(buckets,0,sizeof(buckets));
 Bc250ObjectIndexInit(&w.ObjectIndex,buckets,(unsigned long)offsetof(BC250_WDDM_OBJECT,HashNext));
 a.Device=b.Device=&device;a.Magic=b.Magic=1;a.Serial=10;b.Serial=11;
 a.Allocation=(BC250_WDDM_ALLOCATION_PRIVATE){BC250_WDDM_ALLOCATION_PRIVATE_MAGIC,1,64,16,256,21,4096};
 b.Nv12.Surface=(BC250_WDDM_ALLOCATION_PRIVATE){BC250_WDDM_ALLOCATION_PRIVATE_MAGIC,2,64,16,256,BC250_NV12_D3DDDI_FORMAT,8192};
 b.Nv12.ChromaPitch=256;b.Nv12.Layout=1;b.Nv12.ChromaOffset=6144;b.Nv12.Alignment=256;b.Allocation=b.Nv12.Surface;
 Bc250ObjectIndexInsert(&w.ObjectIndex,&a);Bc250ObjectIndexInsert(&w.ObjectIndex,&b);onUnlock=NULL;
}
static void Reuse(void){CHECK(!locked);CHECK(Bc250ObjectIndexRemove(&w.ObjectIndex,&b));memset(&b,0,sizeof(b));b.Serial=99;}
int main(void){
 BC250_MPO_STORAGE out[2];HANDLE handles[2];
 Reset();handles[0]=&a;handles[1]=&b;onUnlock=Reuse;
 CHECK(WddmSnapshotMpoStorage(&device,&w,handles,out));
 CHECK(out[0].Serial==10 && out[0].Surface.Width==64);
 CHECK(out[1].Serial==11 && out[1].Nv12.ChromaOffset==6144 && b.Serial==99);
 for(unsigned variant=0;variant<10;variant++){
  Reset();handles[0]=&a;handles[1]=&b;
  switch(variant){
  case 0:handles[1]=(void*)16;break;
  case 1:handles[1]=&a;break;
  case 2:b.Magic=2;break;
  case 3:b.Device=&other;break;
  case 4:b.Serial=0;break;
  case 5:w.Stopping=1;break;
  case 6:b.Nv12.ChromaOffset=256;break;
  case 7:b.Allocation.Width=32;break;
  case 8:Bc250ObjectIndexRemove(&w.ObjectIndex,&b);break;
  default:handles[0]=NULL;break;
  }
  memset(out,0x5a,sizeof(out));CHECK(!WddmSnapshotMpoStorage(&device,&w,handles,out));
  for(size_t k=0;k<sizeof(out);k++)CHECK(!((unsigned char*)out)[k]);
 }
 Reset();a.UmdAlloc=1;a.ScanoutRequested=1;a.ScanoutWidth=854;a.ScanoutHeight=480;a.ScanoutPitch=3584;a.ScanoutFormat=32;a.UmdBytes=1720320;
 handles[0]=&a;handles[1]=&b;CHECK(WddmSnapshotMpoStorage(&device,&w,handles,out));
 CHECK(out[0].UmdAlloc && out[0].ScanoutRequested && out[0].Surface.Width==854 && out[0].Surface.Size==1720320);
 printf("mpo_storage_test: %u checks passed\n",checks);return 0;
}
