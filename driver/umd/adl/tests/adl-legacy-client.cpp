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

extern "C" __declspec(dllexport) int __cdecl LegacyClientAllocations() { return (int)g_allocations; }

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
