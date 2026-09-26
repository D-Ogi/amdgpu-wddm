#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define DXGKDDI_INTERFACE_VERSION DXGKDDI_INTERFACE_VERSION_WDDM2_0
#include <ntddk.h>
#include <dispmprt.h>
#include "umd_blob.h"
#define BC250_WDDM_LOG_CALLS 8
#define BC250_WDDM_SEGMENT_VRAM 1u
#define BC250_WDDM_SEGMENT_APERTURE 2u
#define BC250_WDDM_SEGMENT_SET(id) (1u<<((id)-1))
#define BC250_WDDM_MAGIC_RESOURCE 1u
#define BC250_WDDM_MAGIC_ALLOCATION 2u
#define BC250_WDDM_ALLOCATION_PRIVATE_MAGIC 0x4137424Cul
#define WddmDdiCreateAllocation 1
static unsigned checks,failures,created;
#define CHECK(x) do{checks++;if(!(x)){failures++;printf("FAIL %u %s\n",(unsigned)__LINE__,#x);}}while(0)
typedef struct {ULONG Magic,Version,Width,Height,Pitch,Format;ULONGLONG Size;} BC250_WDDM_ALLOCATION_PRIVATE;
typedef struct {LONG UmdAllocRefused,UmdAllocs;} BC250_WDDM;
typedef struct {BC250_WDDM Wddm;} BC250_DEVICE;
typedef struct {BC250_WDDM_ALLOCATION_PRIVATE Allocation;BOOLEAN UmdAlloc;ULONGLONG UmdBytes,UmdRequestedVa;ULONG UmdHeap;} BC250_WDDM_OBJECT;
static BOOLEAN g_ApertureOffered=TRUE;
static BC250_WDDM* WddmOf(HANDLE adapter){return &((BC250_DEVICE*)adapter)->Wddm;}
static int WddmFirstCalls(BC250_WDDM* w,int id){(void)w;(void)id;return 0;}
static BC250_WDDM_OBJECT* WddmNewObject(BC250_DEVICE* d,ULONG magic){(void)d;(void)magic;created++;return calloc(1,sizeof(BC250_WDDM_OBJECT));}
static void WddmFreeObject(BC250_WDDM_OBJECT* o){free(o);}
static void GuardLog(const char* f,...){(void)f;}
