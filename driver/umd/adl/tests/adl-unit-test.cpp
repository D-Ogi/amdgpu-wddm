// Host test of the open ADL subset: every export of the test build (atiadl-test.def) against an injected snapshot,
// and the discovery against the host's real adapters. No GPU work.
//
//   adl-unit-test.exe <test atiadlxx.dll or atiadlxy.dll> [unit|discover]
//
// unit      the export contract: return codes, cleared outputs, allocation through the caller's callback, the
//           AGS 6.3 strides, context ownership (1000 Create/Destroy cycles, no live block left), the legacy
//           context, Refresh, and on x86 that every export leaves its arguments to the caller (cdecl).
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

HMODULE g_dll;

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
        printf("usage: adl-unit-test <test atiadl dll> [unit|discover]\n");
        return 2;
    }
    g_dll = LoadLibraryA(argv[1]);
    if (!g_dll) {
        printf("FAIL cannot load %s (error %lu)\n", argv[1], GetLastError());
        return 1;
    }
    const char* mode = argc > 2 ? argv[2] : "unit";
    int rc = strcmp(mode, "discover") == 0 ? RunDiscover() : RunUnit();
    FreeLibrary(g_dll);
    printf("%s %s\n", rc == 0 ? "PASS" : "FAIL", mode);
    return rc;
}
