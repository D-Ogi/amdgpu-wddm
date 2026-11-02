// A fake driver for the host tests of kmd-reader.ps1 (BD-051). No adapter is opened and no escape reaches a driver.
//
// bc250kmd_cli.c is compiled into this file unchanged, with SetupAPI and the three D3DKMT calls of a clock read and a
// log read renamed to the fakes below (the method of tools/win/bc250kmd_cli/log_test.c). build-fake.ps1 compiles it
// twice: as fake_cli.exe (the CLI's own wmain, so "fake_cli clock read" prints what the CLI prints) and, with
// BC250_CONTROL_DLL, as a bc250control.dll whose exports are the real export code of the release DLL. The test then
// compares what the reader gives from the DLL with what the CLI prints from the same fake driver.
//
// The fake answers three commands, as the KMD does from 0.7.184.1 on, each with NoAdapterSynchronization alone:
//   BC250_ESCAPE_RUN_CLOCK READ         the clock, the VID and the temperature
//   BC250_ESCAPE_GET_LOG                pages of a ring of BC250_FAKE_LOG_TOTAL lines
//   BC250_ESCAPE_RUN_START_HEALTH READ  the start and presentation witness (flags BC250_FAKE_HEALTH_FLAGS, default 23;
//                                       age BC250_FAKE_HEALTH_AGE_MS, default 1500)
// Every other command, and any other flag combination, is refused.
//
// Environment of the process that loads it (read at each escape):
//   BC250_FAKE_TEMP_MC     the temperature in millidegrees (default 51234)
//   BC250_FAKE_LOG_TOTAL   the lines in the ring (default 150)
//   BC250_FAKE_NOT_ADMIN   1: GET_LOG answers BC250_ESCAPE_STATUS_NOT_ADMIN, as LogEscape does for a user
//   BC250_FAKE_BLOCK_FILE  a path: while that file exists, every escape waits (at most 60 s), as a hung escape does
//   BC250_FAKE_COUNT_FILE  a path: each escape appends one byte to it, so a test can count the escapes

#include <windows.h>
#include <stdio.h>
#include <string.h>

#define _SETUPAPI_                  // the fakes are defined here, not imported
#define SetupDiGetClassDevsW FakeSetupDiGetClassDevsW
#define SetupDiEnumDeviceInterfaces FakeSetupDiEnumDeviceInterfaces
#define SetupDiGetDeviceInterfaceDetailW FakeSetupDiGetDeviceInterfaceDetailW
#define SetupDiGetDeviceRegistryPropertyW FakeSetupDiGetDeviceRegistryPropertyW
#define SetupDiDestroyDeviceInfoList FakeSetupDiDestroyDeviceInfoList
#define D3DKMTOpenAdapterFromDeviceName FakeD3DKMTOpenAdapterFromDeviceName
#define D3DKMTEscape FakeD3DKMTEscape
#define D3DKMTCloseAdapter FakeD3DKMTCloseAdapter
#include "../../bc250kmd_cli/bc250kmd_cli.c"

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)    // ntstatus.h, which a user-mode build does not include
#endif

// ---- the fake adapter -------------------------------------------------------------------------------------------

HDEVINFO WINAPI FakeSetupDiGetClassDevsW(const GUID *ClassGuid, PCWSTR Enumerator, HWND hwndParent, DWORD Flags)
{
    (void)ClassGuid; (void)Enumerator; (void)hwndParent; (void)Flags;
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
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY FakeD3DKMTCloseAdapter(const D3DKMT_CLOSEADAPTER *close)
{
    (void)close;
    return STATUS_SUCCESS;
}

// ---- the fake driver ----------------------------------------------------------------------------------------------

static unsigned long EnvNumber(const WCHAR *name, unsigned long fallback)
{
    WCHAR text[32];
    DWORD n = GetEnvironmentVariableW(name, text, 32);
    if (n == 0 || n >= 32) return fallback;
    return wcstoul(text, NULL, 10);
}

static void WaitWhileBlocked(void)
{
    WCHAR path[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"BC250_FAKE_BLOCK_FILE", path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    for (int i = 0; i < 3000 && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES; i++) Sleep(20);
}

static void CountEscape(void)
{
    WCHAR path[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"BC250_FAKE_COUNT_FILE", path, MAX_PATH), written;
    HANDLE file;
    if (n == 0 || n >= MAX_PATH) return;
    file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    WriteFile(file, "e", 1, &written, NULL);
    CloseHandle(file);
}

static NTSTATUS FakeClock(BC250_ESCAPE_CLOCK *clock)
{
    clock->Version = BC250_KMD_VERSION;
    if (clock->AbiVersion != BC250_CLOCK_ABI || clock->Op != BC250_CLOCK_OP_READ) {
        clock->Status = BC250_ESCAPE_STATUS_REFUSED;
        clock->NtStatus = (unsigned long)0xC000000DL;
        return STATUS_SUCCESS;
    }
    clock->Status = BC250_ESCAPE_STATUS_DONE;
    clock->ObservedMHz = 1000;
    clock->ObservedVid = 104;
    clock->TemperatureMc = (long)EnvNumber(L"BC250_FAKE_TEMP_MC", 51234);
    clock->Ready = 1;
    return STATUS_SUCCESS;
}

static NTSTATUS FakeHealth(BC250_ESCAPE_START_HEALTH *h)
{
    h->Version = BC250_KMD_VERSION;
    if (h->AbiVersion != BC250_START_HEALTH_ABI || h->Op != BC250_START_HEALTH_READ) {
        h->Status = BC250_ESCAPE_STATUS_REFUSED;
        h->NtStatus = (unsigned long)0xC000000DL;
        return STATUS_SUCCESS;
    }
    h->Status = BC250_ESCAPE_STATUS_DONE;
    h->Flags = EnvNumber(L"BC250_FAKE_HEALTH_FLAGS", 23);
    h->Generation = 7;
    h->Epoch = 1234567890123ull;
    h->Completed = 4242;
    h->LastCompletionAgeMs = EnvNumber(L"BC250_FAKE_HEALTH_AGE_MS", 1500);
    h->ReadyAgeMs = 90000;
    return STATUS_SUCCESS;
}

static NTSTATUS FakeLog(BC250_ESCAPE_LOG *log)
{
    unsigned long total = EnvNumber(L"BC250_FAKE_LOG_TOTAL", 150), from = log->From, returned = 0;

    log->Version = BC250_KMD_VERSION;
    if (EnvNumber(L"BC250_FAKE_NOT_ADMIN", 0)) {
        log->Status = BC250_ESCAPE_STATUS_NOT_ADMIN;
        log->NtStatus = (unsigned long)0xC0000022L;          // STATUS_ACCESS_DENIED, as 0.7.216.102 and later
        return STATUS_SUCCESS;
    }
    if (from == BC250_LOG_FROM_SUMMARY) from = 0;
    log->Status = BC250_ESCAPE_STATUS_DONE;
    log->Flags = BC250_ESCAPE_FLAG_FULL_WDDM;
    log->Total = total;
    log->Lost = 3;
    log->Above = 1;
    log->HeadLines = 64;
    log->RingLines = 1024;
    while (from + returned < total && returned < BC250_LOG_MAX_LINES) {
        BC250_LOG_LINE *line = &log->Lines[returned];
        line->Sequence = from + returned;
        line->Milliseconds = 1000 + 37 * (from + returned);
        _snprintf_s(line->Text, BC250_LOG_TEXT, _TRUNCATE, "fake line %lu of the ring, %s", from + returned,
                    (from + returned) % 3 ? "dpm tick" : "wddm present 0x40");
        returned++;
    }
    log->Returned = returned;
    log->From = from;
    log->Next = from + returned;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY FakeD3DKMTEscape(const D3DKMT_ESCAPE *escape)
{
    unsigned long *head = (unsigned long *)escape->pPrivateDriverData;   // Magic, Command, Status, Version, ...

    CountEscape();
    WaitWhileBlocked();
    if (escape->Type != D3DKMT_ESCAPE_DRIVERPRIVATE || head == NULL || escape->PrivateDriverDataSize < 16 ||
        head[0] != BC250_ESCAPE_MAGIC)
        return (NTSTATUS)0xC000000DL;                           // STATUS_INVALID_PARAMETER
    // Both reads are software snapshots: exactly NoAdapterSynchronization (display.c SoftwareReadEscape).
    if (!escape->Flags.NoAdapterSynchronization || escape->Flags.HardwareAccess) {
        head[2] = BC250_ESCAPE_STATUS_REFUSED;
        return (NTSTATUS)0xC00000A3L;                           // STATUS_DEVICE_NOT_READY
    }
    if (head[1] == BC250_ESCAPE_RUN_CLOCK && escape->PrivateDriverDataSize == sizeof(BC250_ESCAPE_CLOCK))
        return FakeClock((BC250_ESCAPE_CLOCK *)head);
    if (head[1] == BC250_ESCAPE_GET_LOG && escape->PrivateDriverDataSize == sizeof(BC250_ESCAPE_LOG))
        return FakeLog((BC250_ESCAPE_LOG *)head);
    if (head[1] == BC250_ESCAPE_RUN_START_HEALTH && escape->PrivateDriverDataSize == sizeof(BC250_ESCAPE_START_HEALTH))
        return FakeHealth((BC250_ESCAPE_START_HEALTH *)head);
    head[2] = BC250_ESCAPE_STATUS_UNKNOWN_COMMAND;
    return STATUS_SUCCESS;
}
