// Boot-loop guard, breadcrumbs and log (ADR 0006, points 3 and 4). This lab has no kernel debugger and a
// hang costs the owner a walk to the machine, so the driver leaves its own trail:
//
//   <service key>\Parameters
//     UnconfirmedStarts  REG_DWORD  incremented at every DxgkDdiStartDevice, cleared from user mode
//                                   (bc250mon) once the desktop is up. At BC250_MAX_UNCONFIRMED_STARTS the
//                                   driver refuses to start and Windows falls back to Basic Display.
//     LastStage          REG_DWORD  BC250_STAGE, written and flushed at each step
//     StageHistory       REG_SZ     the last stages of this boot, oldest first
//
// Everything here runs at PASSIVE_LEVEL. DDIs that can run higher (present is not one of them on a
// display-only driver, the pointer and interrupt DDIs are) must not call in.
#include "bc250kmd.h"
#include <ntstrsafe.h>
#include <stdarg.h>

static UNICODE_STRING g_ParametersPath;
static WCHAR g_History[256];
static BC250_STAGE g_LastStage;

static NTSTATUS OpenParameters(_Out_ HANDLE* Key)
{
    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, &g_ParametersPath, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    return ZwCreateKey(Key, KEY_READ | KEY_WRITE, &attributes, 0, NULL, REG_OPTION_NON_VOLATILE, NULL);
}

static NTSTATUS ReadDword(HANDLE Key, PCWSTR Name, _Out_ ULONG* Value)
{
    UNICODE_STRING name;
    UCHAR buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION info = (PKEY_VALUE_PARTIAL_INFORMATION)buffer;
    ULONG length;
    NTSTATUS status;

    *Value = 0;
    RtlInitUnicodeString(&name, Name);
    status = ZwQueryValueKey(Key, &name, KeyValuePartialInformation, info, sizeof(buffer), &length);
    if (!NT_SUCCESS(status)) return status;
    if (info->Type != REG_DWORD || info->DataLength != sizeof(ULONG)) return STATUS_OBJECT_TYPE_MISMATCH;
    RtlCopyMemory(Value, info->Data, sizeof(ULONG));
    return STATUS_SUCCESS;
}

static NTSTATUS WriteDword(HANDLE Key, PCWSTR Name, ULONG Value)
{
    UNICODE_STRING name;
    RtlInitUnicodeString(&name, Name);
    return ZwSetValueKey(Key, &name, 0, REG_DWORD, &Value, sizeof(Value));
}

NTSTATUS GuardInit(_In_ PUNICODE_STRING RegistryPath)
{
    static const WCHAR suffix[] = L"\\Parameters";
    USHORT bytes = RegistryPath->Length + sizeof(suffix);

    g_ParametersPath.Buffer = (PWCH)ExAllocatePool2(POOL_FLAG_PAGED, bytes, BC250_TAG);
    if (g_ParametersPath.Buffer == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    g_ParametersPath.MaximumLength = bytes;
    g_ParametersPath.Length = 0;
    RtlCopyUnicodeString(&g_ParametersPath, RegistryPath);
    return RtlAppendUnicodeToString(&g_ParametersPath, suffix);
}

void GuardCleanup(void)
{
    if (g_ParametersPath.Buffer != NULL) ExFreePoolWithTag(g_ParametersPath.Buffer, BC250_TAG);
    RtlZeroMemory(&g_ParametersPath, sizeof(g_ParametersPath));
}

void GuardStage(BC250_STAGE Stage)
{
    HANDLE key;
    WCHAR item[16];
    UNICODE_STRING name;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL || g_ParametersPath.Buffer == NULL) return;
    if (Stage == g_LastStage) return;
    g_LastStage = Stage;

    // Keep the tail if the history is full: the end of the trail is what matters after a hang.
    if (NT_SUCCESS(RtlStringCchPrintfW(item, RTL_NUMBER_OF(item), L"%u ", (ULONG)Stage)) &&
        !NT_SUCCESS(RtlStringCchCatW(g_History, RTL_NUMBER_OF(g_History), item)))
    {
        RtlMoveMemory(g_History, g_History + 64, sizeof(g_History) - 64 * sizeof(WCHAR));
        RtlStringCchCatW(g_History, RTL_NUMBER_OF(g_History), item);
    }

    if (!NT_SUCCESS(OpenParameters(&key))) return;
    WriteDword(key, L"LastStage", (ULONG)Stage);
    RtlInitUnicodeString(&name, L"StageHistory");
    ZwSetValueKey(key, &name, 0, REG_SZ, g_History, (ULONG)((wcslen(g_History) + 1) * sizeof(WCHAR)));
    ZwFlushKey(key);        // the point of a breadcrumb is that it is on disk before the next step runs
    ZwClose(key);
    GuardLog("stage %u", (ULONG)Stage);
}

BC250_STAGE GuardLastStage(void)
{
    return g_LastStage;
}

NTSTATUS GuardCheckAndCountStart(void)
{
    HANDLE key;
    ULONG starts;
    NTSTATUS status = OpenParameters(&key);

    // No registry, no guard: refusing to start here would turn a registry problem into a dead display.
    if (!NT_SUCCESS(status)) return STATUS_SUCCESS;

    ReadDword(key, L"UnconfirmedStarts", &starts);
    if (starts >= BC250_MAX_UNCONFIRMED_STARTS)
    {
        ZwClose(key);
        GuardLog("guard: %u starts nobody confirmed, refusing to start", starts);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    WriteDword(key, L"UnconfirmedStarts", starts + 1);
    ZwFlushKey(key);
    ZwClose(key);
    return STATUS_SUCCESS;
}

void GuardLog(_In_z_ const char* Format, ...)
{
    // M3: the kernel debug print stream only (readable with DebugView-class tools in a lab session).
    // The TraceLogging provider of ADR 0006 point 4 replaces this body; callers stay as they are.
    va_list arguments;
    char line[200];

    va_start(arguments, Format);
    if (NT_SUCCESS(RtlStringCchVPrintfA(line, sizeof(line), Format, arguments)))
        DbgPrintEx(DPFLTR_IHVVIDEO_ID, DPFLTR_ERROR_LEVEL, "bc250kmd: %s\n", line);
    va_end(arguments);
}
