// Host test of `bc250kmd_cli log` against a fake driver (BD-054): which escapes take the adapter lock, what a
// poller's `log summary only` reads, and the escape count line the overlay checks.
//
// bc250kmd_cli.c is compiled into this file with SetupAPI and the three D3DKMT calls it makes for a log read
// renamed to the fakes below, and printf to a buffer, so that the real Log() runs unchanged. The fake answers
// GET_LOG and LOG_SUMMARY as display.c's LogEscape does from 0.7.184.1 on (GET_LOG with NoAdapterSynchronization
// alone, LOG_SUMMARY only with HardwareAccess, the BC250_LOG_FROM_SUMMARY sentinel, GuardLogRead's paging), or as
// a driver up to 0.7.183.1 does (every NoAdapterSynchronization escape refused with Status REFUSED and nothing else
// written), and from 0.7.216.24 as one that keeps the polling form's block beside the ring (BD-097: SummaryFrom
// BC250_LOG_SUMMARY_SEQ, the block paged out of that space, one note line in the ring). Built and run by
// build.ps1 before the tool itself; nothing here touches a driver.
//
//   pwsh tools\win\bc250kmd_cli\build.ps1 -Kits P:\BC-250\toolchain\nuget -Out P:\BC-250\scratch\build\bc250kmd_cli

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static char g_out[1 << 20];         // what the tool printed to stdout in the current case
static size_t g_outLen;

static int TestPrintf(const char *format, ...)
{
    va_list args;
    int n;

    va_start(args, format);
    n = vsnprintf(g_out + g_outLen, sizeof(g_out) - g_outLen, format, args);
    va_end(args);
    if (n > 0 && g_outLen + (size_t)n < sizeof(g_out)) g_outLen += (size_t)n;
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
#include "bc250kmd_cli.c"
#undef printf

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

// ---- the fake driver: the log ring and LogEscape --------------------------------------------------------------

typedef struct {
    int Before184;                  // refuse every NoAdapterSynchronization escape, as up to 0.7.183.1
    int Beside;                     // 0.7.216.24 and later: the polling form's block goes beside the ring (BD-097)
    unsigned long Total;            // lines in the ring (no wrap: the cases stay under BC250_LOG_RING_LINES)
    unsigned long SummaryLines;     // lines one LOG_SUMMARY writes
    unsigned long Opens, Closes;
    unsigned long Hard, Soft, Plain;        // escapes by flags: HardwareAccess, NoAdapterSynchronization alone, neither
    unsigned long SoftRefused;
    unsigned long SummaryFrom[8];           // the From of each LOG_SUMMARY as sent
    unsigned long SummaryStart, SummaryEnd; // the lines of the ring the last summary wrote
    int Note;                               // those lines are the one note of a block beside the ring
    unsigned long Summaries;
    unsigned long Block;                    // the block of the summary space the driver holds (BD-097)
    int BlockTaken;                         // one block has been written, so the next summary takes the next one
    unsigned long BesidePages;              // page reads answered inside the summary space
    unsigned long StealAfterPages;          // after that many pages another caller's summary replaces the block
} FAKE_DRIVER;

static FAKE_DRIVER g_driver;

NTSTATUS APIENTRY FakeD3DKMTOpenAdapterFromDeviceName(D3DKMT_OPENADAPTERFROMDEVICENAME *open)
{
    open->hAdapter = 0x40000040;
    g_driver.Opens++;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY FakeD3DKMTCloseAdapter(const D3DKMT_CLOSEADAPTER *close)
{
    (void)close;
    g_driver.Closes++;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY FakeD3DKMTEscape(const D3DKMT_ESCAPE *escape)
{
    BC250_ESCAPE_LOG *log = (BC250_ESCAPE_LOG *)escape->pPrivateDriverData;
    int hard = escape->Flags.HardwareAccess, soft = escape->Flags.NoAdapterSynchronization;
    unsigned long from, summaryFrom = 0, returned = 0;
    int summary;

    if (escape->Type != D3DKMT_ESCAPE_DRIVERPRIVATE || escape->PrivateDriverDataSize != sizeof(BC250_ESCAPE_LOG) ||
        log == NULL || log->Magic != BC250_ESCAPE_MAGIC)
        return (NTSTATUS)0xC000000DL;                           // STATUS_INVALID_PARAMETER
    if (hard) g_driver.Hard++; else if (soft) g_driver.Soft++; else g_driver.Plain++;
    summary = (log->Command == BC250_ESCAPE_LOG_SUMMARY);
    if (!summary && log->Command != BC250_ESCAPE_GET_LOG) return (NTSTATUS)0xC000000DL;

    // display.c Bc250Escape: the software read is admitted with exactly {NoAdapterSynchronization}; before 184 no
    // log read was, and the refusal wrote Status alone.
    if (soft && (g_driver.Before184 || summary || hard)) {
        g_driver.SoftRefused++;
        log->Status = BC250_ESCAPE_STATUS_REFUSED;
        return (NTSTATUS)0xC00000A3L;                           // STATUS_DEVICE_NOT_READY
    }
    log->Version = BC250_KMD_VERSION;
    // LogEscape: the summary only as a Level Two call.
    if (summary && !hard) {
        log->Status = BC250_ESCAPE_STATUS_REFUSED;
        log->NtStatus = (unsigned long)0xC0000010L;             // STATUS_INVALID_DEVICE_REQUEST
        return STATUS_SUCCESS;
    }
    from = log->From;
    if (summary) {
        if (g_driver.Summaries < 8) g_driver.SummaryFrom[g_driver.Summaries] = from;
        g_driver.Summaries++;
        // 0.7.216.24 (BD-097): the polling form's block goes beside the ring, addressed from
        // BC250_LOG_SUMMARY_SEQ, and the ring gets one line saying where it was taken. The evidence form (a
        // position of its own) still writes the block into the ring.
        if (g_driver.Beside && from == BC250_LOG_FROM_SUMMARY) {
            // Each summary answers from the next block of the summary space, so that a reader of the block
            // before it is refused instead of given a page of each (guard.c GuardLogSummaryBegin).
            if (g_driver.BlockTaken) g_driver.Block = (g_driver.Block + 1) % BC250_LOG_SUMMARY_BLOCKS;
            g_driver.BlockTaken = 1;
            summaryFrom = BC250_LOG_SUMMARY_SEQ + g_driver.Block * BC250_LOG_SUMMARY_LINES;
            g_driver.SummaryStart = g_driver.Total;     // the one ring line of this summary
            g_driver.SummaryEnd = ++g_driver.Total;
            g_driver.Note = 1;
            from = summaryFrom;
        } else {
            g_driver.Note = 0;
            summaryFrom = g_driver.Total;
            g_driver.Total += g_driver.SummaryLines;
            g_driver.SummaryStart = summaryFrom;
            g_driver.SummaryEnd = g_driver.Total;
            if (from == BC250_LOG_FROM_SUMMARY) from = summaryFrom;
        }
    } else if (from == BC250_LOG_FROM_SUMMARY) {
        from = 0;
    }
    if (from >= BC250_LOG_SUMMARY_SEQ) {
        // The summary space: the block's own lines, and a read past them returns nothing and does not move on.
        // A read of a block the driver no longer holds returns nothing either, and answers the block it does
        // hold, which is how the tool tells a replaced block from the end of its own (BD-097).
        unsigned long base = BC250_LOG_SUMMARY_SEQ + g_driver.Block * BC250_LOG_SUMMARY_LINES;
        unsigned long asked = (from - BC250_LOG_SUMMARY_SEQ) / BC250_LOG_SUMMARY_LINES;

        if (!summary) g_driver.BesidePages++;
        if (asked == g_driver.Block)
            for (unsigned long i = (from - BC250_LOG_SUMMARY_SEQ) % BC250_LOG_SUMMARY_LINES;
                 i < g_driver.SummaryLines && returned < BC250_LOG_MAX_LINES; i++, returned++) {
                BC250_LOG_LINE *line = &log->Lines[returned];
                line->Sequence = base + i;
                line->Milliseconds = 1000 + i;
                sprintf_s(line->Text, BC250_LOG_TEXT, "wddm summary: line %lu", i);
            }
        // Another caller's summary, once, between two pages of this reader's block.
        if (g_driver.StealAfterPages && g_driver.BesidePages >= g_driver.StealAfterPages) {
            g_driver.Block = (g_driver.Block + 1) % BC250_LOG_SUMMARY_BLOCKS;
            g_driver.StealAfterPages = 0;
        }
        summaryFrom = BC250_LOG_SUMMARY_SEQ + g_driver.Block * BC250_LOG_SUMMARY_LINES;
    } else
    // GuardLogRead without the wrap: from .. Total, a page at most; Next never past the end.
    for (unsigned long s = from; s < g_driver.Total && returned < BC250_LOG_MAX_LINES; s++, returned++) {
        BC250_LOG_LINE *line = &log->Lines[returned];
        line->Sequence = s;
        line->Milliseconds = 1000 + s;
        if (s >= g_driver.SummaryStart && s < g_driver.SummaryEnd)
            sprintf_s(line->Text, BC250_LOG_TEXT,
                      g_driver.Note ? "log: summary of %lu lines beside the ring" : "wddm summary: line %lu",
                      g_driver.Note ? g_driver.SummaryLines : s - g_driver.SummaryStart);
        else sprintf_s(line->Text, BC250_LOG_TEXT, "trail %lu", s);
    }
    log->SummaryFrom = summaryFrom;
    log->NtStatus = 0;
    log->Flags = BC250_ESCAPE_FLAG_FULL_WDDM;
    log->HeadLines = BC250_LOG_HEAD_LINES;
    log->RingLines = BC250_LOG_RING_LINES;
    log->From = from;
    log->Total = g_driver.Total;
    log->Lost = log->Above = 0;
    log->Next = (from >= BC250_LOG_SUMMARY_SEQ) ? from + returned :
                (from + returned < g_driver.Total) ? from + returned : g_driver.Total;
    log->Returned = returned;
    log->Status = BC250_ESCAPE_STATUS_DONE;
    return STATUS_SUCCESS;
}

// ---- the cases ------------------------------------------------------------------------------------------------

static int g_checks, g_failures;

static void Check(int ok, const char *what)
{
    g_checks++;
    if (!ok) { g_failures++; fprintf(stderr, "FAIL %s\n", what); }
}

static int Contains(const char *text) { return strstr(g_out, text) != NULL; }

// The line the tool prints for sequence s stamped at ms, with its text.
static int HasLineMs(unsigned long s, unsigned long ms, const char *text)
{
    char line[256];

    sprintf_s(line, sizeof(line), "%6lu %6lu.%03lu %s\n", s, ms / 1000, ms % 1000, text);
    return Contains(line);
}

// The line the tool prints for sequence s of the fake ring (Milliseconds = 1000 + s), with its text.
static int HasLine(unsigned long s, const char *text) { return HasLineMs(s, 1000 + s, text); }

// The line the tool prints for line i of a block beside the ring (0.7.216.24, BD-097).
static int HasBesideLine(unsigned long i, const char *text)
{
    return HasLineMs(BC250_LOG_SUMMARY_SEQ + i, 1000 + i, text);
}

// A fresh driver with `total` lines logged, and a fresh tool: the read path's memory of a refusal is per process.
static void Reset(int before184, unsigned long total, unsigned long summaryLines)
{
    memset(&g_driver, 0, sizeof(g_driver));
    g_driver.Before184 = before184;
    g_driver.Total = total;
    g_driver.SummaryLines = summaryLines;
    g_ReadsHard = 0;
    g_SoftReads = g_HardReads = 0;
    g_outLen = 0;
    g_out[0] = 0;
}

static int Run(const WCHAR *fromText, int summary)
{
    int rc = Log(fromText, summary);
    g_out[g_outLen] = 0;
    return rc;
}

int main(void)
{
    int rc;

    // The poller's form against 0.7.196: the summary is the one Level Two escape; its own 100 lines in two pages
    // without adapter synchronization, and an empty read that ends it.
    Reset(0, 300, 100);
    rc = Run(L"only", 1);
    Check(rc == 0, "summary only: exit 0");
    Check(g_driver.Summaries == 1 && g_driver.SummaryFrom[0] == BC250_LOG_FROM_SUMMARY, "summary only: sends the sentinel");
    Check(g_driver.Hard == 1 && g_driver.Soft == 2 && g_driver.Plain == 0 && g_driver.SoftRefused == 0,
          "summary only: one HardwareAccess escape, two without adapter synchronization");
    Check(HasLine(300, "wddm summary: line 0") && HasLine(399, "wddm summary: line 99") && !Contains("trail"),
          "summary only: the summary's lines, nothing before them");
    Check(Contains("             100 lines printed\n"), "summary only: 100 lines");
    Check(Contains("             escapes: 2 without adapter synchronization, 1 with HardwareAccess\n"),
          "summary only: the escape count line");
    Check(g_driver.Opens == g_driver.Closes && g_driver.Opens == 3, "summary only: every adapter handle closed");

    // The evidence form: the whole ring, still a single Level Two escape.
    Reset(0, 300, 100);
    rc = Run(NULL, 1);
    Check(rc == 0 && Contains("             400 lines printed\n"), "summary: the whole ring");
    Check(g_driver.Summaries == 1 && g_driver.SummaryFrom[0] == 0, "summary: from 0");
    Check(g_driver.Hard == 1 && g_driver.Soft == 7, "summary: one HardwareAccess escape, the pages without");
    Check(Contains("escapes: 7 without adapter synchronization, 1 with HardwareAccess\n"), "summary: counts");

    // 0.7.216.24 (BD-097): the same poll against a driver that keeps the block beside the ring. The tool is
    // unchanged - it asks for the sequence the driver answered with - so the whole block still comes back, in
    // the same number of escapes, and the ring grew by one line instead of a hundred.
    Reset(0, 300, 100);
    g_driver.Beside = 1;
    rc = Run(L"only", 1);
    Check(rc == 0, "beside: exit 0");
    Check(g_driver.Summaries == 1 && g_driver.SummaryFrom[0] == BC250_LOG_FROM_SUMMARY, "beside: sends the sentinel");
    Check(g_driver.Hard == 1 && g_driver.Soft == 2 && g_driver.SoftRefused == 0,
          "beside: one HardwareAccess escape, two without adapter synchronization");
    Check(HasBesideLine(0, "wddm summary: line 0") && HasBesideLine(99, "wddm summary: line 99") &&
          !Contains("trail"), "beside: the whole block, nothing of the ring");
    Check(Contains("             100 lines printed\n"), "beside: 100 lines");
    Check(g_driver.Total == 301, "beside: the ring grew by one line, not by the block");
    Check(g_driver.Opens == g_driver.Closes && g_driver.Opens == 3, "beside: every adapter handle closed");

    // And the evidence form of the same driver is unchanged: `log summary` asks from a position of its own, so
    // the block goes into the ring, where it is read with the lines around it.
    Reset(0, 300, 100);
    g_driver.Beside = 1;
    rc = Run(NULL, 1);
    Check(rc == 0 && Contains("             400 lines printed\n"), "beside, evidence form: the whole ring");
    Check(HasLine(300, "wddm summary: line 0") && HasLine(399, "wddm summary: line 99") && HasLine(0, "trail 0"),
          "beside, evidence form: the block inside the ring");
    Check(g_driver.Hard == 1 && g_driver.Summaries == 1, "beside, evidence form: one HardwareAccess escape");

    // Another caller's summary between two pages of this reader's block (the escapes are not serialized: the
    // pages go without adapter synchronization). The reader gets the pages it asked for before the block was
    // replaced, no line of the newer summary, and a line that says the block is gone - never a page of each.
    Reset(0, 300, 200);         // a block of 200 lines, so that the pages after the replacement are visibly gone
    g_driver.Beside = 1;
    g_driver.StealAfterPages = 1;
    rc = Run(L"only", 1);
    Check(rc == 0, "overtaken: exit 0");
    Check(Contains("             128 lines printed\n"), "overtaken: the lines of the block itself, and no more");
    Check(HasBesideLine(0, "wddm summary: line 0") && HasBesideLine(127, "wddm summary: line 127") &&
          !Contains("wddm summary: line 128"), "overtaken: nothing of the newer summary");
    Check(Contains("the block beside the ring was replaced by a newer summary after 128 lines"),
          "overtaken: the tool says the block is gone");
    Check(g_driver.Block == 1 && g_driver.Summaries == 1, "overtaken: the driver holds the newer block");

    // BD-054's shape: a driver up to 0.7.183.1 refuses the unsynchronized page once, and every read after it goes
    // with HardwareAccess. The count line is how a poller sees it.
    Reset(1, 300, 100);
    rc = Run(L"only", 1);
    Check(rc == 0 && Contains("             100 lines printed\n"), "before 184: same lines");
    Check(g_driver.SoftRefused == 1 && g_driver.Hard == 3, "before 184: one refusal, then HardwareAccess");
    Check(Contains("escapes: 0 without adapter synchronization, 3 with HardwareAccess\n"), "before 184: counts");

    // A sampler's `log N`: no Level Two escape at all.
    Reset(0, 300, 100);
    rc = Run(L"250", 0);
    Check(rc == 0 && HasLine(250, "trail 250") && !HasLine(249, "trail 249") && Contains("             50 lines printed\n"),
          "log N: from N");
    Check(g_driver.Hard == 0 && g_driver.Soft == 2 && g_driver.Summaries == 0, "log N: no HardwareAccess escape");
    Check(Contains("escapes: 2 without adapter synchronization, 0 with HardwareAccess\n"), "log N: counts");

    // The sentinel is a keyword of the summary, never a number to type, and means nothing without it.
    Reset(0, 300, 100);
    Check(Run(L"only", 0) == 2 && g_driver.Opens == 0, "log only: usage error, no escape");
    Check(Run(L"4294967295", 1) == 2 && Run(L"-1", 1) == 2 && g_driver.Opens == 0, "the sentinel as a number: usage error");

    printf("log_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
