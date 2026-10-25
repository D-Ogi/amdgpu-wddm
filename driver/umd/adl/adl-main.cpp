// The exported functions of the open ADL subset (atiadlxx.dll on x64, atiadlxy.dll on x86).
//
// The library answers from one snapshot per context (adl-model.h), taken at Create and at Refresh. It reports
// only adapters that run our kernel-mode driver, with the values the operating system gives for them. It does not
// report a Radeon Software or Catalyst release, an Eyefinity (SLS) group, overdrive or any other feature; such a
// call returns ADL_ERR_NOT_SUPPORTED and clears its outputs. Functions that AMD's library has and this one does
// not are not exported at all, so that a caller sees them as absent (docs/design/adl-subset.md).
//
// Memory: arrays returned to the caller come from the caller's allocation callback, and the caller frees them, as
// the ADL contract says. Context memory is ours: Destroy frees it. One lock serializes every call, so the
// threading model of ADL2_Main_ControlX3_Create needs no other handling. Caller allocation callbacks run outside
// that lock, against a private snapshot, so they can query, refresh or destroy their context.
#include <windows.h>
#include <string.h>

#include "adl-abi.h"
#include "adl-model.h"

namespace {

const uint32_t kContextMagic = 0x41444c32;  // 'ADL2'

struct AdlContext {
    uint32_t magic;
    AdlContext* next;
    ADL_MAIN_MALLOC_CALLBACK alloc;
    int enum_connected;
    int threading;
    AdlSnapshot snap;
};

SRWLOCK g_lock = SRWLOCK_INIT;
AdlContext* g_contexts;     // ADL2 contexts, by handle
AdlContext* g_legacy;       // the context of the ADL_* functions without a handle
int g_legacy_refs;
LONG g_live_blocks;         // context blocks not yet freed (read by the test build)

#if defined(BC250_ADL_TEST_HOOKS)
AdlSnapshot* g_injected;    // test build only: replaces the discovery
#endif

struct Lock {
    Lock() { AcquireSRWLockExclusive(&g_lock); }
    ~Lock() { ReleaseSRWLockExclusive(&g_lock); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
};

void* HeapBlock(size_t bytes)
{
    void* p = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes);
    if (p) {
        InterlockedIncrement(&g_live_blocks);
    }
    return p;
}

void FreeBlock(void* p)
{
    if (p) {
        HeapFree(GetProcessHeap(), 0, p);
        InterlockedDecrement(&g_live_blocks);
    }
}

// Takes the facts outside the lock: discovery calls the operating system and may take a while.
void TakeSnapshot(AdlSnapshot* out)
{
#if defined(BC250_ADL_TEST_HOOKS)
    {
        Lock lock;
        if (g_injected) {
            *out = *g_injected;
            return;
        }
    }
#endif
    AdlDiscover(out);
}

void CopyText(char* dst, size_t cap, const char* src)
{
    size_t n = strnlen(src, cap - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

// The caller's memory, zeroed. Null when the callback fails or the size does not fit its int argument.
void* CallerBlock(ADL_MAIN_MALLOC_CALLBACK alloc, size_t count, size_t size)
{
    if (count == 0 || size == 0 || count > (size_t)0x7fffffff / size) {
        return nullptr;
    }
    void* p = alloc((int)(count * size));
    if (p) {
        memset(p, 0, count * size);
    }
    return p;
}

AdlContext* Resolve(ADL_CONTEXT_HANDLE handle)
{
    if (!handle) {
        return g_legacy;
    }
    for (AdlContext* c = g_contexts; c; c = c->next) {
        if (c == handle && c->magic == kContextMagic) {
            return c;
        }
    }
    return nullptr;
}

// Do not retain a context pointer across an application callback. The callback may re-enter ADL, including
// Destroy or Refresh. Copy the snapshot under the context lock, then release the lock before any callback.
// The snapshot is on our heap, not the caller's stack or allocation callback.
struct CallSnapshot {
    AdlSnapshot* snap = nullptr;
    ADL_MAIN_MALLOC_CALLBACK alloc = nullptr;
    int status = ADL_ERR;

    explicit CallSnapshot(ADL_CONTEXT_HANDLE handle)
    {
        snap = (AdlSnapshot*)HeapBlock(sizeof(*snap));
        if (!snap) {
            return;
        }
        Lock lock;
        AdlContext* context = Resolve(handle);
        if (!context) {
            status = ADL_ERR_NOT_INIT;
            return;
        }
        *snap = context->snap;
        alloc = context->alloc;
        status = ADL_OK;
    }
    ~CallSnapshot() { FreeBlock(snap); }
    CallSnapshot(const CallSnapshot&) = delete;
    CallSnapshot& operator=(const CallSnapshot&) = delete;
};

AdlContext* NewContext(ADL_MAIN_MALLOC_CALLBACK alloc, int enum_connected, int threading)
{
    AdlContext* c = (AdlContext*)HeapBlock(sizeof(AdlContext));
    if (!c) {
        return nullptr;
    }
    c->magic = kContextMagic;
    c->alloc = alloc;
    c->enum_connected = enum_connected;
    c->threading = threading;
    TakeSnapshot(&c->snap);
    return c;
}

void DeleteContext(AdlContext* c)
{
    c->magic = 0;
    FreeBlock(c);
}

int Create(ADL_MAIN_MALLOC_CALLBACK alloc, int enum_connected, ADL_CONTEXT_HANDLE* out, int threading)
{
    if (!out) {
        return ADL_ERR_NULL_POINTER;
    }
    *out = nullptr;
    if (!alloc) {
        return ADL_ERR_INVALID_CALLBACK;
    }
    AdlContext* c = NewContext(alloc, enum_connected, threading);
    if (!c) {
        return ADL_ERR;
    }
    Lock lock;
    c->next = g_contexts;
    g_contexts = c;
    *out = c;
    return ADL_OK;
}

int Destroy(ADL_CONTEXT_HANDLE handle)
{
    if (!handle) {
        return ADL_ERR_INVALID_PARAM;
    }
    AdlContext* found = nullptr;
    {
        Lock lock;
        for (AdlContext** link = &g_contexts; *link; link = &(*link)->next) {
            if (*link == handle && (*link)->magic == kContextMagic) {
                found = *link;
                *link = found->next;
                break;
            }
        }
    }
    if (!found) {
        return ADL_ERR_NOT_INIT;
    }
    DeleteContext(found);
    return ADL_OK;
}

int Refresh(ADL_CONTEXT_HANDLE handle)
{
    AdlSnapshot* fresh = (AdlSnapshot*)HeapBlock(sizeof(AdlSnapshot));
    if (!fresh) {
        return ADL_ERR;
    }
    TakeSnapshot(fresh);
    int result = ADL_ERR_NOT_INIT;
    {
        Lock lock;
        AdlContext* c = Resolve(handle);
        if (c) {
            c->snap = *fresh;
            result = ADL_OK;
        }
    }
    FreeBlock(fresh);
    return result;
}

bool ValidAdapter(const AdlSnapshot* s, int index)
{
    return index >= 0 && index < s->entry_count;
}

ADLDisplayID DisplayId(const AdlDisplay* d)
{
    ADLDisplayID id;
    id.iDisplayLogicalIndex = d->index;
    id.iDisplayPhysicalIndex = d->index;
    id.iDisplayLogicalAdapterIndex = d->entry;
    id.iDisplayPhysicalAdapterIndex = d->entry;
    return id;
}

void FillMode(const AdlDisplay* d, ADLMode* m)
{
    memset(m, 0, sizeof(*m));
    m->iAdapterIndex = d->entry;
    m->displayID = DisplayId(d);
    m->iXPos = d->x;
    m->iYPos = d->y;
    m->iXRes = d->width;
    m->iYRes = d->height;
    m->iColourDepth = 32;
    m->fRefreshRate = d->refresh_hz;
    m->iOrientation = d->orientation;
}

void FillAdapterInfo(const AdlSnapshot* s, int index, AdapterInfo* a)
{
    const AdlEntry* e = &s->entries[index];
    const AdlGpu* g = &s->gpus[e->gpu];
    memset(a, 0, sizeof(*a));
    a->iSize = sizeof(AdapterInfo);
    a->iAdapterIndex = index;
    // The unique device id is the PnP instance id with '\' as '_', as in AMD's library: callers find VEN_, DEV_
    // and REV_ in it (AGS 6.3 does).
    CopyText(a->strUDID, sizeof(a->strUDID), g->instance_id);
    for (char* p = a->strUDID; *p; p++) {
        if (*p == '\\') {
            *p = '_';
        }
    }
    a->iBusNumber = (int)g->bus;
    a->iDeviceNumber = (int)g->device;
    a->iFunctionNumber = (int)g->function;
    a->iVendorID = g->vendor_id == 0x1002 ? ADL_AMD_VENDOR_ID : (int)g->vendor_id;
    CopyText(a->strAdapterName, sizeof(a->strAdapterName), g->name);
    CopyText(a->strDisplayName, sizeof(a->strDisplayName), e->gdi_name);
    a->iPresent = e->present;
    a->iExist = 1;
    if (g->driver_key[0]) {
        static const char kClassRoot[] = "\\Registry\\Machine\\System\\CurrentControlSet\\Control\\Class\\";
        CopyText(a->strDriverPath, sizeof(a->strDriverPath), kClassRoot);
        size_t used = strlen(a->strDriverPath);
        CopyText(a->strDriverPath + used, sizeof(a->strDriverPath) - used, g->driver_key);
    }
    CopyText(a->strPNPString, sizeof(a->strPNPString), g->instance_id);
    a->iOSDisplayIndex = e->os_display_index;
}

// The GPU that an adapter index names, or -1 for "all" (index -1). -2 when the index is not valid.
int GpuOf(const AdlSnapshot* s, int index)
{
    if (index == -1) {
        return -1;
    }
    return ValidAdapter(s, index) ? s->entries[index].gpu : -2;
}

int NumberOfAdapters(ADL_CONTEXT_HANDLE handle, int* count)
{
    if (!count) {
        return ADL_ERR_NULL_POINTER;
    }
    *count = 0;
    Lock lock;
    AdlContext* c = Resolve(handle);
    if (!c) {
        return ADL_ERR_NOT_INIT;
    }
    *count = c->snap.entry_count;
    return ADL_OK;
}

int AdapterInfoArray(ADL_CONTEXT_HANDLE handle, AdapterInfo** out)
{
    if (!out) {
        return ADL_ERR_NULL_POINTER;
    }
    *out = nullptr;
    CallSnapshot call(handle);
    if (call.status != ADL_OK) {
        return call.status;
    }
    if (call.snap->entry_count == 0) {
        return ADL_OK;  // no adapter of ours: nothing allocated
    }
    AdapterInfo* a = (AdapterInfo*)CallerBlock(call.alloc, (size_t)call.snap->entry_count, sizeof(AdapterInfo));
    if (!a) {
        return ADL_ERR;
    }
    for (int i = 0; i < call.snap->entry_count; i++) {
        FillAdapterInfo(call.snap, i, &a[i]);
    }
    *out = a;
    return ADL_OK;
}

int AdapterInfoBuffer(ADL_CONTEXT_HANDLE handle, AdapterInfo* info, int bytes)
{
    if (!info) {
        return ADL_ERR_NULL_POINTER;
    }
    Lock lock;
    AdlContext* c = Resolve(handle);
    if (!c) {
        return ADL_ERR_NOT_INIT;
    }
    size_t need = (size_t)c->snap.entry_count * sizeof(AdapterInfo);
    if (bytes < 0 || (size_t)bytes < need) {
        return ADL_ERR_INVALID_PARAM_SIZE;
    }
    for (int i = 0; i < c->snap.entry_count; i++) {
        FillAdapterInfo(&c->snap, i, &info[i]);
    }
    return ADL_OK;
}

int AdapterActive(ADL_CONTEXT_HANDLE handle, int index, int* status)
{
    if (!status) {
        return ADL_ERR_NULL_POINTER;
    }
    *status = 0;
    Lock lock;
    AdlContext* c = Resolve(handle);
    if (!c) {
        return ADL_ERR_NOT_INIT;
    }
    if (!ValidAdapter(&c->snap, index)) {
        return ADL_ERR_INVALID_ADL_IDX;
    }
    *status = c->snap.entries[index].present;
    return ADL_OK;
}

int MemoryInfo(ADL_CONTEXT_HANDLE handle, int index, ADLMemoryInfo* info)
{
    if (!info) {
        return ADL_ERR_NULL_POINTER;
    }
    memset(info, 0, sizeof(*info));
    Lock lock;
    AdlContext* c = Resolve(handle);
    if (!c) {
        return ADL_ERR_NOT_INIT;
    }
    if (!ValidAdapter(&c->snap, index)) {
        return ADL_ERR_INVALID_ADL_IDX;
    }
    // The dedicated video memory that the kernel-mode driver reports. The memory type and the bandwidth are not
    // known here: empty and 0.
    info->iMemorySize = (long long)c->snap.gpus[c->snap.entries[index].gpu].dedicated_video_bytes;
    return ADL_OK;
}

// The driver version of one GPU. The Catalyst, Crimson (Radeon Software) and web link fields stay empty: our
// driver is no AMD software release.
int Versions(ADL_CONTEXT_HANDLE handle, int index, char* driver_version)
{
    Lock lock;
    AdlContext* c = Resolve(handle);
    if (!c) {
        return ADL_ERR_NOT_INIT;
    }
    if (index == -1) {
        if (c->snap.gpu_count == 0) {
            return ADL_ERR_NOT_SUPPORTED;
        }
        CopyText(driver_version, ADL_MAX_PATH, c->snap.gpus[0].driver_version);
        return ADL_OK;
    }
    if (!ValidAdapter(&c->snap, index)) {
        return ADL_ERR_INVALID_ADL_IDX;
    }
    CopyText(driver_version, ADL_MAX_PATH, c->snap.gpus[c->snap.entries[index].gpu].driver_version);
    return ADL_OK;
}

int DisplayInfo(ADL_CONTEXT_HANDLE handle, int index, int* count, ADLDisplayInfo** out)
{
    if (!count || !out) {
        return ADL_ERR_NULL_POINTER;
    }
    *count = 0;
    *out = nullptr;
    CallSnapshot call(handle);
    if (call.status != ADL_OK) {
        return call.status;
    }
    const AdlSnapshot* s = call.snap;
    int gpu = GpuOf(s, index);
    if (gpu == -2) {
        return ADL_ERR_INVALID_ADL_IDX;
    }
    int n = 0;
    for (int i = 0; i < s->display_count; i++) {
        n += (gpu == -1 || s->displays[i].gpu == gpu) ? 1 : 0;
    }
    if (n == 0) {
        return ADL_OK;
    }
    ADLDisplayInfo* info = (ADLDisplayInfo*)CallerBlock(call.alloc, (size_t)n, sizeof(ADLDisplayInfo));
    if (!info) {
        return ADL_ERR;
    }
    int k = 0;
    for (int i = 0; i < s->display_count; i++) {
        const AdlDisplay* d = &s->displays[i];
        if (gpu != -1 && d->gpu != gpu) {
            continue;
        }
        ADLDisplayInfo* o = &info[k++];
        o->displayID = DisplayId(d);
        o->iDisplayControllerIndex = s->entries[d->entry].source_id;
        CopyText(o->strDisplayName, sizeof(o->strDisplayName), d->name);
        CopyText(o->strDisplayManufacturerName, sizeof(o->strDisplayManufacturerName), d->manufacturer);
        o->iDisplayType = d->display_type;
        o->iDisplayOutputType = d->output_type;
        o->iDisplayConnector = d->connector;
        // Every display of the snapshot is an active path: connected and mapped to a source.
        o->iDisplayInfoMask = ADL_DISPLAY_DISPLAYINFO_DISPLAYCONNECTED | ADL_DISPLAY_DISPLAYINFO_DISPLAYMAPPED;
        o->iDisplayInfoValue = o->iDisplayInfoMask;
    }
    *count = n;
    *out = info;
    return ADL_OK;
}

// The current mode of each selected display. Display index -1 selects the displays of the given adapter (source).
int Modes(ADL_CONTEXT_HANDLE handle, int index, int display, int* count, ADLMode** out)
{
    if (!count || !out) {
        return ADL_ERR_NULL_POINTER;
    }
    *count = 0;
    *out = nullptr;
    CallSnapshot call(handle);
    if (call.status != ADL_OK) {
        return call.status;
    }
    const AdlSnapshot* s = call.snap;
    int gpu = GpuOf(s, index);
    if (gpu == -2) {
        return ADL_ERR_INVALID_ADL_IDX;
    }
    int n = 0;
    for (int pass = 0; pass < 2; pass++) {
        ADLMode* modes = nullptr;
        if (pass == 1) {
            if (n == 0) {
                return display == -1 ? ADL_OK : ADL_ERR_INVALID_DIPLAY_IDX;
            }
            modes = (ADLMode*)CallerBlock(call.alloc, (size_t)n, sizeof(ADLMode));
            if (!modes) {
                return ADL_ERR;
            }
        }
        int k = 0;
        for (int i = 0; i < s->display_count; i++) {
            const AdlDisplay* d = &s->displays[i];
            bool chosen = (gpu == -1 || d->gpu == gpu) &&
                          (display == -1 ? (index == -1 || d->entry == index) : d->index == display);
            if (!chosen) {
                continue;
            }
            if (modes) {
                FillMode(d, &modes[k]);
            }
            k++;
        }
        n = k;
        if (modes) {
            *count = n;
            *out = modes;
        }
    }
    return ADL_OK;
}

// One display map per source with active displays; its targets are those displays (more than one only in a clone
// configuration). No map is an SLS map.
int DisplayMapConfig(ADL_CONTEXT_HANDLE handle, int index, int* map_count, ADLDisplayMap** maps, int* target_count,
                     ADLDisplayTarget** targets)
{
    if (!map_count || !maps || !target_count || !targets) {
        return ADL_ERR_NULL_POINTER;
    }
    *map_count = 0;
    *maps = nullptr;
    *target_count = 0;
    *targets = nullptr;
    CallSnapshot call(handle);
    if (call.status != ADL_OK) {
        return call.status;
    }
    const AdlSnapshot* s = call.snap;
    int gpu = GpuOf(s, index);
    if (gpu == -2) {
        return ADL_ERR_INVALID_ADL_IDX;
    }
    int nmaps = 0, ntargets = 0;
    for (int e = 0; e < s->entry_count; e++) {
        if (gpu != -1 && s->entries[e].gpu != gpu) {
            continue;
        }
        int here = 0;
        for (int i = 0; i < s->display_count; i++) {
            here += s->displays[i].entry == e ? 1 : 0;
        }
        nmaps += here ? 1 : 0;
        ntargets += here;
    }
    if (nmaps == 0) {
        return ADL_OK;
    }
    ADLDisplayMap* m = (ADLDisplayMap*)CallerBlock(call.alloc, (size_t)nmaps, sizeof(ADLDisplayMap));
    if (!m) {
        return ADL_ERR;
    }
    // The map array is the caller's from here on, even if the target array fails: the caller frees both.
    *maps = m;
    *map_count = nmaps;
    ADLDisplayTarget* t = (ADLDisplayTarget*)CallerBlock(call.alloc, (size_t)ntargets, sizeof(ADLDisplayTarget));
    if (!t) {
        return ADL_ERR;
    }
    int mi = 0, ti = 0;
    for (int e = 0; e < s->entry_count; e++) {
        if (gpu != -1 && s->entries[e].gpu != gpu) {
            continue;
        }
        int first = ti;
        for (int i = 0; i < s->display_count; i++) {
            const AdlDisplay* d = &s->displays[i];
            if (d->entry != e) {
                continue;
            }
            if (ti == first) {
                FillMode(d, &m[mi].displayMode);
            }
            t[ti].displayID = DisplayId(d);
            t[ti].iDisplayMapIndex = mi;
            ti++;
        }
        if (ti > first) {
            m[mi].iDisplayMapIndex = mi;
            m[mi].iNumDisplayTarget = ti - first;
            m[mi].iFirstDisplayTargetArrayIndex = first;
            mi++;
        }
    }
    *targets = t;
    *target_count = ntargets;
    return ADL_OK;
}

int SlsMapIndex(ADL_CONTEXT_HANDLE handle, int* sls_index)
{
    if (sls_index) {
        *sls_index = -1;  // no SLS group
    }
    Lock lock;
    return Resolve(handle) ? ADL_ERR_NOT_SUPPORTED : ADL_ERR_NOT_INIT;
}

template <typename T>
void ClearArray(int* count, T** array)
{
    if (count) {
        *count = 0;
    }
    if (array) {
        *array = nullptr;
    }
}

}  // namespace

extern "C" {

int ADL_API ADL2_Main_Control_Create(ADL_MAIN_MALLOC_CALLBACK callback, int iEnumConnectedAdapters,
                                     ADL_CONTEXT_HANDLE* context)
{
    return Create(callback, iEnumConnectedAdapters, context, ADL_THREADING_UNLOCKED);
}

int ADL_API ADL2_Main_ControlX3_Create(ADL_MAIN_MALLOC_CALLBACK callback, int iEnumConnectedAdapters,
                                       ADL_CONTEXT_HANDLE* context, ADLThreadingModel threadingModel,
                                       int adlCreateOptions)
{
    (void)adlCreateOptions;
    return Create(callback, iEnumConnectedAdapters, context, (int)threadingModel);
}

int ADL_API ADL2_Main_Control_Destroy(ADL_CONTEXT_HANDLE context) { return Destroy(context); }

int ADL_API ADL2_Main_Control_Refresh(ADL_CONTEXT_HANDLE context)
{
    return context ? Refresh(context) : ADL_ERR_INVALID_PARAM;
}

int ADL_API ADL2_Adapter_NumberOfAdapters_Get(ADL_CONTEXT_HANDLE context, int* lpNumAdapters)
{
    return NumberOfAdapters(context, lpNumAdapters);
}

int ADL_API ADL2_Adapter_AdapterInfoX2_Get(ADL_CONTEXT_HANDLE context, AdapterInfo** lppAdapterInfo)
{
    return AdapterInfoArray(context, lppAdapterInfo);
}

int ADL_API ADL2_Adapter_AdapterInfo_Get(ADL_CONTEXT_HANDLE context, LPAdapterInfo lpInfo, int iInputSize)
{
    return AdapterInfoBuffer(context, lpInfo, iInputSize);
}

int ADL_API ADL2_Adapter_Active_Get(ADL_CONTEXT_HANDLE context, int iAdapterIndex, int* lpStatus)
{
    return AdapterActive(context, iAdapterIndex, lpStatus);
}

int ADL_API ADL2_Adapter_MemoryInfo_Get(ADL_CONTEXT_HANDLE context, int iAdapterIndex, LPADLMemoryInfo lpMemoryInfo)
{
    return MemoryInfo(context, iAdapterIndex, lpMemoryInfo);
}

int ADL_API ADL2_Graphics_Versions_Get(ADL_CONTEXT_HANDLE context, LPADLVersionsInfo lpVersionsInfo)
{
    if (!lpVersionsInfo) {
        return ADL_ERR_NULL_POINTER;
    }
    memset(lpVersionsInfo, 0, sizeof(*lpVersionsInfo));
    return Versions(context, -1, lpVersionsInfo->strDriverVer);
}

int ADL_API ADL2_Graphics_VersionsX2_Get(ADL_CONTEXT_HANDLE context, LPADLVersionsInfoX2 lpVersionsInfo)
{
    if (!lpVersionsInfo) {
        return ADL_ERR_NULL_POINTER;
    }
    memset(lpVersionsInfo, 0, sizeof(*lpVersionsInfo));
    return Versions(context, -1, lpVersionsInfo->strDriverVer);
}

int ADL_API ADL2_Graphics_VersionsX3_Get(ADL_CONTEXT_HANDLE context, int iAdapterIndex,
                                         LPADLVersionsInfoX2 lpVersionsInfo)
{
    if (!lpVersionsInfo) {
        return ADL_ERR_NULL_POINTER;
    }
    memset(lpVersionsInfo, 0, sizeof(*lpVersionsInfo));
    if (iAdapterIndex == -1) {
        return ADL_ERR_INVALID_ADL_IDX;
    }
    return Versions(context, iAdapterIndex, lpVersionsInfo->strDriverVer);
}

int ADL_API ADL2_Display_DisplayInfo_Get(ADL_CONTEXT_HANDLE context, int iAdapterIndex, int* lpNumDisplays,
                                         ADLDisplayInfo** lppInfo, int iForceDetect)
{
    (void)iForceDetect;
    return DisplayInfo(context, iAdapterIndex, lpNumDisplays, lppInfo);
}

int ADL_API ADL2_Display_Modes_Get(ADL_CONTEXT_HANDLE context, int iAdapterIndex, int iDisplayIndex, int* lpNumModes,
                                   ADLMode** lppModes)
{
    return Modes(context, iAdapterIndex, iDisplayIndex, lpNumModes, lppModes);
}

int ADL_API ADL2_Display_DisplayMapConfig_Get(ADL_CONTEXT_HANDLE context, int iAdapterIndex, int* lpNumDisplayMap,
                                              ADLDisplayMap** lppDisplayMap, int* lpNumDisplayTarget,
                                              ADLDisplayTarget** lppDisplayTarget, int iOptions)
{
    (void)iOptions;
    return DisplayMapConfig(context, iAdapterIndex, lpNumDisplayMap, lppDisplayMap, lpNumDisplayTarget,
                            lppDisplayTarget);
}

int ADL_API ADL2_Display_SLSMapIndex_Get(ADL_CONTEXT_HANDLE context, int iAdapterIndex, int iADLNumDisplayTarget,
                                         ADLDisplayTarget* lpDisplayTarget, int* lpSLSMapIndex)
{
    (void)iAdapterIndex;
    (void)iADLNumDisplayTarget;
    (void)lpDisplayTarget;
    return SlsMapIndex(context, lpSLSMapIndex);
}

int ADL_API ADL2_Display_SLSMapConfig_Get(ADL_CONTEXT_HANDLE context, int iAdapterIndex, int iSLSMapIndex,
                                          ADLSLSMap* lpSLSMap, int* lpNumSLSTarget, ADLSLSTarget** lppSLSTarget,
                                          int* lpNumNativeMode, ADLSLSMode** lppNativeMode, int* lpNumBezelMode,
                                          ADLBezelTransientMode** lppBezelMode, int* lpNumTransientMode,
                                          ADLBezelTransientMode** lppTransientMode, int* lpNumSLSOffset,
                                          ADLSLSOffset** lppSLSOffset, int iOption)
{
    (void)context;
    (void)iAdapterIndex;
    (void)iSLSMapIndex;
    (void)iOption;
    if (lpSLSMap) {
        memset(lpSLSMap, 0, sizeof(*lpSLSMap));
    }
    ClearArray(lpNumSLSTarget, lppSLSTarget);
    ClearArray(lpNumNativeMode, lppNativeMode);
    ClearArray(lpNumBezelMode, lppBezelMode);
    ClearArray(lpNumTransientMode, lppTransientMode);
    ClearArray(lpNumSLSOffset, lppSLSOffset);
    return ADL_ERR_NOT_SUPPORTED;
}

// The ADL_* functions without a handle work on one process-wide context, counted: each Create needs a Destroy.
int ADL_API ADL_Main_Control_Create(ADL_MAIN_MALLOC_CALLBACK callback, int iEnumConnectedAdapters)
{
    if (!callback) {
        return ADL_ERR_INVALID_CALLBACK;
    }
    {
        Lock lock;
        if (g_legacy) {
            g_legacy_refs++;
            return ADL_OK;
        }
    }
    AdlContext* c = NewContext(callback, iEnumConnectedAdapters, ADL_THREADING_UNLOCKED);
    if (!c) {
        return ADL_ERR;
    }
    Lock lock;
    if (g_legacy) {  // another thread was first
        g_legacy_refs++;
        DeleteContext(c);
        return ADL_OK;
    }
    g_legacy = c;
    g_legacy_refs = 1;
    return ADL_OK;
}

int ADL_API ADL_Main_Control_Destroy()
{
    AdlContext* gone = nullptr;
    {
        Lock lock;
        if (!g_legacy) {
            return ADL_ERR_NOT_INIT;
        }
        if (--g_legacy_refs == 0) {
            gone = g_legacy;
            g_legacy = nullptr;
        }
    }
    if (gone) {
        DeleteContext(gone);
    }
    return ADL_OK;
}

int ADL_API ADL_Main_Control_Refresh() { return Refresh(nullptr); }

int ADL_API ADL_Adapter_NumberOfAdapters_Get(int* lpNumAdapters) { return NumberOfAdapters(nullptr, lpNumAdapters); }

int ADL_API ADL_Adapter_AdapterInfoX2_Get(AdapterInfo** lppAdapterInfo)
{
    return AdapterInfoArray(nullptr, lppAdapterInfo);
}

int ADL_API ADL_Adapter_AdapterInfo_Get(LPAdapterInfo lpInfo, int iInputSize)
{
    return AdapterInfoBuffer(nullptr, lpInfo, iInputSize);
}

int ADL_API ADL_Adapter_Active_Get(int iAdapterIndex, int* lpStatus)
{
    return AdapterActive(nullptr, iAdapterIndex, lpStatus);
}

int ADL_API ADL_Adapter_MemoryInfo_Get(int iAdapterIndex, LPADLMemoryInfo lpMemoryInfo)
{
    return MemoryInfo(nullptr, iAdapterIndex, lpMemoryInfo);
}

int ADL_API ADL_Graphics_Versions_Get(LPADLVersionsInfo lpVersionsInfo)
{
    return ADL2_Graphics_Versions_Get(nullptr, lpVersionsInfo);
}

int ADL_API ADL_Display_DisplayInfo_Get(int iAdapterIndex, int* lpNumDisplays, ADLDisplayInfo** lppInfo,
                                        int iForceDetect)
{
    return ADL2_Display_DisplayInfo_Get(nullptr, iAdapterIndex, lpNumDisplays, lppInfo, iForceDetect);
}

int ADL_API ADL_Display_Modes_Get(int iAdapterIndex, int iDisplayIndex, int* lpNumModes, ADLMode** lppModes)
{
    return Modes(nullptr, iAdapterIndex, iDisplayIndex, lpNumModes, lppModes);
}

int ADL_API ADL_Display_DisplayMapConfig_Get(int iAdapterIndex, int* lpNumDisplayMap, ADLDisplayMap** lppDisplayMap,
                                             int* lpNumDisplayTarget, ADLDisplayTarget** lppDisplayTarget,
                                             int iOptions)
{
    return ADL2_Display_DisplayMapConfig_Get(nullptr, iAdapterIndex, lpNumDisplayMap, lppDisplayMap,
                                             lpNumDisplayTarget, lppDisplayTarget, iOptions);
}

int ADL_API ADL_Display_SLSMapIndex_Get(int iAdapterIndex, int iADLNumDisplayTarget,
                                        ADLDisplayTarget* lpDisplayTarget, int* lpSLSMapIndex)
{
    return ADL2_Display_SLSMapIndex_Get(nullptr, iAdapterIndex, iADLNumDisplayTarget, lpDisplayTarget,
                                        lpSLSMapIndex);
}

int ADL_API ADL_Display_SLSMapConfig_Get(int iAdapterIndex, int iSLSMapIndex, ADLSLSMap* lpSLSMap,
                                         int* lpNumSLSTarget, ADLSLSTarget** lppSLSTarget, int* lpNumNativeMode,
                                         ADLSLSMode** lppNativeMode, int* lpNumBezelMode,
                                         ADLBezelTransientMode** lppBezelMode, int* lpNumTransientMode,
                                         ADLBezelTransientMode** lppTransientMode, int* lpNumSLSOffset,
                                         ADLSLSOffset** lppSLSOffset, int iOption)
{
    return ADL2_Display_SLSMapConfig_Get(nullptr, iAdapterIndex, iSLSMapIndex, lpSLSMap, lpNumSLSTarget,
                                         lppSLSTarget, lpNumNativeMode, lppNativeMode, lpNumBezelMode, lppBezelMode,
                                         lpNumTransientMode, lppTransientMode, lpNumSLSOffset, lppSLSOffset,
                                         iOption);
}

#if defined(BC250_ADL_TEST_HOOKS)
// Test build only (never in atiadl.def): replace the discovery with a snapshot, and read the live block count.
__declspec(dllexport) void ADL_API Bc250AdlTest_SetSnapshot(const AdlSnapshot* snapshot)
{
    AdlSnapshot* copy = nullptr;
    if (snapshot) {
        copy = (AdlSnapshot*)HeapAlloc(GetProcessHeap(), 0, sizeof(AdlSnapshot));
        if (copy) {
            *copy = *snapshot;
        }
    }
    AdlSnapshot* old;
    {
        Lock lock;
        old = g_injected;
        g_injected = copy;
    }
    HeapFree(GetProcessHeap(), 0, old);
}

__declspec(dllexport) int ADL_API Bc250AdlTest_LiveBlocks() { return (int)g_live_blocks; }

__declspec(dllexport) void ADL_API Bc250AdlTest_Discover(AdlSnapshot* out) { AdlDiscover(out); }
#endif

}  // extern "C"

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
    } else if (reason == DLL_PROCESS_DETACH && !reserved) {
        // FreeLibrary with contexts still open: they belong to this image, free them. At process exit
        // (reserved != null) the heap goes away with the process.
        while (g_contexts) {
            AdlContext* c = g_contexts;
            g_contexts = c->next;
            DeleteContext(c);
        }
        if (g_legacy) {
            DeleteContext(g_legacy);
            g_legacy = nullptr;
            g_legacy_refs = 0;
        }
    }
    return TRUE;
}
