// The driver version that the adapter's software key reports, for the per-application graphics setting
// ReportAmdDriverVersion (docs/design/per-app-graphics-settings.md).
//
// Some games compare the DriverVersion string of the display adapter's video key (the DeviceKey that
// EnumDisplayDevices returns, HKLM\SYSTEM\CurrentControlSet\Control\Video\{VideoID}\0000) with a minimum version per
// GPU vendor. For vendor 0x1002 they expect AMD's numbering, whose first field is far above this driver's 0: the
// version 0.7.216.18 reads as a very old AMD driver, and the game shows a warning about a known driver problem. With
// ReportAmdDriverVersion = 1 the kernel-mode driver writes the AMD-scheme number a.b.c.d -> (a + 40).b.c.d in that
// value at each adapter start: 0.7.216.18 -> 40.7.216.18. The installed number stays in Bc250DriverVersion and comes
// back when the setting is cleared. The driver store keeps the INF's number, and so does the device property that
// the SetupAPI reports.
//
// The video key is not the key that DXGK_DEVICE_INFO.DeviceRegistryPath gives (b23 lab 489: that one is the class
// key), so driver_version.c reads the GUID of the video key from the VideoID value of the adapter's hardware key and
// builds the path from it. The helpers below check the two parts of that path and join them.
//
// Pure: no WDK header, so driver/kmd/test/driver_version_test.c checks it on the host. driver_version.c does the
// registry work in the driver.
#pragma once

#define DRIVER_VERSION_AMD_OFFSET 40u       // added to the first field
#define DRIVER_VERSION_CHARS 24u            // "65535.65535.65535.65535" and its terminator
#define DRIVER_VERSION_GUID_CHARS 38u       // "{00000000-0000-0000-0000-000000000000}", no terminator
#define DRIVER_VERSION_INSTANCE_CHARS 4u    // "0000", no terminator
#define DRIVER_VERSION_PATH_CHARS 128u      // the video key path and its terminator fit with room to spare

// What a start does with the three values of the software key.
typedef enum _DRIVER_VERSION_ACTION {
    DriverVersionNone = 0,      // nothing to write
    DriverVersionReport = 1,    // DriverVersion = Reported, Bc250DriverVersion = Backup, Bc250ReportedDriverVersion = Reported
    DriverVersionRestore = 2,   // DriverVersion = Backup, then delete Bc250DriverVersion and Bc250ReportedDriverVersion
    DriverVersionForget = 3,    // delete Bc250DriverVersion and Bc250ReportedDriverVersion; DriverVersion stays
    DriverVersionInvalid = 4    // DriverVersion is not a.b.c.d (or the first field would pass 65535): nothing written
} DRIVER_VERSION_ACTION;

typedef struct _DRIVER_VERSION_PLAN {
    DRIVER_VERSION_ACTION Action;
    wchar_t Backup[DRIVER_VERSION_CHARS];       // Report: the installed number; Restore: the number put back
    wchar_t Reported[DRIVER_VERSION_CHARS];     // Report: the AMD-scheme number
} DRIVER_VERSION_PLAN;

static __inline int DriverVersionSame(const wchar_t* A, const wchar_t* B)
{
    unsigned long i;
    if (!A || !B) return 0;
    for (i = 0; i < DRIVER_VERSION_CHARS; i++) {
        if (A[i] != B[i]) return 0;
        if (!A[i]) return 1;
    }
    return 0;
}

static __inline void DriverVersionCopy(wchar_t* Out, const wchar_t* In)
{
    unsigned long i;
    for (i = 0; i + 1 < DRIVER_VERSION_CHARS && In[i]; i++) Out[i] = In[i];
    Out[i] = 0;
}

// "a.b.c.d": four decimal fields of 1 to 5 digits, each at most 65535, nothing else. 1 when it parses.
static __inline int DriverVersionParse(const wchar_t* Text, unsigned long Fields[4])
{
    unsigned long field = 0, digits = 0, value = 0, i;
    if (!Text) return 0;
    for (i = 0; i < DRIVER_VERSION_CHARS; i++) {
        const wchar_t c = Text[i];
        if (c >= L'0' && c <= L'9') {
            if (++digits > 5) return 0;
            value = value * 10u + (unsigned long)(c - L'0');
            if (value > 65535u) return 0;
        } else if (c == L'.' || c == 0) {
            if (!digits || field > 3) return 0;
            Fields[field++] = value;
            value = 0;
            digits = 0;
            if (c == 0) return field == 4;
            if (field == 4) return 0;
        } else {
            return 0;
        }
    }
    return 0;
}

// Fields as "a.b.c.d" in Out (DRIVER_VERSION_CHARS wide characters with the terminator).
static __inline void DriverVersionFormat(const unsigned long Fields[4], wchar_t Out[DRIVER_VERSION_CHARS])
{
    unsigned long n = 0, f;
    for (f = 0; f < 4; f++) {
        wchar_t digits[5];
        unsigned long count = 0, v = Fields[f];
        do { digits[count++] = (wchar_t)(L'0' + v % 10u); v /= 10u; } while (v && count < 5);
        if (f) Out[n++] = L'.';
        while (count) Out[n++] = digits[--count];
    }
    Out[n] = 0;
}

// The AMD-scheme number of Installed: the first field plus DRIVER_VERSION_AMD_OFFSET. 1 on success.
static __inline int DriverVersionAmdScheme(const wchar_t* Installed, wchar_t Out[DRIVER_VERSION_CHARS])
{
    unsigned long fields[4];
    if (!DriverVersionParse(Installed, fields) || fields[0] + DRIVER_VERSION_AMD_OFFSET > 65535u) return 0;
    fields[0] += DRIVER_VERSION_AMD_OFFSET;
    DriverVersionFormat(fields, Out);
    return 1;
}

// What a start does. Enabled: ReportAmdDriverVersion is 1. Current, Backup, Reported: DriverVersion,
// Bc250DriverVersion and Bc250ReportedDriverVersion of the software key, null when absent.
//
// A DriverVersion equal to the number this driver reported last is its own write; any other DriverVersion is the
// installed number (a new install, or Windows wrote the key again), and it becomes the backup. A backup lost by hand
// while the reported number is still in place is not remapped: 40.x would become 80.x.
static __inline void DriverVersionDecide(int Enabled, const wchar_t* Current, const wchar_t* Backup,
                                         const wchar_t* Reported, DRIVER_VERSION_PLAN* Plan)
{
    const int ours = Reported && DriverVersionSame(Current, Reported);
    Plan->Action = DriverVersionNone;
    Plan->Backup[0] = 0;
    Plan->Reported[0] = 0;
    if (Enabled) {
        if (ours) return;
        if (!DriverVersionAmdScheme(Current, Plan->Reported)) {
            Plan->Action = DriverVersionInvalid;
            Plan->Reported[0] = 0;
            return;
        }
        DriverVersionCopy(Plan->Backup, Current);
        Plan->Action = DriverVersionReport;
        return;
    }
    if (!Backup && !Reported) return;
    if (ours && Backup) {
        unsigned long fields[4];
        if (DriverVersionParse(Backup, fields)) {
            DriverVersionCopy(Plan->Backup, Backup);
            Plan->Action = DriverVersionRestore;
            return;
        }
    }
    Plan->Action = DriverVersionForget;
}

// The registry path of the video keys of one adapter, without the GUID. The GUID of the adapter comes from the
// VideoID value of its hardware key, and each numbered subkey below it is one video key of the adapter.
#define DRIVER_VERSION_VIDEO_ROOT L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Control\\Video\\"

// 1 when Text (Chars wide characters, no terminator needed) is a GUID in the registry form
// "{8-4-4-4-12 hexadecimal digits}", as the VideoID value holds it. Hexadecimal digits of both cases are accepted.
static __inline int DriverVersionIsGuidText(const wchar_t* Text, unsigned long Chars)
{
    unsigned long i;
    if (!Text || Chars != DRIVER_VERSION_GUID_CHARS) return 0;
    if (Text[0] != L'{' || Text[37] != L'}') return 0;
    for (i = 1; i < 37; i++) {
        const wchar_t c = Text[i];
        if (i == 9 || i == 14 || i == 19 || i == 24) {
            if (c != L'-') return 0;
        } else if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F'))) {
            return 0;
        }
    }
    return 1;
}

// 1 when Name (Chars wide characters, no terminator needed) is the name of a video key of an adapter: four decimal
// digits, as in "0000". A subkey with another name (for example "Video") is not one.
static __inline int DriverVersionIsInstanceName(const wchar_t* Name, unsigned long Chars)
{
    unsigned long i;
    if (!Name || Chars != DRIVER_VERSION_INSTANCE_CHARS) return 0;
    for (i = 0; i < Chars; i++) {
        if (Name[i] < L'0' || Name[i] > L'9') return 0;
    }
    return 1;
}

// The full registry path of one video key, or of the key that holds them when Instance is null:
//
//   \Registry\Machine\SYSTEM\CurrentControlSet\Control\Video\{VideoID}
//   \Registry\Machine\SYSTEM\CurrentControlSet\Control\Video\{VideoID}\0000
//
// Guid and Instance are given with their character counts and no terminator. The return value is the number of
// characters in Out without its terminator, or 0 when a part does not pass its check or the path does not fit.
static __inline unsigned long DriverVersionVideoPath(const wchar_t* Guid, unsigned long GuidChars,
                                                     const wchar_t* Instance, unsigned long InstanceChars,
                                                     wchar_t* Out, unsigned long OutChars)
{
    static const wchar_t root[] = DRIVER_VERSION_VIDEO_ROOT;
    const unsigned long rootChars = (unsigned long)(sizeof(root) / sizeof(root[0])) - 1u;
    unsigned long n = 0, i;

    if (!Out || !OutChars) return 0;
    Out[0] = 0;
    if (!DriverVersionIsGuidText(Guid, GuidChars)) return 0;
    if (Instance && !DriverVersionIsInstanceName(Instance, InstanceChars)) return 0;
    if (!Instance) InstanceChars = 0;
    // the terminator and, with an instance, the separator
    if (rootChars + GuidChars + (Instance ? 1u + InstanceChars : 0u) + 1u > OutChars) return 0;
    for (i = 0; i < rootChars; i++) Out[n++] = root[i];
    for (i = 0; i < GuidChars; i++) Out[n++] = Guid[i];
    if (Instance) {
        Out[n++] = L'\\';
        for (i = 0; i < InstanceChars; i++) Out[n++] = Instance[i];
    }
    Out[n] = 0;
    return n;
}

// 1 when Path (Chars wide characters, no terminator needed) names a key below Control\Video, compared without case:
// the only key this driver writes the number in.
static __inline int DriverVersionIsVideoKey(const wchar_t* Path, unsigned long Chars)
{
    static const wchar_t needle[] = L"\\CONTROL\\VIDEO\\";
    const unsigned long length = (unsigned long)(sizeof(needle) / sizeof(needle[0])) - 1u;
    unsigned long i, j;
    if (!Path || Chars < length) return 0;
    for (i = 0; i + length <= Chars; i++) {
        for (j = 0; j < length; j++) {
            wchar_t c = Path[i + j];
            if (c >= L'a' && c <= L'z') c = (wchar_t)(c - L'a' + L'A');
            if (c != needle[j]) break;
        }
        if (j == length) return 1;
    }
    return 0;
}
