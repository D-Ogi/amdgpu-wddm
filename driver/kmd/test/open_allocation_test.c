// Host test of DxgkDdiOpenAllocation (F-q44-mem-3): the actual Bc250WddmOpenAllocation of wddm.c, with the actual
// object list code it calls (WddmNewObjectPrepared, WddmNewObject, WddmObject, WddmFreeObject) and the actual
// handle-identity bind, extracted by generate_open_allocation_test.py and built by run_open_allocation.ps1 with no WDK
// header. The real umd_blob.c, gdi_admission.h, object_index.h and dcn_translate.c are compiled beside it.
//
// The contract (d3dkmddi.md, DxgkDdiOpenAllocation): STATUS_SUCCESS promises a non-NULL hDeviceSpecificAllocation for
// every allocation, and STATUS_NO_MEMORY is a listed return. The pool allocation of the object is failed at the first,
// a middle and the last allocation of a five-allocation call. Each case must answer STATUS_NO_MEMORY and leave nothing
// of the call behind: no pool block, no object on the adapter's list or in its index, no ObjectCount, no handle data
// held, no LB7A open count. A blob the driver does not admit keeps the BD-060 NULL handle and the success.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "../umd_blob.h"
#include "../gdi_admission.h"
#include "../object_index.h"

static int g_failures, g_checks;

#define CHECK(cond) \
    do { g_checks++; if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)

// ---- what the extracted code needs of the kernel and of dxgkrnl ----------------------------------------------------
typedef unsigned char BOOLEAN;
typedef unsigned char KIRQL;
typedef unsigned long ULONG;
typedef long LONG;
typedef unsigned int UINT;
typedef unsigned long long ULONGLONG;
typedef long NTSTATUS;
typedef void* HANDLE;
typedef void* PVOID;
typedef unsigned int D3DKMT_HANDLE;
typedef size_t SIZE_T;
typedef int KSPIN_LOCK;
#define TRUE 1
#define FALSE 0
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000DL)
#define STATUS_NO_MEMORY ((NTSTATUS)0xC0000017L)
#define PASSIVE_LEVEL 0
#define APC_LEVEL 1
#define POOL_FLAG_NON_PAGED 0x40ull
#define BC250_WDDM_TAG 0x4d374342ul
#define BC250_WDDM_LOG_CALLS 8
#define FIELD_OFFSET(t, f) ((LONG)offsetof(t, f))
#define RtlZeroMemory(p, n) memset((p), 0, (n))

typedef struct _LIST_ENTRY { struct _LIST_ENTRY *Flink, *Blink; } LIST_ENTRY;
static void InitializeListHead(LIST_ENTRY* Head) { Head->Flink = Head->Blink = Head; }
static void InsertTailList(LIST_ENTRY* Head, LIST_ENTRY* Entry)
{
    Entry->Flink = Head; Entry->Blink = Head->Blink; Head->Blink->Flink = Entry; Head->Blink = Entry;
}
static BOOLEAN RemoveEntryList(LIST_ENTRY* Entry)
{
    LIST_ENTRY* next = Entry->Flink;
    LIST_ENTRY* previous = Entry->Blink;
    previous->Flink = next; next->Blink = previous;
    return next == previous;
}

// The pool: every block is counted, and the Nth allocation from now can be made to fail (0 = none).
static unsigned g_poolCalls, g_poolFailAt, g_poolLive;
static void* ExAllocatePool2(ULONGLONG Flags, SIZE_T Bytes, ULONG Tag)
{
    (void)Flags; (void)Tag;
    if (++g_poolCalls == g_poolFailAt) return NULL;
    g_poolLive++;
    return calloc(1, Bytes);        // ExAllocatePool2 zeroes what it returns
}
static void ExFreePoolWithTag(void* Block, ULONG Tag)
{
    (void)Tag;
    CHECK(Block != NULL && g_poolLive > 0);
    g_poolLive--;
    free(Block);
}

static int g_lockDepth;
static void KeAcquireSpinLock(KSPIN_LOCK* Lock, KIRQL* Irql) { (void)Lock; CHECK(g_lockDepth == 0); g_lockDepth++; *Irql = 0; }
static void KeReleaseSpinLock(KSPIN_LOCK* Lock, KIRQL Irql) { (void)Lock; (void)Irql; CHECK(g_lockDepth == 1); g_lockDepth--; }
static KIRQL KeGetCurrentIrql(void) { return PASSIVE_LEVEL; }
static LONG InterlockedIncrement(volatile LONG* Value) { return ++*Value; }
static LONG InterlockedAdd(volatile LONG* Value, LONG Add) { return *Value += Add; }
static const char* g_lastLog;
static void GuardLog(const char* Format, ...) { g_lastLog = Format; }

typedef struct { D3DKMT_HANDLE hAllocation; PVOID pPrivateDriverData; UINT PrivateDriverDataSize; HANDLE hDeviceSpecificAllocation; }
    DXGK_OPENALLOCATIONINFO;
typedef struct { UINT NumAllocations; DXGK_OPENALLOCATIONINFO* pOpenAllocation; struct { UINT Value; } Flags; } DXGKARG_OPENALLOCATION;
#define DXGK_HANDLE_ALLOCATION 1
typedef void* DXGKARGCB_RELEASEHANDLE;
typedef struct { D3DKMT_HANDLE hObject; int Type; } DXGKARGCB_GETHANDLEDATA;
typedef struct { DXGKARGCB_RELEASEHANDLE ReleaseHandle; int Type; } DXGKARGCB_RELEASEHANDLEDATA;
typedef struct {
    ULONG Size;
    PVOID (*DxgkCbGetHandleData)(const DXGKARGCB_GETHANDLEDATA*);
    PVOID (*DxgkCbAcquireHandleData)(const DXGKARGCB_GETHANDLEDATA*, DXGKARGCB_RELEASEHANDLE*);
    void (*DxgkCbReleaseHandleData)(DXGKARGCB_RELEASEHANDLEDATA);
} DXGKRNL_INTERFACE;

// The fields of the driver's objects that the extracted code reads and writes, under their wddm.c names.
typedef struct { int Reserved; } PAGING_CAPTURE_OWNER;
struct _BC250_DEVICE;
typedef struct _BC250_WDDM_OBJECT {
    LIST_ENTRY Link;
    ULONG Magic;
    void* HashNext;
    struct _BC250_DEVICE* Device;
    ULONGLONG Serial;
    PAGING_CAPTURE_OWNER Captures;
    BOOLEAN UmdAlloc;
    ULONGLONG UmdBytes;
    ULONG UmdHeap;
    ULONGLONG UmdRequestedVa;
    BC250_WDDM_ALLOCATION_PRIVATE Allocation;
    ULONG GdiType;
    HANDLE OwnerDevice;
    void* BackingAllocation;
    ULONGLONG BackingSerial;
} BC250_WDDM_OBJECT;
typedef struct _BC250_DEVICE { void* Wddm; DXGKRNL_INTERFACE Dxgk; } BC250_DEVICE;
#define WddmDdiOpenAllocation 0
typedef struct {
    KSPIN_LOCK Lock;
    int Stopping;
    LIST_ENTRY Objects;
    BC250_OBJECT_INDEX ObjectIndex;
    LONG ObjectCount;
    ULONGLONG ObjectSerial;
    BC250_STDALLOC_COUNTERS StdAlloc;
    LONG Calls[1];
    BOOLEAN GpuPresentGate, HandleIdentityProbe;
    LONG HandleIdentityProbeCalls[2];
} BC250_WDDM;
static unsigned g_captureReleases;
static void WddmReleaseCaptures(BC250_WDDM_OBJECT* Object) { (void)Object; g_captureReleases++; }
static BOOLEAN WddmFirstCalls(BC250_WDDM* Wddm, int Ddi) { if (Wddm != NULL) Wddm->Calls[Ddi]++; return FALSE; }

// dxgkrnl's handle data: every acquire must be released, outside the lock.
static BC250_WDDM_OBJECT* g_backing;
static unsigned g_acquires, g_releases;
static int g_token;
static PVOID AcquireHandleData(const DXGKARGCB_GETHANDLEDATA* Query, DXGKARGCB_RELEASEHANDLE* Release)
{
    CHECK(g_lockDepth == 0 && Query->Type == DXGK_HANDLE_ALLOCATION);
    g_acquires++;
    *Release = &g_token;
    return g_backing;
}
static void ReleaseHandleData(DXGKARGCB_RELEASEHANDLEDATA Release)
{
    CHECK(g_lockDepth == 0 && Release.ReleaseHandle == &g_token);
    g_releases++;
}

#include "open_allocation_actual.inc"

// ---- fixture -------------------------------------------------------------------------------------------------------
static void* g_buckets[BC250_OBJECT_INDEX_BUCKETS];
static BC250_WDDM g_wddm;
static BC250_DEVICE g_device;
static BC250_WDDM_OBJECT* g_parent;
static unsigned char g_bc2a[UMD_BLOB_ALLOC_BYTES];
static BC250_GDI_PRIVATE g_lb7a, g_refused;

enum { K_LB7A, K_BC2A, K_REFUSED };
#define MAX_ALLOCS 8
static DXGK_OPENALLOCATIONINFO g_info[MAX_ALLOCS];

static void Put32(unsigned char* p, unsigned long v) { memcpy(p, &v, 4); }
static void Put64(unsigned char* p, unsigned long long v) { memcpy(p, &v, 8); }

static void Blobs(void)
{
    struct umd_alloc_view view;
    BC250_GDI_ALLOCATION_POLICY policy;
    unsigned long type;

    memset(g_bc2a, 0, sizeof(g_bc2a));
    Put32(g_bc2a + 0, UMD_BLOB_ALLOC_MAGIC);
    Put32(g_bc2a + 4, 1);
    Put32(g_bc2a + 8, UMD_BLOB_ALLOC_BYTES);
    Put64(g_bc2a + 16, 65536);
    Put64(g_bc2a + 24, 4096);
    Put32(g_bc2a + 32, UMD_BLOB_HEAP_GTT);
    memset(&g_lb7a, 0, sizeof(g_lb7a));
    g_lb7a.Surface.Magic = BC250_WDDM_ALLOCATION_PRIVATE_MAGIC; g_lb7a.Surface.Version = 1;
    g_lb7a.Surface.Width = 64; g_lb7a.Surface.Height = 4; g_lb7a.Surface.Format = 21;   // D3DDDIFMT_A8R8G8B8
    DcnStagingLayout(64, 4, 4, &g_lb7a.Surface.Pitch, &g_lb7a.Surface.Size);
    g_lb7a.Magic = BC250_GDI_PRIVATE_MAGIC; g_lb7a.Type = 2;
    g_refused = g_lb7a;
    g_refused.Type = 5;                                                                   // a type no policy admits
    // Preconditions: the three blobs are what the cases below call them.
    CHECK(UmdBlobIsAlloc(g_bc2a, sizeof(g_bc2a)) && UmdBlobParseAlloc(g_bc2a, sizeof(g_bc2a), &view) == UMD_BLOB_OK);
    CHECK(Bc250Lb7aAdmit(&g_lb7a, sizeof(g_lb7a), 0, 0, 1, &type, &policy) == BC250_LB7A_ADMITTED && type == 2);
    CHECK(Bc250Lb7aAdmit(&g_refused, sizeof(g_refused), 0, 0, 1, &type, &policy) == BC250_LB7A_REFUSED && type == 5);
}

static void Setup(void)
{
    memset(&g_wddm, 0, sizeof(g_wddm));
    memset(g_buckets, 0, sizeof(g_buckets));
    memset(&g_device, 0, sizeof(g_device));
    InitializeListHead(&g_wddm.Objects);
    Bc250ObjectIndexInit(&g_wddm.ObjectIndex, g_buckets, (unsigned long)offsetof(BC250_WDDM_OBJECT, HashNext));
    g_device.Wddm = &g_wddm;
    g_device.Dxgk.Size = sizeof(g_device.Dxgk);
    g_device.Dxgk.DxgkCbAcquireHandleData = AcquireHandleData;
    g_device.Dxgk.DxgkCbReleaseHandleData = ReleaseHandleData;
    g_wddm.GpuPresentGate = TRUE;       // the identity bind runs: handle data is acquired and released per open
    g_parent = WddmNewObject(&g_device, BC250_WDDM_MAGIC_DEVICE);
    g_backing = WddmNewObject(&g_device, BC250_WDDM_MAGIC_ALLOCATION);
    CHECK(g_parent != NULL && g_backing != NULL && g_wddm.ObjectCount == 2);
}

static void Teardown(void)
{
    while (g_wddm.Objects.Flink != &g_wddm.Objects)
        WddmFreeObject((BC250_WDDM_OBJECT*)g_wddm.Objects.Flink);    // Link is the first field
    CHECK(g_wddm.ObjectCount == 0 && g_wddm.ObjectIndex.Count == 0 && g_wddm.ObjectIndex.Misses == 0);
    CHECK(g_poolLive == 0 && g_lockDepth == 0);
}

static DXGKARG_OPENALLOCATION Call(const int* Kinds, unsigned Count)
{
    DXGKARG_OPENALLOCATION call;
    unsigned i;
    memset(g_info, 0, sizeof(g_info));
    for (i = 0; i < Count; i++)
    {
        g_info[i].hAllocation = 0x40000100u + i;
        if (Kinds[i] == K_BC2A) { g_info[i].pPrivateDriverData = g_bc2a; g_info[i].PrivateDriverDataSize = sizeof(g_bc2a); }
        else { g_info[i].pPrivateDriverData = Kinds[i] == K_LB7A ? &g_lb7a : &g_refused; g_info[i].PrivateDriverDataSize = sizeof(g_lb7a); }
    }
    memset(&call, 0, sizeof(call));
    call.NumAllocations = Count;
    call.pOpenAllocation = g_info;
    return call;
}

// What the adapter holds: list length, index entries, ObjectCount, pool blocks, handle data and the counters.
typedef struct {
    unsigned Listed, Indexed, Pool, Acquires, Releases;
    LONG Count;
    BC250_STDALLOC_COUNTERS Counters;
} SNAPSHOT;
static SNAPSHOT Snap(void)
{
    SNAPSHOT s;
    LIST_ENTRY* e;
    unsigned long b;
    memset(&s, 0, sizeof(s));
    for (e = g_wddm.Objects.Flink; e != &g_wddm.Objects; e = e->Flink) s.Listed++;
    for (b = 0; b < BC250_OBJECT_INDEX_BUCKETS; b++)
    {
        void* o;
        for (o = g_buckets[b]; o != NULL; o = *Bc250ObjectIndexNext(&g_wddm.ObjectIndex, o)) s.Indexed++;
    }
    s.Pool = g_poolLive; s.Acquires = g_acquires; s.Releases = g_releases;
    s.Count = g_wddm.ObjectCount;
    s.Counters = g_wddm.StdAlloc;
    return s;
}

static void CloseAll(unsigned Count)
{
    unsigned i;
    for (i = 0; i < Count; i++) WddmFreeObject(WddmObject(g_info[i].hDeviceSpecificAllocation, BC250_WDDM_MAGIC_OPENED));
}

// Every admitted allocation of a successful call has its own opened object, bound to the allocation dxgkrnl named.
static void CheckOpened(const int* Kinds, unsigned Count)
{
    unsigned i;
    for (i = 0; i < Count; i++)
    {
        BC250_WDDM_OBJECT* o = WddmObject(g_info[i].hDeviceSpecificAllocation, BC250_WDDM_MAGIC_OPENED);
        if (Kinds[i] == K_REFUSED) { CHECK(g_info[i].hDeviceSpecificAllocation == NULL); continue; }   // BD-060
        CHECK(o != NULL);
        if (o == NULL) continue;
        CHECK(Bc250ObjectIndexFind(&g_wddm.ObjectIndex, o) == o && o->OwnerDevice == g_parent);
        CHECK(o->BackingAllocation == g_backing && o->BackingSerial == g_backing->Serial);
        CHECK(o->UmdAlloc == (Kinds[i] == K_BC2A));
    }
}

static void Success(void)
{
    static const int five[] = { K_LB7A, K_BC2A, K_LB7A, K_BC2A, K_LB7A };
    static const int refused[] = { K_LB7A, K_REFUSED, K_BC2A };
    DXGKARG_OPENALLOCATION call;
    SNAPSHOT before, after;

    Setup();
    before = Snap();
    call = Call(five, 5);
    CHECK(Bc250WddmOpenAllocation(g_parent, &call) == STATUS_SUCCESS);
    CheckOpened(five, 5);
    after = Snap();
    CHECK(after.Count == before.Count + 5 && after.Listed == before.Listed + 5 && after.Indexed == before.Indexed + 5);
    CHECK(after.Pool == before.Pool + 5 && after.Acquires == before.Acquires + 5 && after.Releases == after.Acquires);
    CHECK(after.Counters.GdiOpened[2][0] == 3 && after.Counters.GdiOpened[2][1] == 0);
    CloseAll(5);
    after = Snap();
    CHECK(after.Count == before.Count && after.Listed == before.Listed && after.Pool == before.Pool);

    // BD-060 stays: a blob the driver does not admit gets a NULL handle in a call that succeeds, and is counted as
    // refused under its type.
    call = Call(refused, 3);
    CHECK(Bc250WddmOpenAllocation(g_parent, &call) == STATUS_SUCCESS);
    CheckOpened(refused, 3);
    after = Snap();
    CHECK(after.Count == before.Count + 2 && after.Counters.GdiOpened[2][0] == 4 && after.Counters.GdiOpened[5][1] == 1);
    CloseAll(3);
    Teardown();
}

// The pool allocation of entry Fail fails. Kinds have only admitted entries before Fail, so it is the (Fail + 1)th
// allocation of the call.
static void NoMemory(const int* Kinds, unsigned Count, unsigned Fail, unsigned AllocationsBefore)
{
    DXGKARG_OPENALLOCATION call;
    SNAPSHOT before, after;
    unsigned i;

    Setup();
    before = Snap();
    call = Call(Kinds, Count);
    g_poolFailAt = g_poolCalls + AllocationsBefore + 1;
    g_lastLog = NULL;
    CHECK(Bc250WddmOpenAllocation(g_parent, &call) == STATUS_NO_MEMORY);
    g_poolFailAt = 0;
    after = Snap();
    for (i = 0; i <= Fail; i++) CHECK(g_info[i].hDeviceSpecificAllocation == NULL);
    CHECK(after.Count == before.Count && after.Listed == before.Listed && after.Indexed == before.Indexed);
    CHECK(after.Pool == before.Pool && g_wddm.ObjectIndex.Misses == 0);
    CHECK(after.Releases == after.Acquires && after.Acquires == before.Acquires + AllocationsBefore);
    CHECK(!memcmp(&after.Counters, &before.Counters, sizeof(after.Counters)));
    CHECK(g_lastLog != NULL && strstr(g_lastLog, "no memory") != NULL);
    // The same call again with memory: it succeeds and every admitted allocation has a handle.
    call = Call(Kinds, Count);
    CHECK(Bc250WddmOpenAllocation(g_parent, &call) == STATUS_SUCCESS);
    CheckOpened(Kinds, Count);
    CloseAll(Count);
    Teardown();
}

int main(void)
{
    static const int five[] = { K_LB7A, K_BC2A, K_LB7A, K_BC2A, K_LB7A };
    static const int afterRefused[] = { K_LB7A, K_REFUSED, K_BC2A };
    static const int one[] = { K_BC2A };

    Blobs();
    Success();
    NoMemory(five, 5, 0, 0);            // the first allocation of the call
    NoMemory(five, 5, 2, 2);            // a middle one, after an LB7A and a BC2A object
    NoMemory(five, 5, 4, 4);            // the last one, after four objects
    NoMemory(afterRefused, 3, 2, 1);    // behind a BD-060 refusal: the refusal is not counted either
    NoMemory(one, 1, 0, 0);             // a one-allocation call
    if (g_failures) { printf("open_allocation_test: %d of %d checks FAILED\n", g_failures, g_checks); return 1; }
    printf("open_allocation_test: %d checks passed\n", g_checks);
    return 0;
}
