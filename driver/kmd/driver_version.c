// The per-application graphics setting ReportAmdDriverVersion (docs/design/per-app-graphics-settings.md): the driver
// version string that programs read for the adapter, in this driver's own numbering or in AMD's. The number scheme,
// the decision and the path of the key are pure and host-tested (driver_version.h,
// driver/kmd/test/driver_version_test.c); this file is the registry work around them.
//
//   HKLM\SOFTWARE\amdgpu-wddm\Graphics  ReportAmdDriverVersion    REG_DWORD, 1 = AMD's numbering; 0 or absent = ours
//   <software key>                      DriverVersion             REG_SZ, what games read, directly or through a link
//                                       Bc250DriverVersion        REG_SZ, the installed number while 1 is in force
//                                       Bc250ReportedDriverVersion  REG_SZ, the number this driver wrote
//   <hardware key>                      VideoID                   REG_SZ, the GUID of this adapter's video keys
//
// The software key of the adapter holds the last three values. It is the key that DXGK_DEVICE_INFO.DeviceRegistryPath
// names ("A Unicode string that holds the registry path of the software key for the display adapter. Registry data
// should be written only to this path", dispmprt DXGK_DEVICE_INFO), and IoOpenDeviceRegistryKey with
// PLUGPLAY_REGKEY_DRIVER opens it; a display miniport writes its own registry information there
// (display/registering-hardware-information.md). On unit A it is the class key
// Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}\<nnnn>.
//
// EnumDisplayDevices gives another path for the same values, the video key Control\Video\{VideoID}\<nnnn>, and
// Unreal Engine 4 reads DriverVersion there. Each numbered video key is a registry symbolic link to the software key
// of the adapter, and an open that does not ask for OBJ_OPENLINK follows it, so a write through either path reaches
// the same value. The write therefore also changes the version that Device Manager shows and the SetupAPI property
// DEVPKEY_Device_DriverVersion of the adapter.
//
// The software key comes first, because it exists from the installation of the device while the numbered video keys
// do not exist yet at this point of the adapter start: on the lab the Control\Video key of the adapter opened and
// held no numbered subkey, and one was there after the boot (BD-104). The video keys are visited after it, for an
// installation where such a key holds its own DriverVersion instead of a link; they normally find the number in
// place and write nothing. The GUID of those keys is the VideoID value of the adapter's hardware key
// (IoOpenDeviceRegistryKey, PLUGPLAY_REGKEY_DEVICE), and each four-digit subkey below it is one video key.
//
// Whichever path is used, the key that the open gives back must name a video key or one adapter key of the display
// class before anything is written (DriverVersionIsAdapterKey), and a path this file builds itself is opened only
// below Control\Video.
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
// Room for the name of an open key. The longest name this code admits is the class key of an adapter,
// "\REGISTRY\MACHINE\SYSTEM\ControlSet001\Control\Class\{<38>}\0000": 95 characters.
#define DRIVER_VERSION_KEYNAME_CHARS 160u

// What one start did, for the counts and the numbers of the summary line.
typedef struct _DRIVER_VERSION_RUN {
    ULONG Keys;                             // keys opened, named and decided
    ULONG Changed;                          // keys where a value was written or deleted
    ULONG Failed;                           // keys that kept their values
    ULONG SoftwareKey;                      // 1 when the software key of the adapter was one of them
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

// 1 when the key behind Key is a key that this file may write the number in, by the name that the object manager
// gives it. A numbered video key is a symbolic link to the display class key and the open follows it, so the name
// read back is normally the class key, not the path that was asked for (DriverVersionIsAdapterKey). A name that
// does not fit the buffer, or that ZwQueryKey does not give, is refused: the write target must be known.
static int KeyIsAdapterKey(HANDLE Key)
{
    UCHAR buffer[sizeof(KEY_NAME_INFORMATION) + DRIVER_VERSION_KEYNAME_CHARS * sizeof(WCHAR)];
    PKEY_NAME_INFORMATION info = (PKEY_NAME_INFORMATION)buffer;
    ULONG length = 0;
    NTSTATUS status;

    status = ZwQueryKey(Key, KeyNameInformation, info, (ULONG)sizeof(buffer), &length);
    if (!NT_SUCCESS(status)) return 0;
    if (info->NameLength > DRIVER_VERSION_KEYNAME_CHARS * sizeof(WCHAR)) return 0;
    return DriverVersionIsAdapterKey(info->Name, info->NameLength / sizeof(WCHAR));
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

// What a start does with one key that is open and named. The counts and the two numbers of the run are updated.
// The key stays open: the caller closes it.
static void ApplyToOpenKey(HANDLE Key, ULONG Enabled, _Inout_ DRIVER_VERSION_RUN* Run)
{
    WCHAR current[DRIVER_VERSION_CHARS], backup[DRIVER_VERSION_CHARS], reported[DRIVER_VERSION_CHARS];
    NTSTATUS status, currentStatus, backupStatus, reportedStatus;
    DRIVER_VERSION_PLAN plan;

    Run->Keys++;
    currentStatus = ReadVersionString(Key, L"DriverVersion", current);
    backupStatus = ReadVersionString(Key, L"Bc250DriverVersion", backup);
    reportedStatus = ReadVersionString(Key, L"Bc250ReportedDriverVersion", reported);
    DriverVersionDecide((int)Enabled, NT_SUCCESS(currentStatus) ? current : NULL,
                        NT_SUCCESS(backupStatus) ? backup : NULL, NT_SUCCESS(reportedStatus) ? reported : NULL, &plan);
    status = STATUS_SUCCESS;
    switch (plan.Action) {
    case DriverVersionReport:
        status = WriteVersionString(Key, L"Bc250DriverVersion", plan.Backup);
        if (NT_SUCCESS(status)) status = WriteVersionString(Key, L"Bc250ReportedDriverVersion", plan.Reported);
        if (NT_SUCCESS(status)) status = WriteVersionString(Key, L"DriverVersion", plan.Reported);
        if (!Run->Installed[0]) DriverVersionCopy(Run->Installed, plan.Backup);
        if (!Run->Current[0] && NT_SUCCESS(status)) DriverVersionCopy(Run->Current, plan.Reported);
        break;
    case DriverVersionRestore:
        status = WriteVersionString(Key, L"DriverVersion", plan.Backup);
        if (NT_SUCCESS(status)) status = DeleteValue(Key, L"Bc250ReportedDriverVersion");
        if (NT_SUCCESS(status)) status = DeleteValue(Key, L"Bc250DriverVersion");
        if (!Run->Installed[0]) DriverVersionCopy(Run->Installed, plan.Backup);
        if (!Run->Current[0] && NT_SUCCESS(status)) DriverVersionCopy(Run->Current, plan.Backup);
        break;
    case DriverVersionForget:
        status = DeleteValue(Key, L"Bc250ReportedDriverVersion");
        if (NT_SUCCESS(status)) status = DeleteValue(Key, L"Bc250DriverVersion");
        if (!Run->Current[0] && NT_SUCCESS(currentStatus)) DriverVersionCopy(Run->Current, current);
        break;
    case DriverVersionInvalid:
        Run->Failed++;
        GuardLog("driver version: ReportAmdDriverVersion=1 not applied: DriverVersion %ws is not a.b.c.d (0x%08X)",
                 NT_SUCCESS(currentStatus) ? current : L"(unreadable)", currentStatus);
        return;
    default:
        // Nothing to write: the number in place is already the one the setting asks for.
        if (!Run->Current[0] && NT_SUCCESS(currentStatus)) DriverVersionCopy(Run->Current, current);
        if (!Run->Installed[0] && NT_SUCCESS(backupStatus)) DriverVersionCopy(Run->Installed, backup);
        return;
    }
    if (NT_SUCCESS(status)) {
        Run->Changed++;
    } else {
        Run->Failed++;
        GuardLog("driver version: a key of this adapter kept its values: action %u failed (0x%08X)", plan.Action,
                 status);
    }
}

// The software key of this adapter, the key that the number is read from: IoOpenDeviceRegistryKey with
// PLUGPLAY_REGKEY_DRIVER (install/opening-a-device-s-software-key.md, display/registering-hardware-information.md).
// The PnP manager creates it when the device is installed, so a start always reaches it, which the numbered video
// keys do not: they are not there yet at this point of the start (BD-104). The name of the key that the open gives
// back is checked as on the video path, so this writes only an adapter key.
static void ApplyToSoftwareKey(BC250_DEVICE* Device, ULONG Enabled, _Inout_ DRIVER_VERSION_RUN* Run)
{
    HANDLE key = NULL;
    NTSTATUS status;
    ULONG changed, failed;

    if (!Device->PhysicalDeviceObject) {
        if (Enabled) GuardLog("driver version: the software key of this adapter was not opened: no device object");
        return;
    }
    status = IoOpenDeviceRegistryKey(Device->PhysicalDeviceObject, PLUGPLAY_REGKEY_DRIVER, KEY_READ | KEY_WRITE, &key);
    if (!NT_SUCCESS(status)) {
        if (Enabled) GuardLog("driver version: the software key of this adapter was not opened (0x%08X)", status);
        return;
    }
    if (!KeyIsAdapterKey(key)) {
        Run->Failed++;
        GuardLog("driver version: the software key of this adapter is not an adapter key; nothing was written");
        ZwClose(key);
        return;
    }
    changed = Run->Changed;
    failed = Run->Failed;
    ApplyToOpenKey(key, Enabled, Run);
    Run->SoftwareKey = 1;
    ZwClose(key);
    // The summary line below counts every key of the run together, so this line says what happened at the software
    // key itself: it separates a start that wrote the number here from one that found it already in place. Quiet
    // while the setting is off and nothing moved, as the rest of this file is.
    if (Enabled || Run->Changed != changed || Run->Failed != failed) {
        GuardLog("driver version: the software key of this adapter was reached, written %u, failed %u",
                 Run->Changed - changed, Run->Failed - failed);
    }
}

// What a start does with one key that this file names by its path: the path must be below Control\Video, the open
// follows the symbolic link that a numbered video key holds, and the key behind it must be an adapter key.
static void ApplyToKey(_In_z_ PCWSTR Path, ULONG Enabled, _Inout_ DRIVER_VERSION_RUN* Run)
{
    OBJECT_ATTRIBUTES attributes;
    UNICODE_STRING keyPath;
    HANDLE key = NULL;
    NTSTATUS status;

    // The first half of the guard of this file: only a path below Control\Video is opened.
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
    // The second half: the open followed a symbolic link, so the key it gave back must be named as well.
    if (!KeyIsAdapterKey(key)) {
        Run->Failed++;
        GuardLog("driver version: the video key led to a key that is not an adapter key; nothing was written");
        ZwClose(key);
        return;
    }
    ApplyToOpenKey(key, Enabled, Run);
    ZwClose(key);
}

// Every numbered subkey of Control\Video\{Guid}. Each one is a symbolic link to the software key of the adapter, so
// a key found here normally holds the number already and nothing is written. At this point of the start there is
// usually no such subkey at all (BD-104), which is no error: a missing video path reaches the log only when the
// software key was not reached either.
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
        if (Enabled && !Run->SoftwareKey) {
            GuardLog("driver version: ReportAmdDriverVersion=1 not applied: the video path does not fit");
        }
        return;
    }
    RtlInitUnicodeString(&rootPath, keyPath);
    InitializeObjectAttributes(&attributes, &rootPath, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    status = ZwOpenKey(&root, KEY_READ, &attributes);
    if (!NT_SUCCESS(status)) {
        if (Enabled && !Run->SoftwareKey) {
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
    if (index == DRIVER_VERSION_KEYS) {
        GuardLog("driver version: the Control\\Video key of this adapter holds more than %u subkeys; the rest were "
                 "not visited", DRIVER_VERSION_KEYS);
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

    // First, and the only key that a start is sure to reach: the software key of this adapter.
    ApplyToSoftwareKey(Device, enabled, &run);

    // Then the video keys, for an installation where a numbered one holds its own DriverVersion instead of a link to
    // the software key. They do not exist yet at this point of the start, so this pass normally finds nothing.
    status = ReadVideoId(Device, guid, &guidChars);
    if (NT_SUCCESS(status)) {
        ApplyToVideoKeys(guid, guidChars, enabled, &run);
    } else if (!run.SoftwareKey && path->Buffer &&
               DriverVersionIsVideoKey(path->Buffer, path->Length / sizeof(WCHAR)) &&
               path->Length / sizeof(WCHAR) + 1u <= DRIVER_VERSION_PATH_CHARS) {
        // No VideoID and no software key, but DeviceRegistryPath is itself a video key: write there instead.
        RtlCopyMemory(keyPath, path->Buffer, path->Length);
        keyPath[path->Length / sizeof(WCHAR)] = 0;
        ApplyToKey(keyPath, enabled, &run);
    } else if (enabled && !run.SoftwareKey) {
        // No key at all was reached: this is the one reason line of the start, so the summary below is not needed.
        GuardLog("driver version: ReportAmdDriverVersion=1 not applied: no VideoID for this adapter (0x%08X)", status);
        return;
    }

    // A failure counted outside a key (a name that gives no path, an enumeration that stopped) still has to reach
    // the log, so the summary is skipped only when there was nothing at all to report.
    if (!run.Keys && !run.Failed) {
        if (enabled) GuardLog("driver version: ReportAmdDriverVersion=1 not applied: no key of this adapter opened");
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
