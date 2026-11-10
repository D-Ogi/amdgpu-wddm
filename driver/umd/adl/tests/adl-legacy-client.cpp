// A second caller module of the ADL_* functions without a handle, for adl-unit-test.cpp (BD-347).
//
// A hardware scanner can load two libraries that both use the implicit ADL context, each with its own allocation
// callback and its own deallocator: in 3DMark SystemInfo, the GPU-Z library (malloc of its CRT, on the process heap)
// and the CPUID library (a private heap). The library must give each module the arrays of its own callback. This
// module is the second caller: it allocates from a private heap, as the CPUID library does, and checks that each
// array that ADL returns to it came from its own callback before it frees the array on that heap.
//
// Exports (cdecl, the ADL module is the argument):
//   LegacyClientCreate(adl)   ADL_Main_Control_Create with the private-heap callback
//   LegacyClientScan(adl)     the scanner's calls; returns the number of failed checks
//   LegacyClientDestroy(adl)  ADL_Main_Control_Destroy
//   LegacyClientAllocations() the number of calls to the private-heap callback
//   LegacyClientNoContext(adl) for a module without its own context: every call that allocates through the
//                             callback or changes the context must return ADL_ERR_NOT_INIT with its outputs clear;
//                             returns the number of failed checks
//   LegacyClientFacts(adl)    ADL_Adapter_NumberOfAdapters_Get, a call that returns no memory: its status
//
// The build makes two files of this source, both at the same fixed base address without ASLR:
// adl-legacy-client.dll and, with ADL_CLIENT_VARIANT_B, adl-legacy-client-b.dll, which is larger. The test loads
// one after the other at that base, so that the second module has the base of the first one and a different key.
#include <windows.h>
#include <stdio.h>

#include "../adl-abi.h"

namespace {

HANDLE g_heap;
LONG g_allocations;
void* g_given[64];  // the blocks that the callback gave and this module did not free yet

void* __stdcall PrivateHeapAlloc(int bytes)
{
    InterlockedIncrement(&g_allocations);
    void* p = HeapAlloc(g_heap, 0, bytes > 0 ? (SIZE_T)bytes : 1);
    for (void*& slot : g_given) {
        if (p && !slot) {
            slot = p;
            break;
        }
    }
    return p;
}

// True when p came from PrivateHeapAlloc and is not freed yet; the record is removed.
bool TakeRecord(void* p)
{
    for (void*& slot : g_given) {
        if (p && slot == p) {
            slot = nullptr;
            return true;
        }
    }
    return false;
}

typedef int(ADL_API* PFN_LegacyCreate)(ADL_MAIN_MALLOC_CALLBACK, int);
typedef int(ADL_API* PFN_LegacyVoid)();
typedef int(ADL_API* PFN_LegacyCount)(int*);
typedef int(ADL_API* PFN_LegacyInfoX2)(AdapterInfo**);
typedef int(ADL_API* PFN_LegacyDisplayInfo)(int, int*, ADLDisplayInfo**, int);
typedef int(ADL_API* PFN_LegacyModes)(int, int, int*, ADLMode**);
typedef int(ADL_API* PFN_LegacyMapConfig)(int, int*, ADLDisplayMap**, int*, ADLDisplayTarget**, int);
typedef int(ADL_API* PFN_DisplayInfo)(ADL_CONTEXT_HANDLE, int, int*, ADLDisplayInfo**, int);
typedef int(ADL_API* PFN_LegacySlsIndex)(int, int, ADLDisplayTarget*, int*);
typedef int(ADL_API* PFN_LegacySlsConfig)(int, int, ADLSLSMap*, int*, ADLSLSTarget**, int*, ADLSLSMode**, int*,
                                          ADLBezelTransientMode**, int*, ADLBezelTransientMode**, int*,
                                          ADLSLSOffset**, int);

#if defined(ADL_CLIENT_VARIANT_B)
volatile char g_variant_pad[65536] = {1};  // makes this file larger than the first variant
#endif

template <typename T>
T Get(HMODULE adl, const char* name)
{
    return reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(adl, name)));
}

int g_failures;

void Check(bool ok, const char* what)
{
    if (!ok) {
        g_failures++;
        printf("FAIL legacy client: %s\n", what);
    }
}

// An array that ADL returned to this module: it must come from this module's callback. Only then is it freed here;
// a block of another allocator stays allocated (a leak in a failed test, not a heap corruption).
void TakeArray(void* p, const char* what)
{
    if (!p) {
        Check(false, what);
        return;
    }
    bool own = TakeRecord(p);
    Check(own, what);
    if (own) {
        HeapFree(g_heap, 0, p);
    }
}

// The library finds its caller from the return address. A call in tail position can become a jump, and then the
// return address is in the module that called this one. The CPUID library makes ordinary calls (the BD-347 dump), so
// this module does the same: each ADL status goes through this function after the call returns.
volatile int g_last_status;

int Keep(int status)
{
    g_last_status = status;
    return g_last_status;
}

}  // namespace

extern "C" __declspec(dllexport) int __cdecl LegacyClientCreate(HMODULE adl)
{
    if (!g_heap) {
        g_heap = HeapCreate(0, 0, 0);
    }
    auto create = Get<PFN_LegacyCreate>(adl, "ADL_Main_Control_Create");
    return g_heap && create ? Keep(create(PrivateHeapAlloc, 1)) : ADL_ERR;
}

extern "C" __declspec(dllexport) int __cdecl LegacyClientScan(HMODULE adl)
{
    g_failures = 0;
    auto count = Get<PFN_LegacyCount>(adl, "ADL_Adapter_NumberOfAdapters_Get");
    auto infoX2 = Get<PFN_LegacyInfoX2>(adl, "ADL_Adapter_AdapterInfoX2_Get");
    auto displayInfo = Get<PFN_LegacyDisplayInfo>(adl, "ADL_Display_DisplayInfo_Get");
    auto modes = Get<PFN_LegacyModes>(adl, "ADL_Display_Modes_Get");
    auto mapConfig = Get<PFN_LegacyMapConfig>(adl, "ADL_Display_DisplayMapConfig_Get");
    auto displayInfo2 = Get<PFN_DisplayInfo>(adl, "ADL2_Display_DisplayInfo_Get");
    if (!count || !infoX2 || !displayInfo || !modes || !mapConfig || !displayInfo2) {
        Check(false, "missing export");
        return g_failures;
    }
    int n = 0;
    Check(count(&n) == ADL_OK && n > 0, "ADL_Adapter_NumberOfAdapters_Get");
    AdapterInfo* ai = nullptr;
    Check(infoX2(&ai) == ADL_OK, "ADL_Adapter_AdapterInfoX2_Get status");
    TakeArray(ai, "ADL_Adapter_AdapterInfoX2_Get array from this module's callback");
    // The call of the BD-347 crash: per adapter, the display list, then the free of this module.
    for (int i = 0; i < n; i++) {
        int nd = 0;
        ADLDisplayInfo* di = nullptr;
        if (displayInfo(i, &nd, &di, 0) == ADL_OK && nd > 0) {
            TakeArray(di, "ADL_Display_DisplayInfo_Get array from this module's callback");
        }
    }
    int nd = 0;
    ADLDisplayInfo* di = nullptr;
    Check(displayInfo2(nullptr, -1, &nd, &di, 0) == ADL_OK && nd > 0, "ADL2_Display_DisplayInfo_Get, null handle");
    TakeArray(di, "ADL2_Display_DisplayInfo_Get (null handle) array from this module's callback");
    int nm = 0;
    ADLMode* m = nullptr;
    Check(modes(-1, -1, &nm, &m) == ADL_OK && nm > 0, "ADL_Display_Modes_Get status");
    TakeArray(m, "ADL_Display_Modes_Get array from this module's callback");
    int nmaps = 0, ntargets = 0;
    ADLDisplayMap* maps = nullptr;
    ADLDisplayTarget* targets = nullptr;
    Check(mapConfig(-1, &nmaps, &maps, &ntargets, &targets, 0) == ADL_OK && nmaps > 0, "DisplayMapConfig status");
    TakeArray(maps, "ADL_Display_DisplayMapConfig_Get map array from this module's callback");
    TakeArray(targets, "ADL_Display_DisplayMapConfig_Get target array from this module's callback");
    return g_failures;
}

extern "C" __declspec(dllexport) int __cdecl LegacyClientDestroy(HMODULE adl)
{
    auto destroy = Get<PFN_LegacyVoid>(adl, "ADL_Main_Control_Destroy");
    return destroy ? Keep(destroy()) : ADL_ERR;
}

extern "C" __declspec(dllexport) int __cdecl LegacyClientAllocations()
{
#if defined(ADL_CLIENT_VARIANT_B)
    (void)g_variant_pad[0];
#endif
    return (int)g_allocations;
}

extern "C" __declspec(dllexport) int __cdecl LegacyClientNoContext(HMODULE adl)
{
    g_failures = 0;
    auto refresh = Get<PFN_LegacyVoid>(adl, "ADL_Main_Control_Refresh");
    auto destroy = Get<PFN_LegacyVoid>(adl, "ADL_Main_Control_Destroy");
    auto infoX2 = Get<PFN_LegacyInfoX2>(adl, "ADL_Adapter_AdapterInfoX2_Get");
    auto displayInfo = Get<PFN_LegacyDisplayInfo>(adl, "ADL_Display_DisplayInfo_Get");
    auto modes = Get<PFN_LegacyModes>(adl, "ADL_Display_Modes_Get");
    auto mapConfig = Get<PFN_LegacyMapConfig>(adl, "ADL_Display_DisplayMapConfig_Get");
    auto slsIndex = Get<PFN_LegacySlsIndex>(adl, "ADL_Display_SLSMapIndex_Get");
    auto slsConfig = Get<PFN_LegacySlsConfig>(adl, "ADL_Display_SLSMapConfig_Get");
    auto displayInfo2 = Get<PFN_DisplayInfo>(adl, "ADL2_Display_DisplayInfo_Get");
    if (!refresh || !destroy || !infoX2 || !displayInfo || !modes || !mapConfig || !slsIndex || !slsConfig ||
        !displayInfo2) {
        Check(false, "missing export");
        return g_failures;
    }
    const int before = (int)g_allocations;
    Check(Keep(refresh()) == ADL_ERR_NOT_INIT, "ADL_Main_Control_Refresh without a context of this module");
    Check(Keep(destroy()) == ADL_ERR_NOT_INIT, "ADL_Main_Control_Destroy without a context of this module");
    AdapterInfo* ai = reinterpret_cast<AdapterInfo*>(1);
    Check(Keep(infoX2(&ai)) == ADL_ERR_NOT_INIT && !ai, "ADL_Adapter_AdapterInfoX2_Get without a context");
    int n = 9;
    ADLDisplayInfo* di = reinterpret_cast<ADLDisplayInfo*>(1);
    Check(Keep(displayInfo(-1, &n, &di, 0)) == ADL_ERR_NOT_INIT && !n && !di, "ADL_Display_DisplayInfo_Get");
    n = 9;
    di = reinterpret_cast<ADLDisplayInfo*>(1);
    Check(Keep(displayInfo2(nullptr, -1, &n, &di, 0)) == ADL_ERR_NOT_INIT && !n && !di,
          "ADL2_Display_DisplayInfo_Get, null handle");
    n = 9;
    ADLMode* m = reinterpret_cast<ADLMode*>(1);
    Check(Keep(modes(-1, -1, &n, &m)) == ADL_ERR_NOT_INIT && !n && !m, "ADL_Display_Modes_Get");
    int nmaps = 9, ntargets = 9;
    ADLDisplayMap* maps = reinterpret_cast<ADLDisplayMap*>(1);
    ADLDisplayTarget* targets = reinterpret_cast<ADLDisplayTarget*>(1);
    Check(Keep(mapConfig(-1, &nmaps, &maps, &ntargets, &targets, 0)) == ADL_ERR_NOT_INIT && !nmaps && !maps &&
              !ntargets && !targets,
          "ADL_Display_DisplayMapConfig_Get");
    int sls = 5;
    Check(Keep(slsIndex(0, 0, nullptr, &sls)) == ADL_ERR_NOT_INIT && sls == -1, "ADL_Display_SLSMapIndex_Get");
    n = 9;
    ADLSLSTarget* st = reinterpret_cast<ADLSLSTarget*>(1);
    Check(Keep(slsConfig(0, 0, nullptr, &n, &st, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                         nullptr, 0)) == ADL_ERR_NOT_INIT && !n && !st,
          "ADL_Display_SLSMapConfig_Get");
    Check((int)g_allocations == before, "no call of this module's callback");
    return g_failures;
}

extern "C" __declspec(dllexport) int __cdecl LegacyClientFacts(HMODULE adl)
{
    auto count = Get<PFN_LegacyCount>(adl, "ADL_Adapter_NumberOfAdapters_Get");
    int n = 0;
    return count ? Keep(count(&n)) : ADL_ERR;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
    } else if (reason == DLL_PROCESS_DETACH && g_heap) {
        HeapDestroy(g_heap);
        g_heap = nullptr;
    }
    return TRUE;
}
