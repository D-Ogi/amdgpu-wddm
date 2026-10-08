// The per-application graphics setting ReportAmdDriverVersion (docs/design/per-app-graphics-settings.md): the driver
// version string that programs read for the adapter, in this driver's own numbering or in AMD's. The number scheme,
// the decision and the path of the key are pure and host-tested (driver_version.h,
// driver/kmd/test/driver_version_test.c); this file is the registry work around them.
//
//   HKLM\SOFTWARE\amdgpu-wddm\Graphics  ReportAmdDriverVersion    REG_DWORD, 1 = AMD's numbering; 0 or absent = ours
//   <hardware key>                      VideoID                   REG_SZ, the GUID of this adapter's video keys
//   Control\Video\{VideoID}\<nnnn>      DriverVersion             REG_SZ, what games read through EnumDisplayDevices
//                                       Bc250DriverVersion        REG_SZ, the installed number while 1 is in force
//                                       Bc250ReportedDriverVersion  REG_SZ, the number this driver wrote
//
// The key of the last three values is the one that EnumDisplayDevices gives as the DeviceKey of the adapter, and
// Unreal Engine 4 reads DriverVersion there. It is not the key of DXGK_DEVICE_INFO.DeviceRegistryPath: on unit A
// that one is the class key, and b23 lab 489 showed this code refuse to write because of it. The GUID of the video
// keys is the VideoID value of the adapter's hardware key (IoOpenDeviceRegistryKey, PLUGPLAY_REGKEY_DEVICE), and
// each four-digit subkey below it is one video key of the adapter. Windows makes those keys show the same values as
// the class key, so a write through this path also changes the version that Device Manager shows for the adapter.
//
// It runs once per adapter start, at PASSIVE_LEVEL, and never fails the start: a key it cannot open or a value it
// cannot read leaves everything as it is, with one log line. The control application writes the setting; a change
// takes effect at the next start of the adapter (a driver restart or a Windows restart). There is no environment
// override: the kernel has no process environment.
#include "bc250kmd.h"
#include "driver_version.h"

#define GRAPHICS_KEY L"\\Registry\\Machine\\SOFTWARE\\amdgpu-wddm\\Graphics"
#define DRIVER_VERSION_READ_CHARS 40u       // the longest value this file reads: a GUID with its terminator
#define DRIVER_VERSION_KEYS 16u             // the most video keys of one adapter this code visits
#define DRIVER_VERSION_NAME_CHARS 32u       // room for the name of a subkey: "0000" needs four

// What one start did, for the counts and the numbers of the summary line.
typedef struct _DRIVER_VERSION_RUN {
    ULONG Keys;                             // video keys visited
    ULONG Changed;                          // keys where a value was written or deleted
    ULONG Failed;                           // keys that kept their values
    WCHAR Installed[DRIVER_VERSION_CHARS];  // the installed number, from the first key that gave one
    WCHAR Current[DRIVER_VERSION_CHARS];    // DriverVersion after the run, from the first key
} DRIVER_VERSION_RUN;

static ULONG StringChars(_In_z_ PCWSTR Text, ULONG Limit)
{
    ULONG chars = 0;

    while (chars < Limit && Text[chars]) chars++;
    return chars;
}

// One REG_SZ value of at most Chars - 1 characters, with its terminator in Value. STATUS_OBJECT_NAME_NOT_FOUND when
// it is absent; any other failure (another type, too long) is reported as such and the value counts as unreadable.
static NTSTATUS ReadString(HANDLE Key, PCWSTR Name, _Out_writes_(Chars) WCHAR* Value, ULONG Chars)
{
    UNICODE_STRING name;
    UCHAR buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + DRIVER_VERSION_READ_CHARS * sizeof(WCHAR)];
    PKEY_VALUE_PARTIAL_INFORMATION info = (PKEY_VALUE_PARTIAL_INFORMATION)buffer;
    ULONG length = 0, room, chars, i;
    NTSTATUS status;

    NT_ASSERT(Chars > 0 && Chars <= DRIVER_VERSION_READ_CHARS);
    Value[0] = 0;
    room = (ULONG)sizeof(KEY_VALUE_PARTIAL_INFORMATION) + Chars * sizeof(WCHAR);
    RtlInitUnicodeString(&name, Name);
    status = ZwQueryValueKey(Key, &name, KeyValuePartialInformation, info, room, &length);
    if (!NT_SUCCESS(status)) return status;     // STATUS_BUFFER_OVERFLOW: longer than this value may be
    if (info->Type != REG_SZ || info->DataLength % sizeof(WCHAR)) return STATUS_OBJECT_TYPE_MISMATCH;
    chars = info->DataLength / sizeof(WCHAR);
    // REG_SZ data normally ends with its terminator; one without it is taken as it is.
    for (i = 0; i < chars && i + 1 < Chars; i++) {
        Value[i] = ((const WCHAR*)info->Data)[i];
        if (!Value[i]) return STATUS_SUCCESS;
    }
    if (i < chars) return STATUS_BUFFER_OVERFLOW;
    Value[i] = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS ReadVersionString(HANDLE Key, PCWSTR Name, _Out_writes_(DRIVER_VERSION_CHARS) WCHAR* Value)
{
    return ReadString(Key, Name, Value, DRIVER_VERSION_CHARS);
}

static NTSTATUS WriteVersionString(HANDLE Key, PCWSTR Name, _In_z_ const WCHAR* Value)
{
    UNICODE_STRING name;
    const ULONG chars = StringChars(Value, DRIVER_VERSION_CHARS);

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

// The GUID of this adapter's video keys, from the VideoID value of its hardware key. The GUID itself is never
// logged: it names the installation.
static NTSTATUS ReadVideoId(BC250_DEVICE* Device, _Out_writes_(DRIVER_VERSION_READ_CHARS) WCHAR* Guid,
                            _Out_ ULONG* Chars)
{
    HANDLE key = NULL;
    NTSTATUS status;
    ULONG chars;

    *Chars = 0;
    Guid[0] = 0;
    if (!Device->PhysicalDeviceObject) return STATUS_DEVICE_NOT_READY;
    status = IoOpenDeviceRegistryKey(Device->PhysicalDeviceObject, PLUGPLAY_REGKEY_DEVICE, KEY_READ, &key);
    if (!NT_SUCCESS(status)) return status;
    status = ReadString(key, L"VideoID", Guid, DRIVER_VERSION_READ_CHARS);
    ZwClose(key);
    if (!NT_SUCCESS(status)) return status;
    chars = StringChars(Guid, DRIVER_VERSION_READ_CHARS);
    if (!DriverVersionIsGuidText(Guid, chars)) return STATUS_OBJECT_PATH_INVALID;
    *Chars = chars;
    return STATUS_SUCCESS;
}

// What a start does with one video key. The counts and the two numbers of the run are updated.
static void ApplyToKey(_In_z_ PCWSTR Path, ULONG Enabled, _Inout_ DRIVER_VERSION_RUN* Run)
{
    OBJECT_ATTRIBUTES attributes;
    UNICODE_STRING keyPath;
    HANDLE key = NULL;
    WCHAR current[DRIVER_VERSION_CHARS], backup[DRIVER_VERSION_CHARS], reported[DRIVER_VERSION_CHARS];
    NTSTATUS status, currentStatus, backupStatus, reportedStatus;
    DRIVER_VERSION_PLAN plan;

    Run->Keys++;
    // The guard of this file: a path outside Control\Video is never written, whatever the registry held.
    if (!DriverVersionIsVideoKey(Path, StringChars(Path, DRIVER_VERSION_PATH_CHARS))) {
        Run->Failed++;
        GuardLog("driver version: a key outside Control\\Video was refused");
        return;
    }
    RtlInitUnicodeString(&keyPath, Path);
    InitializeObjectAttributes(&attributes, &keyPath, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    status = ZwOpenKey(&key, KEY_READ | KEY_WRITE, &attributes);
    if (!NT_SUCCESS(status)) {
        Run->Failed++;
        GuardLog("driver version: a video key was not opened (0x%08X), ReportAmdDriverVersion=%u not applied there",
                 status, Enabled);
        return;
    }
    currentStatus = ReadVersionString(key, L"DriverVersion", current);
    backupStatus = ReadVersionString(key, L"Bc250DriverVersion", backup);
    reportedStatus = ReadVersionString(key, L"Bc250ReportedDriverVersion", reported);
    DriverVersionDecide((int)Enabled, NT_SUCCESS(currentStatus) ? current : NULL,
                        NT_SUCCESS(backupStatus) ? backup : NULL, NT_SUCCESS(reportedStatus) ? reported : NULL, &plan);
    status = STATUS_SUCCESS;
    switch (plan.Action) {
    case DriverVersionReport:
        status = WriteVersionString(key, L"Bc250DriverVersion", plan.Backup);
        if (NT_SUCCESS(status)) status = WriteVersionString(key, L"Bc250ReportedDriverVersion", plan.Reported);
        if (NT_SUCCESS(status)) status = WriteVersionString(key, L"DriverVersion", plan.Reported);
        if (!Run->Installed[0]) DriverVersionCopy(Run->Installed, plan.Backup);
        if (!Run->Current[0] && NT_SUCCESS(status)) DriverVersionCopy(Run->Current, plan.Reported);
        break;
    case DriverVersionRestore:
        status = WriteVersionString(key, L"DriverVersion", plan.Backup);
        if (NT_SUCCESS(status)) status = DeleteValue(key, L"Bc250ReportedDriverVersion");
        if (NT_SUCCESS(status)) status = DeleteValue(key, L"Bc250DriverVersion");
        if (!Run->Installed[0]) DriverVersionCopy(Run->Installed, plan.Backup);
        if (!Run->Current[0] && NT_SUCCESS(status)) DriverVersionCopy(Run->Current, plan.Backup);
        break;
    case DriverVersionForget:
        status = DeleteValue(key, L"Bc250ReportedDriverVersion");
        if (NT_SUCCESS(status)) status = DeleteValue(key, L"Bc250DriverVersion");
        if (!Run->Current[0] && NT_SUCCESS(currentStatus)) DriverVersionCopy(Run->Current, current);
        break;
    case DriverVersionInvalid:
        Run->Failed++;
        GuardLog("driver version: ReportAmdDriverVersion=1 not applied: DriverVersion %ws is not a.b.c.d (0x%08X)",
                 NT_SUCCESS(currentStatus) ? current : L"(unreadable)", currentStatus);
        ZwClose(key);
        return;
    default:
        // Nothing to write: the number in place is already the one the setting asks for.
        if (!Run->Current[0] && NT_SUCCESS(currentStatus)) DriverVersionCopy(Run->Current, current);
        if (!Run->Installed[0] && NT_SUCCESS(backupStatus)) DriverVersionCopy(Run->Installed, backup);
        ZwClose(key);
        return;
    }
    ZwClose(key);
    if (NT_SUCCESS(status)) {
        Run->Changed++;
    } else {
        Run->Failed++;
        GuardLog("driver version: a video key kept its values: action %u failed (0x%08X)", plan.Action, status);
    }
}

// Every numbered subkey of Control\Video\{Guid}. Windows makes them show the same values, so the second and later
// keys normally find the number in place and write nothing.
static void ApplyToVideoKeys(_In_z_ PCWSTR Guid, ULONG GuidChars, ULONG Enabled, _Inout_ DRIVER_VERSION_RUN* Run)
{
    OBJECT_ATTRIBUTES attributes;
    UNICODE_STRING rootPath;
    HANDLE root = NULL;
    WCHAR keyPath[DRIVER_VERSION_PATH_CHARS];
    UCHAR buffer[sizeof(KEY_BASIC_INFORMATION) + DRIVER_VERSION_NAME_CHARS * sizeof(WCHAR)];
    PKEY_BASIC_INFORMATION info = (PKEY_BASIC_INFORMATION)buffer;
    NTSTATUS status;
    ULONG index, length = 0;

    if (!DriverVersionVideoPath(Guid, GuidChars, NULL, 0, keyPath, DRIVER_VERSION_PATH_CHARS)) {
        if (Enabled) GuardLog("driver version: ReportAmdDriverVersion=1 not applied: the video path does not fit");
        return;
    }
    RtlInitUnicodeString(&rootPath, keyPath);
    InitializeObjectAttributes(&attributes, &rootPath, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    status = ZwOpenKey(&root, KEY_READ, &attributes);
    if (!NT_SUCCESS(status)) {
        if (Enabled) {
            GuardLog("driver version: ReportAmdDriverVersion=1 not applied: the Control\\Video key of this adapter "
                     "was not opened (0x%08X)", status);
        }
        return;
    }
    for (index = 0; index < DRIVER_VERSION_KEYS; index++) {
        ULONG nameChars;
        status = ZwEnumerateKey(root, index, KeyBasicInformation, info, (ULONG)sizeof(buffer), &length);
        if (status == STATUS_NO_MORE_ENTRIES) break;
        // A name that does not fit DRIVER_VERSION_NAME_CHARS is not the four-digit name of a video key.
        if (status == STATUS_BUFFER_OVERFLOW || status == STATUS_BUFFER_TOO_SMALL) continue;
        if (!NT_SUCCESS(status)) {
            GuardLog("driver version: the Control\\Video key of this adapter stopped at subkey %u (0x%08X)",
                     index, status);
            Run->Failed++;
            break;
        }
        nameChars = info->NameLength / sizeof(WCHAR);
        if (!DriverVersionIsInstanceName(info->Name, nameChars)) continue;      // "Video", "Settings", others
        if (!DriverVersionVideoPath(Guid, GuidChars, info->Name, nameChars, keyPath, DRIVER_VERSION_PATH_CHARS)) {
            Run->Failed++;
            continue;
        }
        ApplyToKey(keyPath, Enabled, Run);
    }
    ZwClose(root);
}

void DriverVersionStart(BC250_DEVICE* Device)
{
    const UNICODE_STRING* path = &Device->DeviceInfo.DeviceRegistryPath;
    WCHAR guid[DRIVER_VERSION_READ_CHARS], keyPath[DRIVER_VERSION_PATH_CHARS];
    DRIVER_VERSION_RUN run;
    NTSTATUS status;
    ULONG enabled, guidChars = 0;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return;
    RtlZeroMemory(&run, sizeof(run));
    enabled = ReadReportSetting();

    status = ReadVideoId(Device, guid, &guidChars);
    if (NT_SUCCESS(status)) {
        ApplyToVideoKeys(guid, guidChars, enabled, &run);
    } else if (path->Buffer && DriverVersionIsVideoKey(path->Buffer, path->Length / sizeof(WCHAR)) &&
               path->Length / sizeof(WCHAR) + 1u <= DRIVER_VERSION_PATH_CHARS) {
        // No VideoID, but the software key of this adapter is itself a video key: write there instead.
        RtlCopyMemory(keyPath, path->Buffer, path->Length);
        keyPath[path->Length / sizeof(WCHAR)] = 0;
        ApplyToKey(keyPath, enabled, &run);
    } else if (enabled) {
        GuardLog("driver version: ReportAmdDriverVersion=1 not applied: no VideoID for this adapter (0x%08X)", status);
        return;
    } else {
        return;
    }

    if (!run.Keys) {
        if (enabled) GuardLog("driver version: ReportAmdDriverVersion=1 not applied: this adapter has no video key");
        return;
    }
    // The summary stays inside the 159 characters of the log ring (tools/quality/guardlog_width.py), so the two
    // numbers are the installed one and the one in DriverVersion now, with an arrow between them.
    if (enabled) {
        GuardLog("driver version: report=1, keys %u, written %u, failed %u, %ws -> %ws", run.Keys, run.Changed,
                 run.Failed, run.Installed[0] ? run.Installed : L"(unknown)",
                 run.Current[0] ? run.Current : L"(unreadable)");
    } else if (run.Changed || run.Failed) {
        GuardLog("driver version: report=0, keys %u, written %u, failed %u, DriverVersion %ws", run.Keys,
                 run.Changed, run.Failed, run.Current[0] ? run.Current : L"(unreadable)");
    }
}
