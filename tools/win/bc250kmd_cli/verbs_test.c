// Host test of the dispatch of the raw diagnostic verbs (BD-167, review r1 R4). Every verb from `read` to `fbdump`
// runs with each argument shape that its handler accepts. In a lab build (AMDGPU_WDDM_LAB_FLAVOUR) each command line
// reaches its handler: the tool opens the one adapter and sends the escape command of that handler. In a release
// build the same command line, in any letter case, is an unknown command: exit 2, and no adapter list, no adapter
// open and no escape.
//
// bc250kmd_cli.c is compiled into this file as log_test.c does it: SetupAPI and the D3DKMT calls renamed to the fakes
// below, printf and fprintf to buffers. The fake adapter is the BC-250 of log_test.c. The fake escape records the head
// of the request (Magic, Command) and fails with STATUS_NOT_SUPPORTED, so every handler stops at its first escape and
// nothing that a reply starts (more reads, a BMP file) runs. No real adapter is opened and no escape reaches a driver:
// these command lines never run against the PC that builds. `info` is the control in both flavours: a release verb
// that sends its escape through the same fakes, so that "no escape" in a release build is not a fake that sees
// nothing. test-verbs.ps1 builds and runs this file in the flavour of the build that calls it, and
// test_lab_verbs.py holds the verbs below to the CLI's own dispatch and to test-flavour.ps1.
//
//   pwsh tools\win\bc250kmd_cli\test-verbs.ps1 -Kits P:\BC-250\toolchain\nuget -Out <dir> [-Arch x86] [-Lab]

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>

static char g_out[1 << 16], g_err[1 << 16];     // what the tool printed to stdout and to stderr in the current case
static size_t g_outLen, g_errLen;

static int Append(char *buffer, size_t size, size_t *length, const char *format, va_list args)
{
    int n = vsnprintf(buffer + *length, size - *length, format, args);

    if (n > 0 && *length + (size_t)n < size) *length += (size_t)n;
    return n;
}

static int TestPrintf(const char *format, ...)
{
    va_list args;
    int n;

    va_start(args, format);
    n = Append(g_out, sizeof(g_out), &g_outLen, format, args);
    va_end(args);
    return n;
}

static int TestFprintf(FILE *stream, const char *format, ...)
{
    va_list args;
    int n;

    va_start(args, format);
    if (stream == stderr) n = Append(g_err, sizeof(g_err), &g_errLen, format, args);
    else n = Append(g_out, sizeof(g_out), &g_outLen, format, args);
    va_end(args);
    return n;
}

#define _SETUPAPI_                  // the fakes are defined here, not imported
#define SetupDiGetClassDevsW FakeSetupDiGetClassDevsW
#define SetupDiEnumDeviceInterfaces FakeSetupDiEnumDeviceInterfaces
#define SetupDiGetDeviceInterfaceDetailW FakeSetupDiGetDeviceInterfaceDetailW
#define SetupDiGetDeviceRegistryPropertyW FakeSetupDiGetDeviceRegistryPropertyW
#define SetupDiDestroyDeviceInfoList FakeSetupDiDestroyDeviceInfoList
#define D3DKMTOpenAdapterFromDeviceName FakeD3DKMTOpenAdapterFromDeviceName
#define D3DKMTEscape FakeD3DKMTEscape
#define D3DKMTCloseAdapter FakeD3DKMTCloseAdapter
#define wmain CliMain
#define printf TestPrintf
#define fprintf TestFprintf
#include "bc250kmd_cli.c"
#undef printf
#undef fprintf

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)    // ntstatus.h, which a user-mode build does not include
#endif
#ifndef STATUS_NOT_SUPPORTED
#define STATUS_NOT_SUPPORTED ((NTSTATUS)0xC00000BBL)
#endif

// ---- the fake adapter: what the tool asked for ------------------------------------------------------------------

static struct {
    unsigned long Lists, Opens, Closes, Escapes;
    unsigned long Magic, Command;   // the head of the first escape
} g_seen;

HDEVINFO WINAPI FakeSetupDiGetClassDevsW(const GUID *ClassGuid, PCWSTR Enumerator, HWND hwndParent, DWORD Flags)
{
    (void)ClassGuid; (void)Enumerator; (void)hwndParent; (void)Flags;
    g_seen.Lists++;
    return (HDEVINFO)(ULONG_PTR)0x1234;
}

BOOL WINAPI FakeSetupDiEnumDeviceInterfaces(HDEVINFO DeviceInfoSet, PSP_DEVINFO_DATA DeviceInfoData,
                                            const GUID *InterfaceClassGuid, DWORD MemberIndex,
                                            PSP_DEVICE_INTERFACE_DATA DeviceInterfaceData)
{
    (void)DeviceInfoSet; (void)DeviceInfoData; (void)InterfaceClassGuid; (void)DeviceInterfaceData;
    if (MemberIndex == 0) return TRUE;
    SetLastError(ERROR_NO_MORE_ITEMS);
    return FALSE;
}

BOOL WINAPI FakeSetupDiGetDeviceInterfaceDetailW(HDEVINFO DeviceInfoSet, PSP_DEVICE_INTERFACE_DATA DeviceInterfaceData,
                                                 PSP_DEVICE_INTERFACE_DETAIL_DATA_W DeviceInterfaceDetailData,
                                                 DWORD DeviceInterfaceDetailDataSize, PDWORD RequiredSize,
                                                 PSP_DEVINFO_DATA DeviceInfoData)
{
    (void)DeviceInfoSet; (void)DeviceInterfaceData; (void)DeviceInfoData;
    if (RequiredSize) *RequiredSize = 0;
    wcscpy_s(DeviceInterfaceDetailData->DevicePath,
             (DeviceInterfaceDetailDataSize - FIELD_OFFSET(SP_DEVICE_INTERFACE_DETAIL_DATA_W, DevicePath)) / sizeof(WCHAR),
             L"fake-interface");
    return TRUE;
}

BOOL WINAPI FakeSetupDiGetDeviceRegistryPropertyW(HDEVINFO DeviceInfoSet, PSP_DEVINFO_DATA DeviceInfoData,
                                                  DWORD Property, PDWORD PropertyRegDataType, PBYTE PropertyBuffer,
                                                  DWORD PropertyBufferSize, PDWORD RequiredSize)
{
    const WCHAR *value = (Property == SPDRP_HARDWAREID) ? BC250_DEFAULT_HWID : L"fake BC-250";

    (void)DeviceInfoSet; (void)DeviceInfoData; (void)PropertyRegDataType; (void)RequiredSize;
    wcscpy_s((WCHAR *)PropertyBuffer, PropertyBufferSize / sizeof(WCHAR), value);
    return TRUE;
}

BOOL WINAPI FakeSetupDiDestroyDeviceInfoList(HDEVINFO DeviceInfoSet)
{
    (void)DeviceInfoSet;
    return TRUE;
}

NTSTATUS APIENTRY FakeD3DKMTOpenAdapterFromDeviceName(D3DKMT_OPENADAPTERFROMDEVICENAME *open)
{
    open->hAdapter = 0x40000040;
    g_seen.Opens++;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY FakeD3DKMTCloseAdapter(const D3DKMT_CLOSEADAPTER *close)
{
    (void)close;
    g_seen.Closes++;
    return STATUS_SUCCESS;
}

// Every BC250_ESCAPE_* request starts with Magic and Command (driver/kmd/bc250kmd_escape.h).
NTSTATUS APIENTRY FakeD3DKMTEscape(const D3DKMT_ESCAPE *escape)
{
    const unsigned long *head = (const unsigned long *)escape->pPrivateDriverData;

    if (g_seen.Escapes++ == 0 && head != NULL && escape->PrivateDriverDataSize >= 2 * sizeof(unsigned long)) {
        g_seen.Magic = head[0];
        g_seen.Command = head[1];
    }
    return STATUS_NOT_SUPPORTED;
}

// ---- the cases ------------------------------------------------------------------------------------------------

static WCHAR g_regName[128];        // `read <name>`: a name of the driver's named read list in a lab build
static WCHAR g_bmp[MAX_PATH];       // `fbdump <file>`: beside this test, and never written

typedef struct {
    const WCHAR *Args[5];           // argv[1] on, up to the first NULL
    unsigned long Command;          // the escape that the lab handler sends first
} DISPATCH_CASE;

static const DISPATCH_CASE g_cases[] = {
    { { L"read", L"0x1000" }, BC250_ESCAPE_READ_REG },
    { { L"read", L"1000" }, BC250_ESCAPE_READ_REG },
    { { L"read", g_regName }, BC250_ESCAPE_READ_REG },
    { { L"write", L"0x1000", L"0x1" }, BC250_ESCAPE_WRITE_REG },
    { { L"memory" }, BC250_ESCAPE_GET_MEMORY },
    { { L"vread", L"phys", L"0x0" }, BC250_ESCAPE_VRAM_READ },
    { { L"vread", L"bar0", L"0x0" }, BC250_ESCAPE_VRAM_READ },
    { { L"vwrite", L"phys", L"0x0", L"0x1" }, BC250_ESCAPE_VRAM_WRITE },
    { { L"vwrite", L"bar0", L"0x0", L"0x1" }, BC250_ESCAPE_VRAM_WRITE },
    { { L"vcompare", L"0x0", L"1" }, BC250_ESCAPE_VRAM_READ },
    { { L"vtable", L"0x1000" }, BC250_ESCAPE_VRAM_READ },
    { { L"gart", L"plan" }, BC250_ESCAPE_RUN_GART },
    { { L"gart", L"enable" }, BC250_ESCAPE_RUN_GART },
    { { L"gart", L"restore" }, BC250_ESCAPE_RUN_GART },
    { { L"psp", L"plan" }, BC250_ESCAPE_RUN_PSP },
    { { L"psp", L"load" }, BC250_ESCAPE_RUN_PSP },
    { { L"psp", L"unload" }, BC250_ESCAPE_RUN_PSP },
    { { L"gfx", L"plan", L"1" }, BC250_ESCAPE_RUN_GFX },
    { { L"gfx", L"run", L"8" }, BC250_ESCAPE_RUN_GFX },
    { { L"gfx", L"fini" }, BC250_ESCAPE_RUN_GFX },
    { { L"gfx", L"state" }, BC250_ESCAPE_RUN_GFX },
    { { L"ih", L"plan" }, BC250_ESCAPE_RUN_IH },
    { { L"ih", L"init" }, BC250_ESCAPE_RUN_IH },
    { { L"ih", L"fini" }, BC250_ESCAPE_RUN_IH },
    { { L"ih", L"state" }, BC250_ESCAPE_RUN_IH },
    { { L"fence", L"gfx" }, BC250_ESCAPE_RUN_FENCE },
    { { L"fence", L"gfx", L"1" }, BC250_ESCAPE_RUN_FENCE },
    { { L"fence", L"c0", L"4", L"test" }, BC250_ESCAPE_RUN_FENCE },
    { { L"fence", L"kiq", L"noint" }, BC250_ESCAPE_RUN_FENCE },
    { { L"fence", L"s0", L"1", L"dispatch" }, BC250_ESCAPE_RUN_FENCE },
    { { L"fence", L"s1", L"1", L"ib" }, BC250_ESCAPE_RUN_FENCE },
    { { L"ib", L"0", L"0x0", L"0x1000", L"1" }, BC250_ESCAPE_RUN_FENCE },
    { { L"dcn" }, BC250_ESCAPE_RUN_DCN },
    { { L"dcnflip", L"restore" }, BC250_ESCAPE_RUN_DCNFLIP },
    { { L"dcnflip", L"0x1000" }, BC250_ESCAPE_RUN_DCNFLIP },
    { { L"dcnflip", L"0x1000", L"fill", L"0xFF000000" }, BC250_ESCAPE_RUN_DCNFLIP },
    { { L"sdmacopy" }, BC250_ESCAPE_RUN_SDMACOPY },
    { { L"sdmacopy", L"4096" }, BC250_ESCAPE_RUN_SDMACOPY },
    { { L"sdmaib" }, BC250_ESCAPE_RUN_SDMAIB },
    { { L"sdmaib", L"4096" }, BC250_ESCAPE_RUN_SDMAIB },
    { { L"fbdump", g_bmp }, BC250_ESCAPE_RUN_FBDUMP },
};

// The raw diagnostic verbs (test_lab_verbs.py holds this list to the CLI's dispatch and to test-flavour.ps1).
static const WCHAR *const g_verbs[] = {
    L"read", L"write", L"memory", L"vread", L"vwrite", L"vcompare", L"vtable", L"gart", L"psp", L"gfx", L"ih",
    L"fence", L"ib", L"dcn", L"dcnflip", L"sdmacopy", L"sdmaib", L"fbdump",
};

static int g_checks, g_failures;

static void Check(int ok, const char *what, const WCHAR *const *args)
{
    g_checks++;
    if (ok) return;
    g_failures++;
    fprintf(stderr, "FAIL %s:", what);
    for (int i = 0; i < 5 && args && args[i]; i++) fprintf(stderr, " %ls", args[i]);
    fprintf(stderr, "\n  stdout: %s\n  stderr: %s\n", g_out, g_err);
}

static int Run(const WCHAR *const *args)
{
    WCHAR *argv[8];
    int argc = 0, rc;

    argv[argc++] = L"bc250kmd_cli";
    for (int i = 0; i < 5 && args[i]; i++) argv[argc++] = (WCHAR *)args[i];
    argv[argc] = NULL;
    memset(&g_seen, 0, sizeof(g_seen));
    g_outLen = g_errLen = 0;
    g_out[0] = g_err[0] = 0;
    rc = CliMain(argc, argv);
    g_out[g_outLen] = 0;
    g_err[g_errLen] = 0;
    return rc;
}

static int FileExists(const WCHAR *path) { return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES; }

// One command line in one flavour. The verb is typed as given, then once more in upper case: the dispatch compares
// with _wcsicmp, so a release build must refuse "READ" as it refuses "read".
static void Case(const DISPATCH_CASE *c, int upper)
{
    const WCHAR *args[5];
    WCHAR verb[32];
    char expected[64];
    int rc;

    for (int i = 0; i < 5; i++) args[i] = c->Args[i];
    wcscpy_s(verb, ARRAYSIZE(verb), c->Args[0]);
    if (upper) _wcsupr_s(verb, ARRAYSIZE(verb));
    args[0] = verb;
    rc = Run(args);
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
    (void)expected;
    Check(rc == 1, "lab: the handler stops at the failed escape, exit 1", args);
    Check(g_seen.Escapes == 1, "lab: exactly one escape reached the adapter", args);
    Check(g_seen.Magic == BC250_ESCAPE_MAGIC && g_seen.Command == c->Command, "lab: the handler's escape command", args);
    Check(g_seen.Opens == 1 && g_seen.Closes == 1, "lab: one adapter opened and closed", args);
    Check(strstr(g_out, "D3DKMTEscape -> 0xC00000BB") != NULL, "lab: the handler reports the escape's status", args);
#else
    sprintf_s(expected, sizeof(expected), "unknown command %ls\n", verb);
    Check(rc == 2, "release: exit 2", args);
    Check(strstr(g_err, expected) != NULL, "release: unknown command", args);
    Check(g_seen.Lists == 0 && g_seen.Opens == 0 && g_seen.Escapes == 0, "release: no adapter list, open or escape", args);
#endif
    Check(!FileExists(g_bmp), "no BMP file written", args);
}

int main(void)
{
    static const WCHAR *const info[] = { L"info", NULL };
    DWORD n;
    WCHAR *slash;
    int rc;

#ifdef AMDGPU_WDDM_LAB_FLAVOUR
    for (size_t i = 0; g_RegNames[0].Name[i] && i + 1 < ARRAYSIZE(g_regName); i++) g_regName[i] = (WCHAR)g_RegNames[0].Name[i];
#else
    wcscpy_s(g_regName, ARRAYSIZE(g_regName), L"GRBM_STATUS");
#endif
    n = GetModuleFileNameW(NULL, g_bmp, ARRAYSIZE(g_bmp));
    slash = wcsrchr(g_bmp, L'\\');
    if (n == 0 || n >= ARRAYSIZE(g_bmp) || slash == NULL || wcslen(g_bmp) + 32 >= ARRAYSIZE(g_bmp)) {
        fprintf(stderr, "verbs_test: no path for the BMP file\n");
        return 1;
    }
    wcscpy_s(slash + 1, ARRAYSIZE(g_bmp) - (size_t)(slash + 1 - g_bmp), L"verbs_test.bmp");
    DeleteFileW(g_bmp);

    // The control: a release verb reaches the fake adapter in either flavour.
    rc = Run(info);
    Check(rc == 1 && g_seen.Lists == 1 && g_seen.Opens == 1 && g_seen.Escapes == 1 &&
          g_seen.Magic == BC250_ESCAPE_MAGIC && g_seen.Command == BC250_ESCAPE_GET_INFO,
          "control: info sends GET_INFO through the fakes", info);

    for (size_t v = 0; v < ARRAYSIZE(g_verbs); v++) {
        const WCHAR *verb[] = { g_verbs[v], NULL };
        int cases = 0;

        for (size_t i = 0; i < ARRAYSIZE(g_cases); i++) cases += !wcscmp(g_cases[i].Args[0], g_verbs[v]);
        Check(cases > 0, "every raw verb has a case", verb);
    }
    for (size_t i = 0; i < ARRAYSIZE(g_cases); i++) {
        Case(&g_cases[i], 0);
        Case(&g_cases[i], 1);
    }

    printf("verbs_test (%s): %zu command lines, %d checks, %d failures\n",
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
           "lab",
#else
           "release",
#endif
           2 * ARRAYSIZE(g_cases), g_checks, g_failures);
    return g_failures ? 1 : 0;
}
