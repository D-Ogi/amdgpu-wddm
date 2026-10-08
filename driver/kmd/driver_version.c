// The per-application graphics setting ReportAmdDriverVersion (docs/design/per-app-graphics-settings.md): the driver
// version string of the adapter's software key, in this driver's own numbering or in AMD's. The number scheme and
// the decision are pure and host-tested (driver_version.h, driver/kmd/test/driver_version_test.c); this file is the
// registry work around them.
//
//   HKLM\SOFTWARE\amdgpu-wddm\Graphics  ReportAmdDriverVersion    REG_DWORD, 1 = AMD's numbering; 0 or absent = ours
//   <DeviceRegistryPath>                DriverVersion             REG_SZ, what games read through EnumDisplayDevices
//                                       Bc250DriverVersion        REG_SZ, the installed number while 1 is in force
//                                       Bc250ReportedDriverVersion  REG_SZ, the number this driver wrote
//
// It runs once per adapter start, at PASSIVE_LEVEL, and never fails the start: a key it cannot open or a value it
// cannot read leaves everything as it is, with one log line. The control application writes the setting; a change
// takes effect at the next start of the adapter (a driver restart or a Windows restart). There is no environment
// override: the kernel has no process environment.
#include "bc250kmd.h"
#include "driver_version.h"

#define GRAPHICS_KEY L"\\Registry\\Machine\\SOFTWARE\\amdgpu-wddm\\Graphics"

// One REG_SZ value of at most DRIVER_VERSION_CHARS - 1 characters. STATUS_OBJECT_NAME_NOT_FOUND when it is absent;
// any other failure (another type, too long) is reported as such and the value counts as unreadable.
static NTSTATUS ReadVersionString(HANDLE Key, PCWSTR Name, _Out_writes_(DRIVER_VERSION_CHARS) WCHAR* Value)
{
    UNICODE_STRING name;
    UCHAR buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + DRIVER_VERSION_CHARS * sizeof(WCHAR)];
    PKEY_VALUE_PARTIAL_INFORMATION info = (PKEY_VALUE_PARTIAL_INFORMATION)buffer;
    ULONG length = 0, chars, i;
    NTSTATUS status;

    Value[0] = 0;
    RtlInitUnicodeString(&name, Name);
    status = ZwQueryValueKey(Key, &name, KeyValuePartialInformation, info, sizeof(buffer), &length);
    if (!NT_SUCCESS(status)) return status;     // STATUS_BUFFER_OVERFLOW: longer than any a.b.c.d
    if (info->Type != REG_SZ || info->DataLength % sizeof(WCHAR)) return STATUS_OBJECT_TYPE_MISMATCH;
    chars = info->DataLength / sizeof(WCHAR);
    // REG_SZ data normally ends with its terminator; one without it is taken as it is.
    for (i = 0; i < chars && i + 1 < DRIVER_VERSION_CHARS; i++) {
        Value[i] = ((const WCHAR*)info->Data)[i];
        if (!Value[i]) return STATUS_SUCCESS;
    }
    if (i < chars) return STATUS_BUFFER_OVERFLOW;
    Value[i] = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS WriteVersionString(HANDLE Key, PCWSTR Name, _In_z_ const WCHAR* Value)
{
    UNICODE_STRING name;
    ULONG chars = 0;

    while (chars < DRIVER_VERSION_CHARS && Value[chars]) chars++;
    RtlInitUnicodeString(&name, Name);
    return ZwSetValueKey(Key, &name, 0, REG_SZ, (PVOID)Value, (chars + 1) * sizeof(WCHAR));
}

static NTSTATUS DeleteValue(HANDLE Key, PCWSTR Name)
{
    UNICODE_STRING name;
    NTSTATUS status;

    RtlInitUnicodeString(&name, Name);
    status = ZwDeleteValueKey(Key, &name);
    return status == STATUS_OBJECT_NAME_NOT_FOUND ? STATUS_SUCCESS : status;
}

// ReportAmdDriverVersion: 1 enabled, 0 not. Absent, unreadable and out-of-range values all mean 0, the last two
// with one log line (the user-mode settings log theirs the same way).
static ULONG ReadReportSetting(void)
{
    UNICODE_STRING path, name;
    OBJECT_ATTRIBUTES attributes;
    UCHAR buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION info = (PKEY_VALUE_PARTIAL_INFORMATION)buffer;
    HANDLE key = NULL;
    ULONG length = 0, value = 0;
    NTSTATUS status;

    RtlInitUnicodeString(&path, GRAPHICS_KEY);
    InitializeObjectAttributes(&attributes, &path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    status = ZwOpenKey(&key, KEY_READ, &attributes);
    if (!NT_SUCCESS(status)) return 0;          // no Graphics key: nothing was ever set
    RtlInitUnicodeString(&name, L"ReportAmdDriverVersion");
    status = ZwQueryValueKey(key, &name, KeyValuePartialInformation, info, sizeof(buffer), &length);
    ZwClose(key);
    if (status == STATUS_OBJECT_NAME_NOT_FOUND) return 0;
    if (!NT_SUCCESS(status) || info->Type != REG_DWORD || info->DataLength != sizeof(ULONG)) {
        GuardLog("driver version: ignored ReportAmdDriverVersion from the global key: not a REG_DWORD (0x%08X)", status);
        return 0;
    }
    RtlCopyMemory(&value, info->Data, sizeof(value));
    if (value > 1) {
        GuardLog("driver version: ignored ReportAmdDriverVersion=%u from the global key (accepted: 0, 1)", value);
        return 0;
    }
    return value;
}

void DriverVersionStart(BC250_DEVICE* Device)
{
    const UNICODE_STRING* path = &Device->DeviceInfo.DeviceRegistryPath;
    OBJECT_ATTRIBUTES attributes;
    UNICODE_STRING keyPath;
    HANDLE key = NULL;
    WCHAR current[DRIVER_VERSION_CHARS], backup[DRIVER_VERSION_CHARS], reported[DRIVER_VERSION_CHARS];
    NTSTATUS status, currentStatus, backupStatus, reportedStatus;
    DRIVER_VERSION_PLAN plan;
    ULONG enabled;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return;
    enabled = ReadReportSetting();
    // The software key the system gave this adapter. It is written only when it is the Control\Video key that
    // EnumDisplayDevices names; the path itself is not logged (it holds a per-installation GUID).
    if (!path->Buffer || !DriverVersionIsVideoKey(path->Buffer, path->Length / sizeof(WCHAR))) {
        if (enabled) GuardLog("driver version: ReportAmdDriverVersion=1 not applied: the software key is not a Control\\Video key");
        return;
    }
    keyPath = *path;
    InitializeObjectAttributes(&attributes, &keyPath, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    status = ZwOpenKey(&key, KEY_READ | KEY_WRITE, &attributes);
    if (!NT_SUCCESS(status)) {
        GuardLog("driver version: software key not opened (0x%08X), ReportAmdDriverVersion=%u not applied", status, enabled);
        return;
    }
    currentStatus = ReadVersionString(key, L"DriverVersion", current);
    backupStatus = ReadVersionString(key, L"Bc250DriverVersion", backup);
    reportedStatus = ReadVersionString(key, L"Bc250ReportedDriverVersion", reported);
    DriverVersionDecide((int)enabled, NT_SUCCESS(currentStatus) ? current : NULL,
                        NT_SUCCESS(backupStatus) ? backup : NULL, NT_SUCCESS(reportedStatus) ? reported : NULL, &plan);
    switch (plan.Action) {
    case DriverVersionReport:
        status = WriteVersionString(key, L"Bc250DriverVersion", plan.Backup);
        if (NT_SUCCESS(status)) status = WriteVersionString(key, L"Bc250ReportedDriverVersion", plan.Reported);
        if (NT_SUCCESS(status)) status = WriteVersionString(key, L"DriverVersion", plan.Reported);
        GuardLog("driver version: ReportAmdDriverVersion=1, DriverVersion %ws reported as %ws (0x%08X)",
                 plan.Backup, plan.Reported, status);
        break;
    case DriverVersionRestore:
        status = WriteVersionString(key, L"DriverVersion", plan.Backup);
        if (NT_SUCCESS(status)) status = DeleteValue(key, L"Bc250ReportedDriverVersion");
        if (NT_SUCCESS(status)) status = DeleteValue(key, L"Bc250DriverVersion");
        GuardLog("driver version: ReportAmdDriverVersion=0, DriverVersion %ws restored (0x%08X)", plan.Backup, status);
        break;
    case DriverVersionForget:
        status = DeleteValue(key, L"Bc250ReportedDriverVersion");
        if (NT_SUCCESS(status)) status = DeleteValue(key, L"Bc250DriverVersion");
        GuardLog("driver version: ReportAmdDriverVersion=%u, DriverVersion %ws not ours, backups removed (0x%08X)",
                 enabled, NT_SUCCESS(currentStatus) ? current : L"(unreadable)", status);
        break;
    case DriverVersionInvalid:
        GuardLog("driver version: ReportAmdDriverVersion=1 not applied: DriverVersion %ws is not a.b.c.d (0x%08X)",
                 NT_SUCCESS(currentStatus) ? current : L"(unreadable)", currentStatus);
        break;
    default:
        if (enabled) GuardLog("driver version: ReportAmdDriverVersion=1, DriverVersion %ws in place", current);
        break;
    }
    ZwClose(key);
}
