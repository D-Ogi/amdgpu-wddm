// The driver version that the adapter's software key reports, for the per-application graphics setting
// ReportAmdDriverVersion (docs/design/per-app-graphics-settings.md).
//
// Some games compare the DriverVersion string of the display adapter's software key (the DeviceKey that
// EnumDisplayDevices returns, HKLM\SYSTEM\CurrentControlSet\Control\Video\{...}\0000) with a minimum version per GPU
// vendor. For vendor 0x1002 they expect AMD's numbering, whose first field is far above this driver's 0: the
// version 0.7.216.18 reads as a very old AMD driver, and the game shows a warning about a known driver problem. With
// ReportAmdDriverVersion = 1 the kernel-mode driver writes the AMD-scheme number a.b.c.d -> (a + 40).b.c.d in that
// value at each adapter start: 0.7.216.18 -> 40.7.216.18. The installed number stays in Bc250DriverVersion and comes
// back when the setting is cleared. The PnP driver key (Control\Class) and the driver store keep the INF's number.
//
// Pure: no WDK header, so driver/kmd/test/driver_version_test.c checks it on the host. driver_version.c does the
// registry work in the driver.
#pragma once

#define DRIVER_VERSION_AMD_OFFSET 40u       // added to the first field
#define DRIVER_VERSION_CHARS 24u            // "65535.65535.65535.65535" and its terminator

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
