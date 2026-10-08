// Host test of driver\kmd\driver_version.h: the AMD-scheme number of ReportAmdDriverVersion, the decision a start
// makes from the three values of the software key, and the Control\Video guard.
//
//   pwsh driver\kmd\test\run_driver_version.ps1
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "driver_version.h"

static int failures, checks;

static void Check(int ok, const char* what)
{
    checks++;
    if (!ok) {
        failures++;
        printf("FAIL  %s\n", what);
    }
}

static void Scheme(const wchar_t* in, const wchar_t* want)
{
    wchar_t out[DRIVER_VERSION_CHARS];
    char what[128];
    const int ok = DriverVersionAmdScheme(in, out);
    snprintf(what, sizeof(what), "AMD scheme of %ls", in ? in : L"(null)");
    if (want) Check(ok && wcscmp(out, want) == 0, what);
    else Check(!ok, what);
}

static void Plan(int enabled, const wchar_t* current, const wchar_t* backup, const wchar_t* reported,
                 DRIVER_VERSION_ACTION action, const wchar_t* wantBackup, const wchar_t* wantReported, const char* what)
{
    DRIVER_VERSION_PLAN plan;
    memset(&plan, 0xA5, sizeof(plan));
    DriverVersionDecide(enabled, current, backup, reported, &plan);
    Check(plan.Action == action && wcscmp(plan.Backup, wantBackup) == 0 && wcscmp(plan.Reported, wantReported) == 0,
          what);
}

int main(void)
{
    unsigned long fields[4];

    // The number scheme: the first field plus 40, the other three unchanged.
    Scheme(L"0.7.216.18", L"40.7.216.18");
    Scheme(L"0.7.216.100", L"40.7.216.100");
    Scheme(L"1.0.0.0", L"41.0.0.0");
    Scheme(L"65495.65535.65535.65535", L"65535.65535.65535.65535");
    Scheme(L"65496.0.0.0", NULL);                   // the first field would pass 65535
    Scheme(L"0.7.216", NULL);                       // three fields
    Scheme(L"0.7.216.18.1", NULL);                  // five fields
    Scheme(L"0.7.216.65536", NULL);                 // a field above 65535
    Scheme(L"0.7.216.000018", NULL);                // six digits
    Scheme(L"0.7..18", NULL);                       // an empty field
    Scheme(L".7.216.18", NULL);
    Scheme(L"0.7.216.18.", NULL);
    Scheme(L"0.7.216.1a", NULL);
    Scheme(L" 0.7.216.18", NULL);
    Scheme(L"", NULL);
    Scheme(NULL, NULL);
    Check(DriverVersionParse(L"00.07.0216.00018", fields) && fields[0] == 0 && fields[1] == 7 && fields[2] == 216 &&
          fields[3] == 18, "leading zeros parse as their value");

    // The decision of a start.
    Plan(1, L"0.7.216.18", NULL, NULL, DriverVersionReport, L"0.7.216.18", L"40.7.216.18",
         "enabled, first start: back up the installed number and report the AMD scheme");
    Plan(1, L"40.7.216.18", L"0.7.216.18", L"40.7.216.18", DriverVersionNone, L"", L"",
         "enabled, the report already in place: nothing to write");
    Plan(1, L"0.7.216.100", L"0.7.216.18", L"40.7.216.18", DriverVersionReport, L"0.7.216.100", L"40.7.216.100",
         "enabled, a new install wrote its own number: that number is the new backup");
    Plan(1, L"40.7.216.18", NULL, L"40.7.216.18", DriverVersionNone, L"", L"",
         "enabled, backup lost by hand while the report stands: not remapped to 80.x");
    Plan(1, L"bogus", NULL, NULL, DriverVersionInvalid, L"", L"", "enabled, DriverVersion not a.b.c.d: nothing written");
    Plan(1, NULL, NULL, NULL, DriverVersionInvalid, L"", L"", "enabled, DriverVersion absent: nothing written");
    Plan(0, L"40.7.216.18", L"0.7.216.18", L"40.7.216.18", DriverVersionRestore, L"0.7.216.18", L"",
         "disabled after a report: the installed number comes back");
    Plan(0, L"0.7.216.100", L"0.7.216.18", L"40.7.216.18", DriverVersionForget, L"", L"",
         "disabled, a new install replaced the report: only the backup values go");
    Plan(0, L"40.7.216.18", NULL, L"40.7.216.18", DriverVersionForget, L"", L"",
         "disabled, backup lost: the stray value goes, nothing to restore");
    Plan(0, L"40.7.216.18", L"junk", L"40.7.216.18", DriverVersionForget, L"", L"",
         "disabled, a backup that is not a.b.c.d is not written back");
    Plan(0, L"0.7.216.18", NULL, NULL, DriverVersionNone, L"", L"", "disabled, never enabled: nothing to do");
    Plan(0, NULL, NULL, NULL, DriverVersionNone, L"", L"", "disabled, no values: nothing to do");

    // Only a key below Control\Video is written, whatever the case of the path.
    {
        static const wchar_t video[] = L"\\REGISTRY\\MACHINE\\SYSTEM\\ControlSet001\\Control\\Video\\{0}\\0000";
        static const wchar_t lower[] = L"\\registry\\machine\\system\\currentcontrolset\\control\\video\\{0}\\0000";
        static const wchar_t cls[] = L"\\REGISTRY\\MACHINE\\SYSTEM\\CurrentControlSet\\Control\\Class\\{0}\\0000";
        static const wchar_t cut[] = L"\\REGISTRY\\MACHINE\\SYSTEM\\CurrentControlSet\\Control\\Video";
        Check(DriverVersionIsVideoKey(video, (unsigned long)wcslen(video)), "Control\\Video key accepted");
        Check(DriverVersionIsVideoKey(lower, (unsigned long)wcslen(lower)), "Control\\Video key accepted in lower case");
        Check(!DriverVersionIsVideoKey(cls, (unsigned long)wcslen(cls)), "Control\\Class key refused");
        Check(!DriverVersionIsVideoKey(cut, (unsigned long)wcslen(cut)), "Control\\Video itself refused");
        Check(!DriverVersionIsVideoKey(video, 30), "a counted length that stops early is honoured");
        Check(!DriverVersionIsVideoKey(NULL, 10), "no path refused");
    }

    // The GUID of the video keys, as the VideoID value of the hardware key holds it. The GUID below is made up:
    // a real VideoID names one installation of Windows on one machine, and such a value stays out of this repo.
    {
        static const wchar_t upper[] = L"{0F1E2D3C-4B5A-6C7D-8E9F-A1B2C3D4E5F6}";
        static const wchar_t lower[] = L"{0f1e2d3c-4b5a-6c7d-8e9f-a1b2c3d4e5f6}";
        static const wchar_t nobrace[] = L"0F1E2D3C-4B5A-6C7D-8E9F-A1B2C3D4E5F6";
        static const wchar_t dash[] = L"{0F1E2D3C-4B5A-6C7D-8E9FA1B2C3D4E5F6-}";
        static const wchar_t hex[] = L"{0F1E2D3G-4B5A-6C7D-8E9F-A1B2C3D4E5F6}";
        Check(DriverVersionIsGuidText(upper, 38), "GUID accepted");
        Check(DriverVersionIsGuidText(lower, 38), "GUID accepted in lower case");
        Check(!DriverVersionIsGuidText(upper, 37), "GUID of the wrong length refused");
        Check(!DriverVersionIsGuidText(nobrace, 36), "GUID without braces refused");
        Check(!DriverVersionIsGuidText(dash, 38), "GUID with a dash out of place refused");
        Check(!DriverVersionIsGuidText(hex, 38), "GUID with a character that is not hexadecimal refused");
        Check(!DriverVersionIsGuidText(NULL, 38), "no GUID refused");
    }

    // The name of a video key: four decimal digits.
    {
        Check(DriverVersionIsInstanceName(L"0000", 4), "0000 accepted");
        Check(DriverVersionIsInstanceName(L"0013", 4), "0013 accepted");
        Check(!DriverVersionIsInstanceName(L"Video", 5), "Video refused");
        Check(!DriverVersionIsInstanceName(L"000", 3), "three digits refused");
        Check(!DriverVersionIsInstanceName(L"00000", 5), "five digits refused");
        Check(!DriverVersionIsInstanceName(L"00a0", 4), "a name that is not decimal refused");
        Check(!DriverVersionIsInstanceName(NULL, 4), "no name refused");
    }

    // The path of a video key, and the guard applied to the path this code builds.
    {
        static const wchar_t guid[] = L"{0F1E2D3C-4B5A-6C7D-8E9F-A1B2C3D4E5F6}";
        static const wchar_t want[] = L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Control\\Video\\"
                                      L"{0F1E2D3C-4B5A-6C7D-8E9F-A1B2C3D4E5F6}\\0000";
        wchar_t path[DRIVER_VERSION_PATH_CHARS];
        unsigned long chars = DriverVersionVideoPath(guid, 38, L"0000", 4, path, DRIVER_VERSION_PATH_CHARS);
        Check(chars == (unsigned long)wcslen(want) && wcscmp(path, want) == 0, "the path of a video key");
        Check(DriverVersionIsVideoKey(path, chars), "the path this code builds passes the Control\\Video guard");
        chars = DriverVersionVideoPath(guid, 38, NULL, 0, path, DRIVER_VERSION_PATH_CHARS);
        Check(chars == (unsigned long)wcslen(want) - 5 && wcsncmp(path, want, chars) == 0,
              "the path of the key that holds the video keys");
        // That key is only read, to enumerate the video keys; it is below Control\Video like the keys themselves.
        Check(DriverVersionIsVideoKey(path, chars), "the key that holds the video keys is also below Control\\Video");
        Check(!DriverVersionVideoPath(guid, 38, L"Video", 5, path, DRIVER_VERSION_PATH_CHARS) && !path[0],
              "a subkey that is not four digits gives no path");
        Check(!DriverVersionVideoPath(L"not a guid", 10, L"0000", 4, path, DRIVER_VERSION_PATH_CHARS) && !path[0],
              "a VideoID that is not a GUID gives no path");
        Check(!DriverVersionVideoPath(guid, 38, L"0000", 4, path, (unsigned long)wcslen(want)) && !path[0],
              "a path that does not fit the buffer is not written");
        Check(DriverVersionVideoPath(guid, 38, L"0000", 4, path, (unsigned long)wcslen(want) + 1) ==
                  (unsigned long)wcslen(want),
              "a buffer of exactly the path and its terminator is enough");
        Check(!DriverVersionVideoPath(guid, 38, L"0000", 4, NULL, DRIVER_VERSION_PATH_CHARS),
              "no buffer refused");
    }

    printf("driver_version_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
