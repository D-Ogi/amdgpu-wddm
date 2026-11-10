// Host test of the open ADL subset: every export of the test build (atiadl-test.def) against an injected snapshot,
// and the discovery against the host's real adapters. No GPU work.
//
//   adl-unit-test.exe <test atiadlxx.dll or atiadlxy.dll> [unit|discover] [<adl-legacy-client.dll>]
//
// unit      the export contract: return codes, cleared outputs, allocation through the caller's callback, the
//           AGS 6.3 strides, context ownership (1000 Create/Destroy cycles, no live block left), the legacy
//           context, Refresh, and on x86 that every export leaves its arguments to the caller (cdecl). With a
//           second caller module (adl-legacy-client.dll, by default next to this program): each module that uses
//           the context without a handle gets the arrays of its own callback (BD-347).
// discover  prints the snapshot that the discovery takes on this machine (for the build with BC250_ADL_TEST_SERVICE,
//           a host adapter's service stands for ours).
// Exit code 0 when every check passes.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../adl-abi.h"
#include "../adl-model.h"
#include "test-snapshot.h"

namespace {

using test_snapshot::MakeSnapshot;

int g_failures;
int g_checks;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        g_checks++;                                                                    \
        if (!(cond)) {                                                                 \
            g_failures++;                                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                     \
        }                                                                              \
    } while (0)

// The caller's allocator: counts what the library hands out, the test frees it.
LONG g_outstanding;
LONG g_allocations;

void* __stdcall CountingAlloc(int bytes)
{
    void* p = malloc(bytes > 0 ? (size_t)bytes : 1);
    if (p) {
        memset(p, 0xcd, (size_t)(bytes > 0 ? bytes : 1));  // the library must zero what it returns
        InterlockedIncrement(&g_outstanding);
        InterlockedIncrement(&g_allocations);
    }
    return p;
}

void CallerFree(void* p)
{
    if (p) {
        free(p);
        InterlockedDecrement(&g_outstanding);
    }
}

void* __stdcall FailingAlloc(int) { return nullptr; }

// The allocator of this module in the two-module test: a private heap, and a record of every block that it gave.
HANDLE g_tracked_heap;
void* g_tracked[64];

void* __stdcall TrackedAlloc(int bytes)
{
    void* p = HeapAlloc(g_tracked_heap, 0, bytes > 0 ? (SIZE_T)bytes : 1);
    for (void*& slot : g_tracked) {
        if (p && !slot) {
            slot = p;
            break;
        }
    }
    return p;
}

// True when p came from TrackedAlloc; then it is freed. Any other block stays allocated (a leak in a failed test).
bool FreeTracked(void* p)
{
    for (void*& slot : g_tracked) {
        if (p && slot == p) {
            slot = nullptr;
            HeapFree(g_tracked_heap, 0, p);
            return true;
        }
    }
    return false;
}

typedef int(ADL_API* PFN_Create)(ADL_MAIN_MALLOC_CALLBACK, int, ADL_CONTEXT_HANDLE*);
typedef int(ADL_API* PFN_CreateX3)(ADL_MAIN_MALLOC_CALLBACK, int, ADL_CONTEXT_HANDLE*, ADLThreadingModel, int);
typedef int(ADL_API* PFN_Destroy)(ADL_CONTEXT_HANDLE);
typedef int(ADL_API* PFN_Refresh)(ADL_CONTEXT_HANDLE);
typedef int(ADL_API* PFN_Count)(ADL_CONTEXT_HANDLE, int*);
typedef int(ADL_API* PFN_InfoX2)(ADL_CONTEXT_HANDLE, AdapterInfo**);
typedef int(ADL_API* PFN_Info)(ADL_CONTEXT_HANDLE, LPAdapterInfo, int);
typedef int(ADL_API* PFN_Active)(ADL_CONTEXT_HANDLE, int, int*);
typedef int(ADL_API* PFN_Memory)(ADL_CONTEXT_HANDLE, int, LPADLMemoryInfo);
typedef int(ADL_API* PFN_Versions)(ADL_CONTEXT_HANDLE, LPADLVersionsInfo);
typedef int(ADL_API* PFN_VersionsX2)(ADL_CONTEXT_HANDLE, LPADLVersionsInfoX2);
typedef int(ADL_API* PFN_VersionsX3)(ADL_CONTEXT_HANDLE, int, LPADLVersionsInfoX2);
typedef int(ADL_API* PFN_DisplayInfo)(ADL_CONTEXT_HANDLE, int, int*, ADLDisplayInfo**, int);
typedef int(ADL_API* PFN_Modes)(ADL_CONTEXT_HANDLE, int, int, int*, ADLMode**);
typedef int(ADL_API* PFN_MapConfig)(ADL_CONTEXT_HANDLE, int, int*, ADLDisplayMap**, int*, ADLDisplayTarget**, int);
typedef int(ADL_API* PFN_SlsIndex)(ADL_CONTEXT_HANDLE, int, int, ADLDisplayTarget*, int*);
typedef int(ADL_API* PFN_SlsConfig)(ADL_CONTEXT_HANDLE, int, int, ADLSLSMap*, int*, ADLSLSTarget**, int*, ADLSLSMode**,
                                    int*, ADLBezelTransientMode**, int*, ADLBezelTransientMode**, int*,
                                    ADLSLSOffset**, int);
typedef int(ADL_API* PFN_LegacyCreate)(ADL_MAIN_MALLOC_CALLBACK, int);
typedef int(ADL_API* PFN_LegacyVoid)();
typedef int(ADL_API* PFN_LegacyCount)(int*);
typedef int(ADL_API* PFN_LegacyInfoX2)(AdapterInfo**);
typedef int(ADL_API* PFN_LegacyInfo)(LPAdapterInfo, int);
typedef int(ADL_API* PFN_LegacyActive)(int, int*);
typedef int(ADL_API* PFN_LegacyMemory)(int, LPADLMemoryInfo);
typedef int(ADL_API* PFN_LegacyVersions)(LPADLVersionsInfo);
typedef int(ADL_API* PFN_LegacyDisplayInfo)(int, int*, ADLDisplayInfo**, int);
typedef int(ADL_API* PFN_LegacyModes)(int, int, int*, ADLMode**);
typedef int(ADL_API* PFN_LegacyMapConfig)(int, int*, ADLDisplayMap**, int*, ADLDisplayTarget**, int);
typedef int(ADL_API* PFN_LegacySlsIndex)(int, int, ADLDisplayTarget*, int*);
typedef int(ADL_API* PFN_LegacySlsConfig)(int, int, ADLSLSMap*, int*, ADLSLSTarget**, int*, ADLSLSMode**, int*,
                                          ADLBezelTransientMode**, int*, ADLBezelTransientMode**, int*,
                                          ADLSLSOffset**, int);
typedef void(ADL_API* PFN_SetSnapshot)(const AdlSnapshot*);
typedef int(ADL_API* PFN_LiveBlocks)();
typedef void(ADL_API* PFN_Discover)(AdlSnapshot*);
typedef void(ADL_API* PFN_IgnoreUnload)(int);

HMODULE g_dll;
const char* g_client_path;
const char* g_client_b_path;  // the larger variant of the second caller module, at the same base

template <typename T>
T Get(const char* name)
{
    FARPROC p = GetProcAddress(g_dll, name);
    if (!p) {
        printf("FAIL missing export %s\n", name);
        g_failures++;
    }
    return reinterpret_cast<T>(reinterpret_cast<void*>(p));
}

bool AllZero(const void* p, size_t n)
{
    const unsigned char* b = (const unsigned char*)p;
    for (size_t i = 0; i < n; i++) {
        if (b[i]) {
            return false;
        }
    }
    return true;
}

#if defined(_M_IX86)
// Calls fn with count 32-bit arguments and stores how many bytes of arguments the callee removed from the stack:
// 0 for cdecl, 4 * count for stdcall. ESP is restored either way.
extern "C" __declspec(naked) int __cdecl CallAndMeasure(void* fn, const int* args, int count, int* popped)
{
    __asm {
        push ebp
        mov ebp, esp
        push ebx
        push esi
        push edi
        mov esi, esp
        mov ecx, [ebp + 16]
        mov edx, [ebp + 12]
    next_arg:
        test ecx, ecx
        jz do_call
        dec ecx
        push dword ptr [edx + ecx * 4]
        jmp next_arg
    do_call:
        mov edi, esp
        call dword ptr [ebp + 8]
        mov ecx, esp
        sub ecx, edi
        mov edx, [ebp + 20]
        mov [edx], ecx
        mov esp, esi
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret
    }
}

void CheckCdecl(const char* name, const int* args, int count)
{
    void* fn = (void*)GetProcAddress(g_dll, name);
    int popped = -1;
    if (!fn) {
        printf("FAIL missing export %s\n", name);
        g_failures++;
        return;
    }
    CallAndMeasure(fn, args, count, &popped);
    g_checks++;
    if (popped != 0) {
        printf("FAIL %s removed %d bytes of arguments (not cdecl)\n", name, popped);
        g_failures++;
    }
}
#endif

ADL_CONTEXT_HANDLE g_reentry_context;
int g_reentry_action;
int g_reentry_calls;

void* __stdcall ReentrantAlloc(int bytes)
{
    if (++g_reentry_calls == 1) {
        int count = -1;
        CHECK(Get<PFN_Count>("ADL2_Adapter_NumberOfAdapters_Get")(g_reentry_context, &count) == ADL_OK && count == 3);
        if (g_reentry_action == 1) {
            static AdlSnapshot empty = {};
            Get<PFN_SetSnapshot>("Bc250AdlTest_SetSnapshot")(&empty);
            CHECK(Get<PFN_Refresh>("ADL2_Main_Control_Refresh")(g_reentry_context) == ADL_OK);
            CHECK(Get<PFN_Count>("ADL2_Adapter_NumberOfAdapters_Get")(g_reentry_context, &count) == ADL_OK && count == 0);
        } else if (g_reentry_action == 2) {
            CHECK(Get<PFN_Destroy>("ADL2_Main_Control_Destroy")(g_reentry_context) == ADL_OK);
        }
    }
    return CountingAlloc(bytes);
}

DWORD WINAPI ReentryTests(void*)
{
    static AdlSnapshot original;
    MakeSnapshot(&original);
    for (int action = 0; action < 3; action++) {
        for (int api = 0; api < 4; api++) {
            Get<PFN_SetSnapshot>("Bc250AdlTest_SetSnapshot")(&original);
            g_reentry_calls = 0;
            g_reentry_action = action;
            CHECK(Get<PFN_Create>("ADL2_Main_Control_Create")(ReentrantAlloc, 1, &g_reentry_context) == ADL_OK);
            if (api == 0) {
                AdapterInfo* out = nullptr;
                CHECK(Get<PFN_InfoX2>("ADL2_Adapter_AdapterInfoX2_Get")(g_reentry_context, &out) == ADL_OK);
                CHECK(out && out[2].iAdapterIndex == 2 && strcmp(out[0].strAdapterName, "Test GPU zero") == 0);
                CallerFree(out);
            } else if (api == 1) {
                int n = 0;
                ADLDisplayInfo* out = nullptr;
                CHECK(Get<PFN_DisplayInfo>("ADL2_Display_DisplayInfo_Get")(g_reentry_context, 0, &n, &out, 0) == ADL_OK);
                CHECK(n == 3 && out && strcmp(out[0].strDisplayName, "Monitor A") == 0);
                CallerFree(out);
            } else if (api == 2) {
                int n = 0;
                ADLMode* out = nullptr;
                CHECK(Get<PFN_Modes>("ADL2_Display_Modes_Get")(g_reentry_context, 0, -1, &n, &out) == ADL_OK);
                CHECK(n == 2 && out && out[0].iXRes == 2560);
                CallerFree(out);
            } else {
                int nm = 0, nt = 0;
                ADLDisplayMap* maps = nullptr;
                ADLDisplayTarget* targets = nullptr;
                CHECK(Get<PFN_MapConfig>("ADL2_Display_DisplayMapConfig_Get")(g_reentry_context, 0, &nm, &maps, &nt, &targets, 0) == ADL_OK);
                CHECK(nm == 2 && nt == 3 && maps && targets && maps[0].iNumDisplayTarget == 2);
                CallerFree(maps);
                CallerFree(targets);
            }
            CHECK(g_reentry_calls == (api == 3 ? 2 : 1));
            if (action != 2) {
                CHECK(Get<PFN_Destroy>("ADL2_Main_Control_Destroy")(g_reentry_context) == ADL_OK);
            }
            CHECK(Get<PFN_LiveBlocks>("Bc250AdlTest_LiveBlocks")() == 0 && g_outstanding == 0);
        }
    }
    Get<PFN_SetSnapshot>("Bc250AdlTest_SetSnapshot")(&original);
    return 0;
}

void CheckReentryBounded()
{
    HANDLE thread = CreateThread(nullptr, 0, ReentryTests, nullptr, 0, nullptr);
    CHECK(thread != nullptr);
    if (!thread) {
        return;
    }
    if (WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0) {
        // A recursive-lock regression must fail the gate, not hang the build or leave a blocked worker alive.
        puts("FAIL allocation callback reentry exceeded 5 seconds");
        fflush(stdout);
        ExitProcess(1);
    }
    CloseHandle(thread);
}

int g_failure_call;
void* __stdcall FailSecondAlloc(int bytes)
{
    return ++g_failure_call == 2 ? nullptr : CountingAlloc(bytes);
}

typedef int(__cdecl* PFN_ClientCall)(HMODULE);
typedef int(__cdecl* PFN_ClientCount)();

template <typename T>
T GetFrom(HMODULE module, const char* name)
{
    return reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(module, name)));
}

// Calls fn with two pointer-size arguments from code outside any module: a caller that the library cannot name. A
// function with fewer arguments ignores the others (cdecl: the caller removes them).
int CallFromAnonymousCode(void* fn, void* arg = nullptr, void* arg2 = nullptr)
{
#if defined(_M_IX86)
    // mov eax, fn; push arg2; push arg; call eax; add esp, 8; ret
    unsigned char code[] = {0xB8, 0, 0, 0, 0, 0x68, 0, 0, 0, 0, 0x68, 0, 0, 0, 0,
                            0xFF, 0xD0, 0x83, 0xC4, 0x08, 0xC3};
    memcpy(code + 1, &fn, sizeof(fn));
    memcpy(code + 6, &arg2, sizeof(arg2));
    memcpy(code + 11, &arg, sizeof(arg));
#elif defined(_M_X64)
    // sub rsp, 28h; mov rcx, arg; mov rdx, arg2; mov rax, fn; call rax; add rsp, 28h; ret
    unsigned char code[] = {0x48, 0x83, 0xEC, 0x28, 0x48, 0xB9, 0, 0, 0, 0, 0, 0, 0, 0, 0x48, 0xBA, 0, 0, 0, 0, 0, 0,
                            0, 0, 0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xD0, 0x48, 0x83, 0xC4, 0x28, 0xC3};
    memcpy(code + 6, &arg, sizeof(arg));
    memcpy(code + 16, &arg2, sizeof(arg2));
    memcpy(code + 26, &fn, sizeof(fn));
#else
#error "CallFromAnonymousCode supports x86 and x64"
#endif
    void* page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!page) {
        return ADL_ERR;
    }
    memcpy(page, code, sizeof(code));
    DWORD old = 0;
    VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), page, sizeof(code));
    int rc = reinterpret_cast<int(__cdecl*)()>(page)();
    VirtualFree(page, 0, MEM_RELEASE);
    return rc;
}

// The second caller module: adl-legacy-client.dll and its exports.
struct LegacyClient {
    HMODULE module = nullptr;
    PFN_ClientCall create = nullptr;
    PFN_ClientCall scan = nullptr;
    PFN_ClientCall destroy = nullptr;
    PFN_ClientCount allocations = nullptr;
    PFN_ClientCall no_context = nullptr;
    PFN_ClientCall facts = nullptr;

    bool Load(const char* path = g_client_path)
    {
        module = LoadLibraryA(path);
        if (!module) {
            printf("FAIL cannot load the second caller module %s (error %lu)\n", path, GetLastError());
            g_failures++;
            return false;
        }
        create = GetFrom<PFN_ClientCall>(module, "LegacyClientCreate");
        scan = GetFrom<PFN_ClientCall>(module, "LegacyClientScan");
        destroy = GetFrom<PFN_ClientCall>(module, "LegacyClientDestroy");
        allocations = GetFrom<PFN_ClientCount>(module, "LegacyClientAllocations");
        no_context = GetFrom<PFN_ClientCall>(module, "LegacyClientNoContext");
        facts = GetFrom<PFN_ClientCall>(module, "LegacyClientFacts");
        bool all = create && scan && destroy && allocations && no_context && facts;
        CHECK(all);
        return all;
    }

    void Unload()
    {
        if (module) {
            FreeLibrary(module);
        }
        *this = LegacyClient();
    }
};

// BD-347: two modules of one process use the context without a handle, each with its own allocator and its own
// deallocator. This program is the first (the GPU-Z library of 3DMark SystemInfo calls ADL_Main_Control_Create
// first); adl-legacy-client.dll is the second (the CPUID library: a private heap, and the free of that heap after
// ADL_Display_DisplayInfo_Get). Each module must get the arrays of its own callback.
void CheckTwoCallerModules()
{
    LegacyClient client;
    if (!client.Load()) {
        client.Unload();
        return;
    }
    auto lCreate = Get<PFN_LegacyCreate>("ADL_Main_Control_Create");
    auto lDestroy = Get<PFN_LegacyVoid>("ADL_Main_Control_Destroy");
    auto lRefresh = Get<PFN_LegacyVoid>("ADL_Main_Control_Refresh");
    auto lCount = Get<PFN_LegacyCount>("ADL_Adapter_NumberOfAdapters_Get");
    auto lInfoX2 = Get<PFN_LegacyInfoX2>("ADL_Adapter_AdapterInfoX2_Get");
    auto lDisplayInfo = Get<PFN_LegacyDisplayInfo>("ADL_Display_DisplayInfo_Get");
    auto liveBlocks = Get<PFN_LiveBlocks>("Bc250AdlTest_LiveBlocks");
    g_tracked_heap = HeapCreate(0, 0, 0);
    CHECK(g_tracked_heap != nullptr);

    // The order of the scanner: this module creates first, the second module creates and scans.
    CHECK(lCreate(TrackedAlloc, 1) == ADL_OK);
    CHECK(client.create(g_dll) == ADL_OK);
    CHECK(liveBlocks() == 2);  // one implicit context per module
    int before = client.allocations();
    CHECK(client.scan(g_dll) == 0);
    CHECK(client.allocations() > before);
    // A caller outside any image (generated code) has no context of its own. A call that returns no memory uses the
    // newest implicit context; an array call, Refresh and Destroy get ADL_ERR_NOT_INIT and change nothing.
    int n = -1;
    CHECK(CallFromAnonymousCode(reinterpret_cast<void*>(lCount), &n) == ADL_OK && n == 3);
    AdapterInfo* ai = reinterpret_cast<AdapterInfo*>(1);
    CHECK(CallFromAnonymousCode(reinterpret_cast<void*>(lInfoX2), &ai) == ADL_ERR_NOT_INIT && ai == nullptr);
    CHECK(CallFromAnonymousCode(reinterpret_cast<void*>(lRefresh)) == ADL_ERR_NOT_INIT);
    CHECK(CallFromAnonymousCode(reinterpret_cast<void*>(lDestroy)) == ADL_ERR_NOT_INIT && liveBlocks() == 2);
    // The arrays of this module still come from its own callback.
    ADLDisplayInfo* di = nullptr;
    CHECK(lDisplayInfo(0, &n, &di, 0) == ADL_OK && n == 3);
    CHECK(FreeTracked(di));
    ai = nullptr;
    CHECK(lInfoX2(&ai) == ADL_OK);
    CHECK(FreeTracked(ai));
    // Counted per module: the second module creates again and scans again; then it releases both of its Creates.
    CHECK(client.create(g_dll) == ADL_OK && liveBlocks() == 2);
    CHECK(client.scan(g_dll) == 0);
    CHECK(client.destroy(g_dll) == ADL_OK && liveBlocks() == 2);
    CHECK(client.destroy(g_dll) == ADL_OK && liveBlocks() == 1);
    // The context of this module stays, with its callback.
    di = nullptr;
    CHECK(lDisplayInfo(1, &n, &di, 0) == ADL_OK && n == 3);
    CHECK(FreeTracked(di));
    // A module without its own Create (the second module now): the array calls, Refresh and Destroy get
    // ADL_ERR_NOT_INIT, its callback is not called and the context of this module stays; a call that returns no
    // memory gets the facts of the newest implicit context.
    CHECK(client.no_context(g_dll) == 0);
    CHECK(client.facts(g_dll) == ADL_OK);
    CHECK(liveBlocks() == 1);
    // A Destroy of one module leaves the context of the other module alive.
    CHECK(client.create(g_dll) == ADL_OK && liveBlocks() == 2);
    CHECK(lDestroy() == ADL_OK && liveBlocks() == 1);
    CHECK(lDestroy() == ADL_ERR_NOT_INIT && liveBlocks() == 1);
    CHECK(client.scan(g_dll) == 0);
    CHECK(client.destroy(g_dll) == ADL_OK && liveBlocks() == 0);
    CHECK(client.destroy(g_dll) == ADL_ERR_NOT_INIT);
    // No implicit context at all: the facts calls get ADL_ERR_NOT_INIT too.
    CHECK(client.facts(g_dll) == ADL_ERR_NOT_INIT);
    CHECK(CallFromAnonymousCode(reinterpret_cast<void*>(lCount), &n) == ADL_ERR_NOT_INIT);
    // A module that unloads without Destroy: its context goes at the unload (the loader notification). A module that
    // loads later at the same address starts with no context and does not get the callback of the earlier one.
    CHECK(lCreate(TrackedAlloc, 1) == ADL_OK);
    CHECK(client.create(g_dll) == ADL_OK && liveBlocks() == 2);
    client.Unload();
    CHECK(liveBlocks() == 1);
    if (client.Load()) {
        CHECK(client.no_context(g_dll) == 0);
        CHECK(client.create(g_dll) == ADL_OK && liveBlocks() == 2);
        CHECK(client.scan(g_dll) == 0);
        CHECK(client.destroy(g_dll) == ADL_OK && liveBlocks() == 1);
    }
    client.Unload();
    // The key alone, with the notification ignored: the second module loads at the base of the first one, which
    // unloaded without Destroy. Its key differs (size, time stamp), so it does not get the context that the first
    // module left: that context is a leak of one block until the notification of that base.
    auto ignoreUnload = Get<PFN_IgnoreUnload>("Bc250AdlTest_IgnoreUnload");
    ignoreUnload(1);
    LegacyClient first;
    if (first.Load()) {
        HMODULE base = first.module;
        CHECK(first.create(g_dll) == ADL_OK && liveBlocks() == 2);
        first.Unload();
        CHECK(liveBlocks() == 2);
        LegacyClient second;
        if (second.Load(g_client_b_path)) {
            CHECK(second.module == base);
            CHECK(second.no_context(g_dll) == 0);
            CHECK(second.create(g_dll) == ADL_OK && liveBlocks() == 3);
            CHECK(second.scan(g_dll) == 0);
            CHECK(second.destroy(g_dll) == ADL_OK && liveBlocks() == 2);
            ignoreUnload(0);
            second.Unload();  // the notification of this base also removes the context that the first module left
            CHECK(liveBlocks() == 1);
        }
    }
    ignoreUnload(0);
    CHECK(lDestroy() == ADL_OK && liveBlocks() == 0);
    for (void* p : g_tracked) {
        CHECK(p == nullptr);
    }
    HeapDestroy(g_tracked_heap);
    g_tracked_heap = nullptr;
}

// The presence exports (docs/design/adl-subset.md, "Presence exports"), with the SDK prototypes.
typedef int(ADL_API* PFN_AdapterId)(int, int*);
typedef int(ADL_API* PFN_CrossfireCaps)(int, int*, int*, ADLCrossfireComb**);
typedef int(ADL_API* PFN_CrossfireGet)(int, ADLCrossfireComb*, ADLCrossfireInfo*);
typedef int(ADL_API* PFN_MvpuStatus)(int, ADLMVPUStatus*);
typedef int(ADL_API* PFN_Activity)(int, ADLPMActivity*);
typedef int(ADL_API* PFN_ThermalEnum)(int, int, ADLThermalControllerInfo*);
typedef int(ADL_API* PFN_Temperature)(int, int, ADLTemperature*);
typedef int(ADL_API* PFN_FanInfo)(int, int, ADLFanSpeedInfo*);
typedef int(ADL_API* PFN_FanSpeed)(int, int, ADLFanSpeedValue*);
typedef int(ADL_API* PFN_OdParameters)(int, ADLODParameters*);
typedef int(ADL_API* PFN_ProfileSearch)(const char*, const char*, const char*, const char*, ADLApplicationProfile**);

// A structure of the SDK filled with 0xcd, with iSize set as a caller sets it.
template <typename T>
T Dirty()
{
    T t;
    memset(&t, 0xcd, sizeof(t));
    t.iSize = (int)sizeof(T);
    return t;
}

// True when only iSize (the first int) is not zero.
template <typename T>
bool ClearedButSize(const T& t)
{
    return t.iSize == (int)sizeof(T) && AllZero(reinterpret_cast<const char*>(&t) + sizeof(int), sizeof(T) - sizeof(int));
}

// Each presence export: ADL_ERR_NOT_SUPPORTED with its outputs cleared and the input fields kept, the argument
// errors in the documented sequence, no callback call and no change of a context.
void CheckPresenceExports()
{
    auto lCreate = Get<PFN_LegacyCreate>("ADL_Main_Control_Create");
    auto lDestroy = Get<PFN_LegacyVoid>("ADL_Main_Control_Destroy");
    auto liveBlocks = Get<PFN_LiveBlocks>("Bc250AdlTest_LiveBlocks");
    auto adapterId = Get<PFN_AdapterId>("ADL_Adapter_ID_Get");
    auto xfCaps = Get<PFN_CrossfireCaps>("ADL_Adapter_Crossfire_Caps");
    auto xfGet = Get<PFN_CrossfireGet>("ADL_Adapter_Crossfire_Get");
    auto mvpu = Get<PFN_MvpuStatus>("ADL_Display_MVPUStatus_Get");
    auto activity = Get<PFN_Activity>("ADL_Overdrive5_CurrentActivity_Get");
    auto thermal = Get<PFN_ThermalEnum>("ADL_Overdrive5_ThermalDevices_Enum");
    auto temperature = Get<PFN_Temperature>("ADL_Overdrive5_Temperature_Get");
    auto fanInfo = Get<PFN_FanInfo>("ADL_Overdrive5_FanSpeedInfo_Get");
    auto fanSpeed = Get<PFN_FanSpeed>("ADL_Overdrive5_FanSpeed_Get");
    auto odParams = Get<PFN_OdParameters>("ADL_Overdrive5_ODParameters_Get");
    auto userLoad = Get<PFN_LegacyVoid>("ADL_ApplicationProfiles_User_Load");
    auto search = Get<PFN_ProfileSearch>("ADL_ApplicationProfiles_ProfileOfAnApplication_Search");
    if (!adapterId || !xfCaps || !xfGet || !mvpu || !activity || !thermal || !temperature || !fanInfo || !fanSpeed ||
        !odParams || !userLoad || !search) {
        return;
    }

    // No context: ADL_ERR_NOT_INIT, outputs cleared.
    int id = 77;
    CHECK(adapterId(0, &id) == ADL_ERR_NOT_INIT && id == 0);
    ADLTemperature t = Dirty<ADLTemperature>();
    CHECK(temperature(0, 0, &t) == ADL_ERR_NOT_INIT && ClearedButSize(t));
    CHECK(userLoad() == ADL_ERR_NOT_INIT);
    ADLApplicationProfile* profile = reinterpret_cast<ADLApplicationProfile*>(1);
    CHECK(search("a.exe", nullptr, nullptr, "D3D", &profile) == ADL_ERR_NOT_INIT && profile == nullptr);

    CHECK(lCreate(CountingAlloc, 1) == ADL_OK && liveBlocks() == 1);
    LONG allocations = g_allocations;

    // ADL_Adapter_ID_Get: one ID for each GPU, the same for all entries of a GPU, never 0 for an AMD GPU.
    int id0 = 0, id1 = 0, id2 = 0;
    CHECK(adapterId(0, &id0) == ADL_OK && id0 != 0);
    CHECK(adapterId(1, &id1) == ADL_OK && id1 == id0);  // entries 0 and 1 are GPU 0
    CHECK(adapterId(2, &id2) == ADL_OK && id2 != 0 && id2 != id0);  // entry 2 is GPU 1
    CHECK(id0 > 0 && id2 > 0);  // 31 bits
    id = 77;
    CHECK(adapterId(3, &id) == ADL_ERR_INVALID_ADL_IDX && id == 0);
    CHECK(adapterId(-1, &id) == ADL_ERR_INVALID_ADL_IDX && id == 0);
    CHECK(adapterId(0, nullptr) == ADL_ERR_NULL_POINTER);

    int pref = 5, num = 5;
    ADLCrossfireComb* combs = reinterpret_cast<ADLCrossfireComb*>(1);
    CHECK(xfCaps(0, &pref, &num, &combs) == ADL_ERR_NOT_SUPPORTED && pref == 0 && num == 0 && combs == nullptr);
    CHECK(xfCaps(3, &pref, &num, &combs) == ADL_ERR_INVALID_ADL_IDX);
    num = 5;
    CHECK(xfCaps(0, &pref, &num, nullptr) == ADL_ERR_NULL_POINTER && num == 0);

    ADLCrossfireComb comb;
    memset(&comb, 0x11, sizeof(comb));
    ADLCrossfireInfo xinfo;
    memset(&xinfo, 0xcd, sizeof(xinfo));
    CHECK(xfGet(0, &comb, &xinfo) == ADL_ERR_NOT_SUPPORTED && AllZero(&xinfo, sizeof(xinfo)));
    CHECK(comb.iNumLinkAdapter == 0x11111111);  // an input: not changed
    CHECK(xfGet(0, &comb, nullptr) == ADL_ERR_NULL_POINTER);
    memset(&xinfo, 0xcd, sizeof(xinfo));
    CHECK(xfGet(0, nullptr, &xinfo) == ADL_ERR_NULL_POINTER && AllZero(&xinfo, sizeof(xinfo)));

    ADLMVPUStatus ms = Dirty<ADLMVPUStatus>();
    CHECK(mvpu(0, &ms) == ADL_ERR_NOT_SUPPORTED && ClearedButSize(ms));
    ms = Dirty<ADLMVPUStatus>();
    CHECK(mvpu(9, &ms) == ADL_ERR_INVALID_ADL_IDX && ClearedButSize(ms));
    CHECK(mvpu(0, nullptr) == ADL_ERR_NULL_POINTER);

    ADLPMActivity act = Dirty<ADLPMActivity>();
    CHECK(activity(0, &act) == ADL_ERR_NOT_SUPPORTED && ClearedButSize(act));
    act = Dirty<ADLPMActivity>();
    CHECK(activity(-1, &act) == ADL_ERR_INVALID_ADL_IDX && ClearedButSize(act));
    CHECK(activity(0, nullptr) == ADL_ERR_NULL_POINTER);

    ADLThermalControllerInfo tc = Dirty<ADLThermalControllerInfo>();
    CHECK(thermal(0, 0, &tc) == ADL_ERR_NOT_SUPPORTED && ClearedButSize(tc));
    CHECK(thermal(0, 0, nullptr) == ADL_ERR_NULL_POINTER);

    t = Dirty<ADLTemperature>();
    CHECK(temperature(2, 0, &t) == ADL_ERR_NOT_SUPPORTED && ClearedButSize(t));
    t = Dirty<ADLTemperature>();
    CHECK(temperature(3, 0, &t) == ADL_ERR_INVALID_ADL_IDX && ClearedButSize(t));
    CHECK(temperature(0, 0, nullptr) == ADL_ERR_NULL_POINTER);

    ADLFanSpeedInfo fi = Dirty<ADLFanSpeedInfo>();
    CHECK(fanInfo(0, 0, &fi) == ADL_ERR_NOT_SUPPORTED && ClearedButSize(fi));
    CHECK(fanInfo(0, 0, nullptr) == ADL_ERR_NULL_POINTER);

    ADLFanSpeedValue fv = Dirty<ADLFanSpeedValue>();
    fv.iSpeedType = 1;  // ADL_DL_FANCTRL_SPEED_TYPE_PERCENT, an input
    CHECK(fanSpeed(0, 0, &fv) == ADL_ERR_NOT_SUPPORTED);
    CHECK(fv.iSize == (int)sizeof(fv) && fv.iSpeedType == 1 && fv.iFanSpeed == 0 && fv.iFlags == 0);
    CHECK(fanSpeed(0, 0, nullptr) == ADL_ERR_NULL_POINTER);

    ADLODParameters od = Dirty<ADLODParameters>();
    CHECK(odParams(0, &od) == ADL_ERR_NOT_SUPPORTED && ClearedButSize(od));
    CHECK(odParams(0, nullptr) == ADL_ERR_NULL_POINTER);

    CHECK(userLoad() == ADL_ERR_NOT_SUPPORTED);
    profile = reinterpret_cast<ADLApplicationProfile*>(1);
    CHECK(search("a.exe", nullptr, nullptr, "D3D", &profile) == ADL_ERR_NOT_SUPPORTED && profile == nullptr);
    profile = reinterpret_cast<ADLApplicationProfile*>(1);
    CHECK(search(nullptr, nullptr, nullptr, "D3D", &profile) == ADL_ERR_NULL_POINTER && profile == nullptr);
    CHECK(search("a.exe", nullptr, nullptr, "D3D", nullptr) == ADL_ERR_NULL_POINTER);

    // A caller without a context of its own: the facts functions use the newest context, the functions that
    // allocate or load in AMD's library get ADL_ERR_NOT_INIT.
    int anon = 0;
    CHECK(CallFromAnonymousCode(reinterpret_cast<void*>(adapterId), nullptr, &anon) == ADL_OK && anon == id0);
    act = Dirty<ADLPMActivity>();
    CHECK(CallFromAnonymousCode(reinterpret_cast<void*>(activity), nullptr, &act) == ADL_ERR_NOT_SUPPORTED &&
          ClearedButSize(act));
    CHECK(CallFromAnonymousCode(reinterpret_cast<void*>(userLoad)) == ADL_ERR_NOT_INIT);

    // No side effect: no callback call, no new context block.
    CHECK(g_allocations == allocations);
    CHECK(liveBlocks() == 1);

    // The ID is stable: the same after Refresh, and the same from a new context (a new snapshot).
    auto lRefresh = Get<PFN_LegacyVoid>("ADL_Main_Control_Refresh");
    CHECK(lRefresh() == ADL_OK);
    CHECK(adapterId(0, &id) == ADL_OK && id == id0);
    CHECK(lDestroy() == ADL_OK && liveBlocks() == 0);
    CHECK(lCreate(CountingAlloc, 1) == ADL_OK);
    CHECK(adapterId(2, &id) == ADL_OK && id == id2);
    CHECK(lDestroy() == ADL_OK && liveBlocks() == 0);
}

// The adapter ID against other snapshots: a GPU that is not AMD gets 0, and two GPUs with the same instance ID (a
// hash collision in the extreme) still get different IDs.
void CheckAdapterIdRules(const AdlSnapshot& base)
{
    auto setSnapshot = Get<PFN_SetSnapshot>("Bc250AdlTest_SetSnapshot");
    auto lCreate = Get<PFN_LegacyCreate>("ADL_Main_Control_Create");
    auto lDestroy = Get<PFN_LegacyVoid>("ADL_Main_Control_Destroy");
    auto adapterId = Get<PFN_AdapterId>("ADL_Adapter_ID_Get");
    if (!adapterId) {
        return;
    }
    static AdlSnapshot s;
    s = base;
    s.gpus[1].vendor_id = 0x10de;
    setSnapshot(&s);
    int id = 77;
    CHECK(lCreate(CountingAlloc, 1) == ADL_OK);
    CHECK(adapterId(2, &id) == ADL_OK && id == 0);
    CHECK(adapterId(0, &id) == ADL_OK && id != 0);
    CHECK(lDestroy() == ADL_OK);

    s = base;
    memcpy(s.gpus[1].instance_id, s.gpus[0].instance_id, sizeof(s.gpus[0].instance_id));
    setSnapshot(&s);
    int a = 0, b = 0;
    CHECK(lCreate(CountingAlloc, 1) == ADL_OK);
    CHECK(adapterId(0, &a) == ADL_OK && adapterId(2, &b) == ADL_OK && a != 0 && b != 0 && a != b);
    CHECK(lDestroy() == ADL_OK);

    // The instance ID decides, not the position: GPU 0 keeps its ID when it is the only GPU.
    s = base;
    setSnapshot(&s);
    int full = 0;
    CHECK(lCreate(CountingAlloc, 1) == ADL_OK && adapterId(0, &full) == ADL_OK);
    CHECK(lDestroy() == ADL_OK);
    s.gpu_count = 1;
    s.entry_count = 2;
    setSnapshot(&s);
    CHECK(lCreate(CountingAlloc, 1) == ADL_OK && adapterId(1, &id) == ADL_OK && id == full);
    CHECK(lDestroy() == ADL_OK);
    setSnapshot(&base);
}

// The ADL module of the 3DMark SystemInfo scanner (FMSIScan.exe 5.93, BD-351): it gets 16 addresses and uses the
// library only if all are not null. Then Create, NumberOfAdapters, AdapterInfo_Get into its own buffer (0x624 bytes
// for each entry), Adapter_ID_Get for each entry, and Destroy. It clears its copy of a value on an error.
void CheckScannerShape()
{
    static const char* const kNames[16] = {
        "ADL_Main_Control_Create",
        "ADL_Main_Control_Destroy",
        "ADL_Adapter_NumberOfAdapters_Get",
        "ADL_Adapter_AdapterInfo_Get",
        "ADL_Overdrive5_CurrentActivity_Get",
        "ADL_Overdrive5_ThermalDevices_Enum",
        "ADL_Overdrive5_Temperature_Get",
        "ADL_Overdrive5_FanSpeedInfo_Get",
        "ADL_Overdrive5_FanSpeed_Get",
        "ADL_Adapter_Crossfire_Get",
        "ADL_Adapter_Crossfire_Caps",
        "ADL_Adapter_ID_Get",
        "ADL_Display_MVPUStatus_Get",
        "ADL_Overdrive5_ODParameters_Get",
        "ADL_ApplicationProfiles_User_Load",
        "ADL_ApplicationProfiles_ProfileOfAnApplication_Search",
    };
    void* fn[16] = {};
    bool all = true;
    for (int i = 0; i < 16; i++) {
        fn[i] = reinterpret_cast<void*>(GetProcAddress(g_dll, kNames[i]));
        if (!fn[i]) {
            printf("FAIL scanner shape: no %s\n", kNames[i]);
            all = false;
        }
    }
    CHECK(all);
    if (!all) {
        return;
    }
    auto liveBlocks = Get<PFN_LiveBlocks>("Bc250AdlTest_LiveBlocks");
    CHECK(reinterpret_cast<PFN_LegacyCreate>(fn[0])(CountingAlloc, 1) == ADL_OK);
    int n = 0;
    CHECK(reinterpret_cast<PFN_LegacyCount>(fn[2])(&n) == ADL_OK && n == 3);
    static AdapterInfo infos[8];
    static_assert(sizeof(AdapterInfo) == 0x624, "AdapterInfo stride of the scanner");
    memset(infos, 0, sizeof(infos));
    CHECK(n > 0 && n <= 8 && reinterpret_cast<PFN_LegacyInfo>(fn[3])(infos, n * (int)sizeof(AdapterInfo)) == ADL_OK);
    int ids[8] = {};
    for (int i = 0; i < n && i < 8; i++) {
        if (reinterpret_cast<PFN_AdapterId>(fn[11])(infos[i].iAdapterIndex, &ids[i]) != ADL_OK) {
            ids[i] = 0;  // as the scanner does
        }
        CHECK(ids[i] != 0);
        ADLPMActivity act = Dirty<ADLPMActivity>();
        if (reinterpret_cast<PFN_Activity>(fn[4])(infos[i].iAdapterIndex, &act) != ADL_OK) {
            memset(&act, 0, sizeof(act));
        }
        ADLTemperature t = Dirty<ADLTemperature>();
        CHECK(reinterpret_cast<PFN_Temperature>(fn[6])(infos[i].iAdapterIndex, 0, &t) == ADL_ERR_NOT_SUPPORTED);
    }
    // The entries of one GPU (same bus) share an ID, the GPUs differ.
    CHECK(infos[0].iBusNumber == infos[1].iBusNumber && ids[0] == ids[1]);
    CHECK(infos[2].iBusNumber != infos[0].iBusNumber && ids[2] != ids[0]);
    CHECK(reinterpret_cast<PFN_LegacyVoid>(fn[14])() == ADL_ERR_NOT_SUPPORTED);
    ADLApplicationProfile* profile = nullptr;
    CHECK(reinterpret_cast<PFN_ProfileSearch>(fn[15])("3DMark.exe", nullptr, nullptr, "D3D", &profile) ==
              ADL_ERR_NOT_SUPPORTED &&
          profile == nullptr);
    CHECK(reinterpret_cast<PFN_LegacyVoid>(fn[1])() == ADL_OK && liveBlocks() == 0);
}

int RunUnit()
{
    auto create = Get<PFN_Create>("ADL2_Main_Control_Create");
    auto createX3 = Get<PFN_CreateX3>("ADL2_Main_ControlX3_Create");
    auto destroy = Get<PFN_Destroy>("ADL2_Main_Control_Destroy");
    auto refresh = Get<PFN_Refresh>("ADL2_Main_Control_Refresh");
    auto count = Get<PFN_Count>("ADL2_Adapter_NumberOfAdapters_Get");
    auto infoX2 = Get<PFN_InfoX2>("ADL2_Adapter_AdapterInfoX2_Get");
    auto info = Get<PFN_Info>("ADL2_Adapter_AdapterInfo_Get");
    auto active = Get<PFN_Active>("ADL2_Adapter_Active_Get");
    auto memory = Get<PFN_Memory>("ADL2_Adapter_MemoryInfo_Get");
    auto versions = Get<PFN_Versions>("ADL2_Graphics_Versions_Get");
    auto versionsX2 = Get<PFN_VersionsX2>("ADL2_Graphics_VersionsX2_Get");
    auto versionsX3 = Get<PFN_VersionsX3>("ADL2_Graphics_VersionsX3_Get");
    auto displayInfo = Get<PFN_DisplayInfo>("ADL2_Display_DisplayInfo_Get");
    auto modes = Get<PFN_Modes>("ADL2_Display_Modes_Get");
    auto mapConfig = Get<PFN_MapConfig>("ADL2_Display_DisplayMapConfig_Get");
    auto slsIndex = Get<PFN_SlsIndex>("ADL2_Display_SLSMapIndex_Get");
    auto slsConfig = Get<PFN_SlsConfig>("ADL2_Display_SLSMapConfig_Get");
    auto lCreate = Get<PFN_LegacyCreate>("ADL_Main_Control_Create");
    auto lDestroy = Get<PFN_LegacyVoid>("ADL_Main_Control_Destroy");
    auto lRefresh = Get<PFN_LegacyVoid>("ADL_Main_Control_Refresh");
    auto lCount = Get<PFN_LegacyCount>("ADL_Adapter_NumberOfAdapters_Get");
    auto lInfoX2 = Get<PFN_LegacyInfoX2>("ADL_Adapter_AdapterInfoX2_Get");
    auto lInfo = Get<PFN_LegacyInfo>("ADL_Adapter_AdapterInfo_Get");
    auto lActive = Get<PFN_LegacyActive>("ADL_Adapter_Active_Get");
    auto lMemory = Get<PFN_LegacyMemory>("ADL_Adapter_MemoryInfo_Get");
    auto lVersions = Get<PFN_LegacyVersions>("ADL_Graphics_Versions_Get");
    auto lDisplayInfo = Get<PFN_LegacyDisplayInfo>("ADL_Display_DisplayInfo_Get");
    auto lModes = Get<PFN_LegacyModes>("ADL_Display_Modes_Get");
    auto lMapConfig = Get<PFN_LegacyMapConfig>("ADL_Display_DisplayMapConfig_Get");
    auto lSlsIndex = Get<PFN_LegacySlsIndex>("ADL_Display_SLSMapIndex_Get");
    auto lSlsConfig = Get<PFN_LegacySlsConfig>("ADL_Display_SLSMapConfig_Get");
    auto setSnapshot = Get<PFN_SetSnapshot>("Bc250AdlTest_SetSnapshot");
    auto liveBlocks = Get<PFN_LiveBlocks>("Bc250AdlTest_LiveBlocks");
    if (g_failures) {
        return 1;
    }

    static AdlSnapshot snap;
    MakeSnapshot(&snap);
    setSnapshot(&snap);
    CheckReentryBounded();

    // Create: argument checks.
    ADL_CONTEXT_HANDLE ctx = (ADL_CONTEXT_HANDLE)1;
    CHECK(create(CountingAlloc, 1, nullptr) == ADL_ERR_NULL_POINTER);
    CHECK(create(nullptr, 1, &ctx) == ADL_ERR_INVALID_CALLBACK && ctx == nullptr);
    CHECK(create(CountingAlloc, 1, &ctx) == ADL_OK && ctx != nullptr);
    CHECK(liveBlocks() == 1);

    int n = -1;
    CHECK(count(ctx, &n) == ADL_OK && n == 3);
    CHECK(count(ctx, nullptr) == ADL_ERR_NULL_POINTER);

    // AdapterInfoX2: caller memory, the AGS stride, the fields AGS 6.3 reads.
    AdapterInfo* ai = (AdapterInfo*)1;
    LONG before = g_outstanding;
    CHECK(infoX2(ctx, &ai) == ADL_OK && ai != nullptr);
    CHECK(g_outstanding == before + 1);
    if (ai) {
        CHECK(ai[0].iSize == (int)sizeof(AdapterInfo) && ai[0].iAdapterIndex == 0 && ai[2].iAdapterIndex == 2);
        CHECK(strcmp(ai[0].strUDID, "PCI_VEN_1002&DEV_13FE&SUBSYS_00011002&REV_00_4&1A2B3C4D&0&0008") == 0);
        CHECK(strstr(ai[2].strUDID, "VEN_1002") && strstr(ai[2].strUDID, "DEV_73BF") && strstr(ai[2].strUDID, "REV_C1"));
        CHECK(ai[0].iBusNumber == 3 && ai[1].iBusNumber == 3 && ai[2].iBusNumber == 10);
        CHECK(ai[0].iDeviceNumber == 0 && ai[0].iFunctionNumber == 0);
        CHECK(ai[0].iVendorID == 1002 && ai[2].iVendorID == 1002);
        CHECK(strcmp(ai[0].strAdapterName, "Test GPU zero") == 0 && strcmp(ai[2].strAdapterName, "Test GPU one") == 0);
        CHECK(strcmp(ai[0].strDisplayName, "\\\\.\\DISPLAY1") == 0 && strcmp(ai[1].strDisplayName, "\\\\.\\DISPLAY2") == 0);
        CHECK(ai[2].strDisplayName[0] == '\0');
        CHECK(ai[0].iPresent == 1 && ai[2].iPresent == 0 && ai[0].iExist == 1 && ai[2].iExist == 1);
        CHECK(strcmp(ai[0].strDriverPath, "\\Registry\\Machine\\System\\CurrentControlSet\\Control\\Class\\"
                                          "{4d36e968-e325-11ce-bfc1-08002be10318}\\0001") == 0);
        CHECK(ai[2].strDriverPath[0] == '\0' && ai[0].strDriverPathExt[0] == '\0');
        CHECK(strcmp(ai[0].strPNPString, snap.gpus[0].instance_id) == 0);
        CHECK(ai[0].iOSDisplayIndex == 0 && ai[1].iOSDisplayIndex == 1 && ai[2].iOSDisplayIndex == -1);
        // The bytes AGS 6.3 reads at its own offsets: iExist at 0x31c, the display name at 0x218.
        CHECK(*(const int*)((const char*)&ai[1] + 0x31c) == 1);
        CHECK(strcmp((const char*)&ai[1] + 0x218, "\\\\.\\DISPLAY2") == 0);
        CallerFree(ai);
    }
    CHECK(infoX2(ctx, nullptr) == ADL_ERR_NULL_POINTER);

    // AdapterInfo into a caller buffer.
    AdapterInfo buf[3];
    CHECK(info(ctx, buf, (int)sizeof(AdapterInfo) * 2) == ADL_ERR_INVALID_PARAM_SIZE);
    CHECK(info(ctx, buf, (int)sizeof(buf)) == ADL_OK && buf[1].iAdapterIndex == 1 && buf[2].iBusNumber == 10);
    CHECK(info(ctx, nullptr, 0) == ADL_ERR_NULL_POINTER);

    int status = -1;
    CHECK(active(ctx, 0, &status) == ADL_OK && status == 1);
    CHECK(active(ctx, 2, &status) == ADL_OK && status == 0);
    status = 7;
    CHECK(active(ctx, 3, &status) == ADL_ERR_INVALID_ADL_IDX && status == 0);

    ADLMemoryInfo mem;
    memset(&mem, 0xcd, sizeof(mem));
    CHECK(memory(ctx, 1, &mem) == ADL_OK && mem.iMemorySize == (long long)(8ull << 30));
    CHECK(mem.strMemoryType[0] == '\0' && mem.iMemoryBandwidth == 0);
    CHECK(memory(ctx, 2, &mem) == ADL_OK && mem.iMemorySize == (long long)(16ull << 30));
    memset(&mem, 0xcd, sizeof(mem));
    CHECK(memory(ctx, -1, &mem) == ADL_ERR_INVALID_ADL_IDX && AllZero(&mem, sizeof(mem)));

    // Versions: our driver version, no Catalyst or Radeon Software release.
    ADLVersionsInfo v;
    ADLVersionsInfoX2 v2;
    memset(&v, 0xcd, sizeof(v));
    CHECK(versions(ctx, &v) == ADL_OK && strcmp(v.strDriverVer, "0.7.216.104") == 0);
    CHECK(v.strCatalystVersion[0] == '\0' && v.strCatalystWebLink[0] == '\0');
    memset(&v2, 0xcd, sizeof(v2));
    CHECK(versionsX2(ctx, &v2) == ADL_OK && strcmp(v2.strDriverVer, "0.7.216.104") == 0);
    CHECK(v2.strCatalystVersion[0] == '\0' && v2.strCrimsonVersion[0] == '\0' && v2.strCatalystWebLink[0] == '\0');
    memset(&v2, 0xcd, sizeof(v2));
    CHECK(versionsX3(ctx, 2, &v2) == ADL_OK && strcmp(v2.strDriverVer, "0.7.216.105") == 0 && v2.strCrimsonVersion[0] == '\0');
    memset(&v2, 0xcd, sizeof(v2));
    CHECK(versionsX3(ctx, 9, &v2) == ADL_ERR_INVALID_ADL_IDX && AllZero(&v2, sizeof(v2)));
    CHECK(versionsX3(ctx, -1, &v2) == ADL_ERR_INVALID_ADL_IDX);

    // DisplayInfo: the displays of the GPU of the adapter index; -1 for all.
    ADLDisplayInfo* di = nullptr;
    n = -1;
    CHECK(displayInfo(ctx, 1, &n, &di, 0) == ADL_OK && n == 3 && di != nullptr);
    if (di) {
        CHECK((di[0].iDisplayInfoValue & 1) && (di[1].iDisplayInfoValue & 1) && (di[2].iDisplayInfoValue & 1));
        CHECK(di[0].displayID.iDisplayLogicalAdapterIndex == 0 && di[1].displayID.iDisplayLogicalAdapterIndex == 1);
        CHECK(di[2].displayID.iDisplayLogicalIndex == 2 && di[2].displayID.iDisplayLogicalAdapterIndex == 0);
        CHECK(strcmp(di[0].strDisplayName, "Monitor A") == 0 && strcmp(di[0].strDisplayManufacturerName, "AAA") == 0);
        CHECK(di[1].iDisplayConnector == ADL_DISPLAY_CONTYPE_HDMI_TYPE_A && di[1].iDisplayControllerIndex == 1);
        CHECK(*(const int*)((const char*)&di[1] + 0x224) & 1);  // the bit AGS 6.3 tests, at its offset
        CallerFree(di);
    }
    di = (ADLDisplayInfo*)1;
    n = -1;
    CHECK(displayInfo(ctx, 2, &n, &di, 0) == ADL_OK && n == 0 && di == nullptr);
    CHECK(displayInfo(ctx, -1, &n, &di, 1) == ADL_OK && n == 3);
    CallerFree(di);
    di = (ADLDisplayInfo*)1;
    CHECK(displayInfo(ctx, 5, &n, &di, 0) == ADL_ERR_INVALID_ADL_IDX && n == 0 && di == nullptr);

    // Modes: current mode of each display.
    ADLMode* m = nullptr;
    CHECK(modes(ctx, 0, -1, &n, &m) == ADL_OK && n == 2);
    if (m) {
        CHECK(m[0].iXRes == 2560 && m[0].iYRes == 1440 && m[0].iColourDepth == 32 && m[0].displayID.iDisplayLogicalIndex == 0);
        CHECK(m[1].displayID.iDisplayLogicalIndex == 2 && m[0].iAdapterIndex == 0);
        CallerFree(m);
    }
    CHECK(modes(ctx, 1, 1, &n, &m) == ADL_OK && n == 1);
    if (m) {
        CHECK(m[0].iXPos == 2560 && m[0].iOrientation == 90 && m[0].fRefreshRate == 60.0f && m[0].iAdapterIndex == 1);
        CallerFree(m);
    }
    m = (ADLMode*)1;
    CHECK(modes(ctx, 0, 7, &n, &m) == ADL_ERR_INVALID_DIPLAY_IDX && n == 0 && m == nullptr);
    CHECK(modes(ctx, 2, -1, &n, &m) == ADL_OK && n == 0 && m == nullptr);

    // DisplayMapConfig: one map per source with displays, a clone pair is one map with two targets.
    ADLDisplayMap* maps = nullptr;
    ADLDisplayTarget* targets = nullptr;
    int nmaps = -1, ntargets = -1;
    CHECK(mapConfig(ctx, 0, &nmaps, &maps, &ntargets, &targets, 1) == ADL_OK && nmaps == 2 && ntargets == 3);
    if (maps && targets) {
        CHECK(maps[0].iNumDisplayTarget == 2 && maps[0].iFirstDisplayTargetArrayIndex == 0);
        CHECK(maps[1].iNumDisplayTarget == 1 && maps[1].iFirstDisplayTargetArrayIndex == 2);
        CHECK(maps[1].displayMode.iOrientation == 90 && targets[2].iDisplayMapIndex == 1);
        CHECK(targets[1].displayID.iDisplayLogicalIndex == 2);
    }
    CallerFree(maps);
    CallerFree(targets);
    maps = (ADLDisplayMap*)1;
    targets = (ADLDisplayTarget*)1;
    CHECK(mapConfig(ctx, 2, &nmaps, &maps, &ntargets, &targets, 0) == ADL_OK && nmaps == 0 && ntargets == 0 &&
          maps == nullptr && targets == nullptr);

    // No SLS group: the index is -1 and the configuration call clears everything.
    int sls = 5;
    ADLDisplayTarget t2[2] = {};
    CHECK(slsIndex(ctx, 0, 2, t2, &sls) == ADL_ERR_NOT_SUPPORTED && sls == -1);
    ADLSLSMap smap;
    memset(&smap, 0xcd, sizeof(smap));
    int c1 = 9, c2 = 9, c3 = 9, c4 = 9, c5 = 9;
    ADLSLSTarget* p1 = (ADLSLSTarget*)1;
    ADLSLSMode* p2 = (ADLSLSMode*)1;
    ADLBezelTransientMode* p3 = (ADLBezelTransientMode*)1;
    ADLBezelTransientMode* p4 = (ADLBezelTransientMode*)1;
    ADLSLSOffset* p5 = (ADLSLSOffset*)1;
    CHECK(slsConfig(ctx, 0, 0, &smap, &c1, &p1, &c2, &p2, &c3, &p3, &c4, &p4, &c5, &p5, 0) == ADL_ERR_NOT_SUPPORTED);
    CHECK(AllZero(&smap, sizeof(smap)) && !c1 && !c2 && !c3 && !c4 && !c5 && !p1 && !p2 && !p3 && !p4 && !p5);

    // A failing callback: the call fails, nothing is returned.
    ADL_CONTEXT_HANDLE starving = nullptr;
    CHECK(createX3(FailingAlloc, 1, &starving, ADL_THREADING_LOCKED, 0) == ADL_OK);
    ai = (AdapterInfo*)1;
    CHECK(infoX2(starving, &ai) == ADL_ERR && ai == nullptr);
    CHECK(destroy(starving) == ADL_OK);

    // ADL supplies no deallocator: if the second callback allocation fails, the first remains caller-owned.
    g_failure_call = 0;
    CHECK(create(FailSecondAlloc, 1, &starving) == ADL_OK);
    maps = nullptr;
    targets = nullptr;
    CHECK(mapConfig(starving, 0, &nmaps, &maps, &ntargets, &targets, 0) == ADL_ERR);
    CHECK(nmaps == 2 && maps && ntargets == 0 && !targets && g_failure_call == 2);
    CallerFree(maps);
    CHECK(destroy(starving) == ADL_OK);

    // Refresh takes a new snapshot.
    static AdlSnapshot one;
    MakeSnapshot(&one);
    one.gpu_count = 1;
    one.entry_count = 2;
    setSnapshot(&one);
    CHECK(refresh(ctx) == ADL_OK && count(ctx, &n) == ADL_OK && n == 2);
    CHECK(refresh(nullptr) == ADL_ERR_INVALID_PARAM);
    setSnapshot(&snap);

    // Destroy: the handle is gone, calls on it fail without touching it.
    CHECK(destroy(ctx) == ADL_OK);
    CHECK(destroy(ctx) == ADL_ERR_NOT_INIT);
    CHECK(destroy(nullptr) == ADL_ERR_INVALID_PARAM);
    n = 5;
    CHECK(count(ctx, &n) == ADL_ERR_NOT_INIT && n == 0);
    CHECK(liveBlocks() == 0);

    // Repeated Create/Destroy, both entry points, interleaved.
    for (int i = 0; i < 1000; i++) {
        ADL_CONTEXT_HANDLE a = nullptr, b = nullptr;
        CHECK(create(CountingAlloc, 1, &a) == ADL_OK);
        CHECK(createX3(CountingAlloc, 0, &b, ADL_THREADING_UNLOCKED, 1) == ADL_OK);
        AdapterInfo* x = nullptr;
        if (infoX2(b, &x) == ADL_OK) {
            CallerFree(x);
        }
        CHECK(destroy(a) == ADL_OK && destroy(b) == ADL_OK);
        if (g_failures) {
            break;
        }
    }
    CHECK(liveBlocks() == 0);
    CHECK(g_outstanding == 0);

    // The legacy context: none before Create, counted Create/Destroy.
    n = 7;
    CHECK(lCount(&n) == ADL_ERR_NOT_INIT && n == 0);
    CHECK(lDestroy() == ADL_ERR_NOT_INIT);
    CHECK(lCreate(nullptr, 1) == ADL_ERR_INVALID_CALLBACK);
    CHECK(lCreate(CountingAlloc, 1) == ADL_OK && lCreate(CountingAlloc, 1) == ADL_OK && liveBlocks() == 1);
    CHECK(lCount(&n) == ADL_OK && n == 3);
    // ADL2 calls with a null handle use the legacy context.
    CHECK(count(nullptr, &n) == ADL_OK && n == 3);
    ai = nullptr;
    CHECK(lInfoX2(&ai) == ADL_OK && ai && ai[1].iAdapterIndex == 1);
    CallerFree(ai);
    CHECK(lInfo(buf, (int)sizeof(buf)) == ADL_OK && buf[0].iVendorID == 1002);
    CHECK(lActive(1, &status) == ADL_OK && status == 1);
    CHECK(lMemory(2, &mem) == ADL_OK && mem.iMemorySize == (long long)(16ull << 30));
    CHECK(lVersions(&v) == ADL_OK && strcmp(v.strDriverVer, "0.7.216.104") == 0);
    di = nullptr;
    CHECK(lDisplayInfo(0, &n, &di, 0) == ADL_OK && n == 3);
    CallerFree(di);
    m = nullptr;
    CHECK(lModes(1, -1, &n, &m) == ADL_OK && n == 1);
    CallerFree(m);
    maps = nullptr;
    targets = nullptr;
    CHECK(lMapConfig(0, &nmaps, &maps, &ntargets, &targets, 0) == ADL_OK && nmaps == 2);
    CallerFree(maps);
    CallerFree(targets);
    CHECK(lSlsIndex(0, 0, nullptr, &sls) == ADL_ERR_NOT_SUPPORTED && sls == -1);
    p1 = (ADLSLSTarget*)1;
    CHECK(lSlsConfig(0, 0, nullptr, &c1, &p1, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                     0) == ADL_ERR_NOT_SUPPORTED && p1 == nullptr);
    CHECK(lRefresh() == ADL_OK);
    CHECK(lDestroy() == ADL_OK && liveBlocks() == 1);
    CHECK(lDestroy() == ADL_OK && liveBlocks() == 0);
    CHECK(lDestroy() == ADL_ERR_NOT_INIT);

    CheckTwoCallerModules();
    CheckPresenceExports();
    CheckAdapterIdRules(snap);
    CheckScannerShape();
    CHECK(liveBlocks() == 0);

    // No adapter of ours: an empty, truthful answer.
    static AdlSnapshot empty;
    memset(&empty, 0, sizeof(empty));
    setSnapshot(&empty);
    CHECK(create(CountingAlloc, 1, &ctx) == ADL_OK);
    CHECK(count(ctx, &n) == ADL_OK && n == 0);
    ai = (AdapterInfo*)1;
    CHECK(infoX2(ctx, &ai) == ADL_OK && ai == nullptr);
    memset(&v, 0xcd, sizeof(v));
    CHECK(versions(ctx, &v) == ADL_ERR_NOT_SUPPORTED && AllZero(&v, sizeof(v)));
    memset(&v2, 0xcd, sizeof(v2));
    CHECK(versionsX2(ctx, &v2) == ADL_ERR_NOT_SUPPORTED && AllZero(&v2, sizeof(v2)));
    CHECK(displayInfo(ctx, -1, &n, &di, 0) == ADL_OK && n == 0 && di == nullptr);
    CHECK(destroy(ctx) == ADL_OK);
    setSnapshot(nullptr);

    CHECK(g_outstanding == 0);
    CHECK(liveBlocks() == 0);

#if defined(_M_IX86)
    // Every export leaves its arguments to the caller (cdecl). A live context makes the calls do their work.
    setSnapshot(&snap);
    ADL_CONTEXT_HANDLE live = nullptr;
    create(CountingAlloc, 1, &live);
    static AdapterInfo abuf[3];
    static int iout[8];
    static void* pout[8];
    static ADLMemoryInfo mout;
    static ADLVersionsInfoX2 vout;
    static ADLSLSMap sout;
    static ADLCrossfireComb xcomb;
    static ADLCrossfireInfo xinfo;
    static ADLMVPUStatus mvout;
    static ADLPMActivity actout;
    static ADLThermalControllerInfo tcout;
    static ADLTemperature tout;
    static ADLFanSpeedInfo fiout;
    static ADLFanSpeedValue fvout;
    static ADLODParameters odout;
    int h = (int)(intptr_t)live;
    const struct {
        const char* name;
        int args[15];
        int count;
    } calls[] = {
        {"ADL2_Adapter_NumberOfAdapters_Get", {h, (int)(intptr_t)&iout[0]}, 2},
        {"ADL2_Adapter_AdapterInfo_Get", {h, (int)(intptr_t)abuf, (int)sizeof(abuf)}, 3},
        {"ADL2_Adapter_Active_Get", {h, 0, (int)(intptr_t)&iout[0]}, 3},
        {"ADL2_Adapter_MemoryInfo_Get", {h, 0, (int)(intptr_t)&mout}, 3},
        {"ADL2_Graphics_Versions_Get", {h, (int)(intptr_t)&vout}, 2},
        {"ADL2_Graphics_VersionsX2_Get", {h, (int)(intptr_t)&vout}, 2},
        {"ADL2_Graphics_VersionsX3_Get", {h, 0, (int)(intptr_t)&vout}, 3},
        {"ADL2_Display_SLSMapIndex_Get", {h, 0, 0, 0, (int)(intptr_t)&iout[0]}, 5},
        {"ADL2_Display_SLSMapConfig_Get", {h, 0, 0, (int)(intptr_t)&sout, (int)(intptr_t)&iout[0], (int)(intptr_t)&pout[0],
                                           (int)(intptr_t)&iout[1], (int)(intptr_t)&pout[1], (int)(intptr_t)&iout[2],
                                           (int)(intptr_t)&pout[2], (int)(intptr_t)&iout[3], (int)(intptr_t)&pout[3],
                                           (int)(intptr_t)&iout[4], (int)(intptr_t)&pout[4], 0}, 15},
        {"ADL2_Main_Control_Refresh", {h}, 1},
        {"ADL_Display_SLSMapIndex_Get", {0, 0, 0, (int)(intptr_t)&iout[0]}, 4},
        {"ADL_Adapter_NumberOfAdapters_Get", {(int)(intptr_t)&iout[0]}, 1},
        {"ADL_Adapter_Active_Get", {0, (int)(intptr_t)&iout[0]}, 2},
        {"ADL_Adapter_MemoryInfo_Get", {0, (int)(intptr_t)&mout}, 2},
        {"ADL_Graphics_Versions_Get", {(int)(intptr_t)&vout}, 1},
        {"ADL_Adapter_AdapterInfo_Get", {(int)(intptr_t)abuf, (int)sizeof(abuf)}, 2},
        {"ADL_Main_Control_Refresh", {}, 0},
        // The presence exports, with the argument count of the SDK prototype.
        {"ADL_Adapter_ID_Get", {0, (int)(intptr_t)&iout[0]}, 2},
        {"ADL_Adapter_Crossfire_Caps", {0, (int)(intptr_t)&iout[0], (int)(intptr_t)&iout[1], (int)(intptr_t)&pout[0]}, 4},
        {"ADL_Adapter_Crossfire_Get", {0, (int)(intptr_t)&xcomb, (int)(intptr_t)&xinfo}, 3},
        {"ADL_Display_MVPUStatus_Get", {0, (int)(intptr_t)&mvout}, 2},
        {"ADL_Overdrive5_CurrentActivity_Get", {0, (int)(intptr_t)&actout}, 2},
        {"ADL_Overdrive5_ThermalDevices_Enum", {0, 0, (int)(intptr_t)&tcout}, 3},
        {"ADL_Overdrive5_Temperature_Get", {0, 0, (int)(intptr_t)&tout}, 3},
        {"ADL_Overdrive5_FanSpeedInfo_Get", {0, 0, (int)(intptr_t)&fiout}, 3},
        {"ADL_Overdrive5_FanSpeed_Get", {0, 0, (int)(intptr_t)&fvout}, 3},
        {"ADL_Overdrive5_ODParameters_Get", {0, (int)(intptr_t)&odout}, 2},
        {"ADL_ApplicationProfiles_User_Load", {}, 0},
        {"ADL_ApplicationProfiles_ProfileOfAnApplication_Search",
         {(int)(intptr_t)"a.exe", 0, 0, (int)(intptr_t)"D3D", (int)(intptr_t)&pout[1]}, 5},
        {"ADL_Main_Control_Destroy", {}, 0},
    };
    for (const auto& c : calls) {
        CheckCdecl(c.name, c.args, c.count);
    }
    // The calls that allocate: their arrays go back to the counting allocator.
    {
        int a1[] = {h, 0, (int)(intptr_t)&iout[0], (int)(intptr_t)&pout[0], 0};
        CheckCdecl("ADL2_Display_DisplayInfo_Get", a1, 5);
        CallerFree(pout[0]);
        int a2[] = {h, 0, -1, (int)(intptr_t)&iout[0], (int)(intptr_t)&pout[0]};
        CheckCdecl("ADL2_Display_Modes_Get", a2, 5);
        CallerFree(pout[0]);
        int a3[] = {h, 0, (int)(intptr_t)&iout[0], (int)(intptr_t)&pout[0], (int)(intptr_t)&iout[1],
                    (int)(intptr_t)&pout[1], 0};
        CheckCdecl("ADL2_Display_DisplayMapConfig_Get", a3, 7);
        CallerFree(pout[0]);
        CallerFree(pout[1]);
        int a4[] = {h, (int)(intptr_t)&pout[0]};
        CheckCdecl("ADL2_Adapter_AdapterInfoX2_Get", a4, 2);
        CallerFree(pout[0]);
        int a5[] = {(int)(intptr_t)CountingAlloc, 1};
        CheckCdecl("ADL_Main_Control_Create", a5, 2);
        int a6[] = {(int)(intptr_t)&pout[0]};
        CheckCdecl("ADL_Adapter_AdapterInfoX2_Get", a6, 1);
        CallerFree(pout[0]);
        int a7[] = {0, (int)(intptr_t)&iout[0], (int)(intptr_t)&pout[0], 0};
        CheckCdecl("ADL_Display_DisplayInfo_Get", a7, 4);
        CallerFree(pout[0]);
        int a8[] = {0, -1, (int)(intptr_t)&iout[0], (int)(intptr_t)&pout[0]};
        CheckCdecl("ADL_Display_Modes_Get", a8, 4);
        CallerFree(pout[0]);
        int a9[] = {0, (int)(intptr_t)&iout[0], (int)(intptr_t)&pout[0], (int)(intptr_t)&iout[1],
                    (int)(intptr_t)&pout[1], 0};
        CheckCdecl("ADL_Display_DisplayMapConfig_Get", a9, 6);
        CallerFree(pout[0]);
        CallerFree(pout[1]);
        int a10[] = {0, 0, (int)(intptr_t)&sout, (int)(intptr_t)&iout[0], (int)(intptr_t)&pout[0],
                     (int)(intptr_t)&iout[1], (int)(intptr_t)&pout[1], (int)(intptr_t)&iout[2],
                     (int)(intptr_t)&pout[2], (int)(intptr_t)&iout[3], (int)(intptr_t)&pout[3],
                     (int)(intptr_t)&iout[4], (int)(intptr_t)&pout[4], 0};
        CheckCdecl("ADL_Display_SLSMapConfig_Get", a10, 14);
        CheckCdecl("ADL_Main_Control_Destroy", nullptr, 0);
        ADL_CONTEXT_HANDLE fresh = nullptr;
        int a11[] = {(int)(intptr_t)CountingAlloc, 1, (int)(intptr_t)&fresh};
        CheckCdecl("ADL2_Main_Control_Create", a11, 3);
        int a12[] = {(int)(intptr_t)fresh};
        CheckCdecl("ADL2_Main_Control_Destroy", a12, 1);
        fresh = nullptr;
        int a13[] = {(int)(intptr_t)CountingAlloc, 1, (int)(intptr_t)&fresh, 0, 0};
        CheckCdecl("ADL2_Main_ControlX3_Create", a13, 5);
        int a14[] = {(int)(intptr_t)fresh};
        CheckCdecl("ADL2_Main_Control_Destroy", a14, 1);
    }
    destroy(live);
    setSnapshot(nullptr);
    CHECK(liveBlocks() == 0);
    CHECK(g_outstanding == 0);
    printf("x86 calling convention: every export measured as cdecl\n");
#endif

    printf("unit: %d checks, %d failed, %ld caller allocations, %ld outstanding\n", g_checks, g_failures,
           g_allocations, g_outstanding);
    return g_failures ? 1 : 0;
}

int RunDiscover()
{
    auto discover = Get<PFN_Discover>("Bc250AdlTest_Discover");
    if (!discover) {
        return 1;
    }
    static AdlSnapshot s;
    discover(&s);
    printf("discover: %d gpu(s), %d adapter entr%s, %d display(s)\n", s.gpu_count, s.entry_count,
           s.entry_count == 1 ? "y" : "ies", s.display_count);
    for (int i = 0; i < s.gpu_count; i++) {
        const AdlGpu* g = &s.gpus[i];
        printf("gpu %d: %04X:%04X subsys %08X rev %02X at %u:%u.%u luid %08X:%08X\n", i, g->vendor_id, g->device_id,
               g->subsys_id, g->revision_id, g->bus, g->device, g->function, (unsigned)g->luid_high, g->luid_low);
        printf("  name \"%s\" driver \"%s\" key \"%s\"\n", g->name, g->driver_version, g->driver_key);
        printf("  instance \"%s\"\n", g->instance_id);
        printf("  memory dedicated video %llu, dedicated system %llu, shared system %llu bytes\n",
               (unsigned long long)g->dedicated_video_bytes, (unsigned long long)g->dedicated_system_bytes,
               (unsigned long long)g->shared_system_bytes);
    }
    for (int i = 0; i < s.entry_count; i++) {
        const AdlEntry* e = &s.entries[i];
        printf("entry %d: gpu %d \"%s\" present %d os index %d source %d\n", i, e->gpu, e->gdi_name, e->present,
               e->os_display_index, e->source_id);
    }
    for (int i = 0; i < s.display_count; i++) {
        const AdlDisplay* d = &s.displays[i];
        printf("display %d: gpu %d entry %d index %d target %d \"%s\" maker \"%s\" type %d output %d connector %d "
               "%dx%d at %d,%d %.3f Hz rotation %d\n",
               i, d->gpu, d->entry, d->index, d->target_id, d->name, d->manufacturer, d->display_type, d->output_type,
               d->connector, d->width, d->height, d->x, d->y, d->refresh_hz, d->orientation);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        printf("usage: adl-unit-test <test atiadl dll> [unit|discover] [<adl-legacy-client.dll>]\n"
               "       (adl-legacy-client-b.dll must be next to adl-legacy-client.dll)\n");
        return 2;
    }
    g_dll = LoadLibraryA(argv[1]);
    if (!g_dll) {
        printf("FAIL cannot load %s (error %lu)\n", argv[1], GetLastError());
        return 1;
    }
    const char* mode = argc > 2 ? argv[2] : "unit";
    static char client[MAX_PATH];
    if (argc > 3) {
        g_client_path = argv[3];
    } else {
        // By default the second caller module is next to this program.
        DWORD len = GetModuleFileNameA(nullptr, client, MAX_PATH);
        char* slash = len && len < MAX_PATH ? strrchr(client, '\\') : nullptr;
        size_t used = slash ? (size_t)(slash + 1 - client) : 0;
        if (slash && used + sizeof("adl-legacy-client.dll") <= sizeof(client)) {
            strcpy_s(slash + 1, sizeof(client) - used, "adl-legacy-client.dll");
        }
        g_client_path = client;
    }
    // The larger variant is next to the second caller module: <name>-b.dll.
    static char client_b[MAX_PATH];
    size_t length = strlen(g_client_path);
    if (length > 4 && length + 2 < sizeof(client_b) && _stricmp(g_client_path + length - 4, ".dll") == 0) {
        memcpy(client_b, g_client_path, length - 4);
        strcpy_s(client_b + length - 4, sizeof(client_b) - (length - 4), "-b.dll");
    }
    g_client_b_path = client_b;
    int rc = strcmp(mode, "discover") == 0 ? RunDiscover() : RunUnit();
    FreeLibrary(g_dll);
    printf("%s %s\n", rc == 0 ? "PASS" : "FAIL", mode);
    return rc;
}
