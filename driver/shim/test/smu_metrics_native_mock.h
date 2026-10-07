/* Compiles the REAL driver/kmd/smu_metrics.c on the host: run_smu_metrics.ps1 replaces its #include "bc250kmd.h"
 * with this header, which stubs the handful of kernel primitives the file uses and puts a model of the SMU owner
 * (SmuReadMetrics) and of the VRAM page behind them. The binding is then tested, not a copy of it: the registry
 * gate, the page, the cadence, the latch, the published snapshot and the log are the ones that ship.
 *
 * The same pattern as hwmon_native_mock.h and smu_native_mock.h, for the same reason.
 */
#pragma once
#include <windows.h>
#include <stdio.h>
#include <string.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>

static volatile LONG native_checks, native_failures;
#define CHECK(x) do { InterlockedIncrement(&native_checks); if (!(x)) { InterlockedIncrement(&native_failures); \
    printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#ifndef MAXULONG
#define MAXULONG 0xffffffffUL
#endif

typedef UCHAR KIRQL;
typedef ULONG_PTR KSPIN_LOCK;
typedef KSPIN_LOCK *PKSPIN_LOCK;
typedef LARGE_INTEGER PHYSICAL_ADDRESS;

/* The reservation table of driver/kmd/bc250kmd.h, copied out of the shipping header by run_smu_metrics.ps1, so a
 * moved page is a moved page here as well. */
#include "vram_reservations.generated.h"

/* ---- kernel primitives ---------------------------------------------------------------------------------- */

static ULONG64 native_time;                 /* KeQueryInterruptTime units, 100 ns */
static ULONG64 KeQueryInterruptTime(void) { return native_time; }
static void native_advance_ms(ULONG ms) { native_time += (ULONG64)ms * 10000u; }

static void KeInitializeSpinLock(PKSPIN_LOCK lock) { *lock = 0; }
static void KeAcquireSpinLock(PKSPIN_LOCK lock, KIRQL *irql)
{
    CHECK(*lock == 0);                      /* a leaf lock, held once: no recursion */
    *lock = 1;
    *irql = 2;
}
static void KeReleaseSpinLock(PKSPIN_LOCK lock, KIRQL irql)
{
    CHECK(*lock == 1 && irql == 2);
    *lock = 0;
}

/* The VRAM page. MmMapIoSpaceEx answers the one physical address the test expects, uncached and read-write, and
 * counts every mapping; MmUnmapIoSpace must give back exactly that mapping. */
static __declspec(align(4096)) ULONG native_page[4096 / sizeof(ULONG)];
static LONGLONG native_expect_physical;
static int native_maps, native_unmaps, native_map_fail;

static PVOID MmMapIoSpaceEx(PHYSICAL_ADDRESS physical, SIZE_T bytes, ULONG protect)
{
    native_maps++;
    CHECK(physical.QuadPart == native_expect_physical);
    CHECK(bytes == 4096 && protect == (PAGE_READWRITE | PAGE_NOCACHE));
    if (native_map_fail) return NULL;
    return native_page;
}
static void MmUnmapIoSpace(PVOID address, SIZE_T bytes)
{
    native_unmaps++;
    CHECK(address == (PVOID)native_page && bytes == 4096);
}

/* ---- the driver log and the registry, as much of them as smu_metrics.c uses ----------------------------- */

#define NATIVE_LOG_LINES 64
static char native_log[NATIVE_LOG_LINES][256];
static int native_lines;

static void GuardLog(_In_z_ const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    if (native_lines < NATIVE_LOG_LINES) {
        vsnprintf(native_log[native_lines], sizeof(native_log[0]), format, arguments);
        /* The driver's ring truncates at 160 bytes (BC250_LOG_TEXT): a longer line loses its tail on the lab. */
        CHECK(strlen(native_log[native_lines]) < 160);
        native_lines++;
    }
    va_end(arguments);
}

static int native_log_has(const char *needle)
{
    int i;
    for (i = 0; i < native_lines; i++)
        if (strstr(native_log[i], needle) != NULL) return 1;
    return 0;
}

static int native_setting_present;
static ULONG native_setting_value;
static int native_setting_reads;

static ULONG GuardReadSetting(_In_z_ PCWSTR name, ULONG fallback)
{
    CHECK(wcscmp(name, L"EnableSmuMetrics") == 0);
    native_setting_reads++;
    return native_setting_present ? native_setting_value : fallback;
}

/* ---- the device ----------------------------------------------------------------------------------------- */

#include "bc250kmd_escape.h"
#include "smu_metrics.h"

typedef struct _BC250_SMU_OWNER_NATIVE {
    BOOLEAN Online;
} BC250_SMU_OWNER_NATIVE;

/* Only the fields smu_metrics.c reaches for. */
typedef struct _BC250_DEVICE {
    BOOLEAN FullWddm;
    BOOLEAN VramEnabled;
    PHYSICAL_ADDRESS VramPhysical;
    ULONGLONG VramLength;
    ULONGLONG VramMcBase;
    BC250_SMU_OWNER_NATIVE Smu;
    BC250_SMU_METRICS SmuMetrics;
} BC250_DEVICE;

/* The SMU owner (smu.c SmuReadMetrics), modelled: the result the test chose, and on success the table it chose,
 * written into the page at the address the binding named and copied out the way the owner copies it. */
static int native_reads;
static int native_result;
static ULONGLONG native_expect_mc;
static unsigned char native_table[BC250_SMU_METRICS_BYTES];

static int SmuReadMetrics(BC250_SMU_OWNER_NATIVE *owner, ULONGLONG tableMc, volatile ULONG *table, UCHAR *copy, ULONG length)
{
    ULONG i;
    native_reads++;
    CHECK(owner != NULL && tableMc == native_expect_mc && table == (volatile ULONG *)native_page);
    CHECK(copy != NULL && length == BC250_SMU_METRICS_BYTES);
    if (!owner->Online) return BC250_SMU_METRICS_OFFLINE;
    if (native_result != 0) return native_result;
    memcpy((void *)native_page, native_table, sizeof(native_table));
    for (i = 0; i < length; i++) copy[i] = ((volatile UCHAR *)table)[i];
    return 0;
}

void SmuMetricsInitialize(BC250_DEVICE *Device);
void SmuMetricsStart(BC250_DEVICE *Device);
void SmuMetricsStop(BC250_DEVICE *Device);
void SmuMetricsSample(BC250_DEVICE *Device);
BOOLEAN SmuMetricsFill(BC250_DEVICE *Device, BC250_DPM_METRICS *Out, BC250_DPM_CLOCKS *Clocks);
void SmuMetricsLogLine(BC250_DEVICE *Device, _In_z_ const char *What);
