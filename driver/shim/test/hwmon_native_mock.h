/* Compiles the REAL driver/kmd/hwmon.c on the host: run_hwmon.ps1 replaces its #include "bc250kmd.h" with this
 * header, which redirects READ_PORT_UCHAR and WRITE_PORT_UCHAR to the EC model (hwmon_ec_mock.h) and stubs the
 * handful of kernel primitives the file uses. The binding code is then tested, not a copy of it: the gate, the
 * identity refusals, the retry and give-up rules, the snapshot and the escape are the ones that ship.
 *
 * The same pattern as smu_native_mock.h, for the same reason.
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
#define NT_ASSERT(x) CHECK(x)
#ifndef MAXULONG
#define MAXULONG 0xffffffffUL
#endif

typedef UCHAR KIRQL;
typedef ULONG_PTR KSPIN_LOCK;
typedef KSPIN_LOCK *PKSPIN_LOCK;
typedef LONG EX_RUNDOWN_REF;

/* D3DDDI_ESCAPEFLAGS, field for field as d3dukmdt.h declares it. The test asserts that
 * NoAdapterSynchronization is still bit 3, so a drift in the real header cannot pass unseen here. */
typedef union _D3DDDI_ESCAPEFLAGS {
    struct {
        UINT HardwareAccess : 1;
        UINT DeviceStatusQuery : 1;
        UINT ChangeFrameLatency : 1;
        UINT NoAdapterSynchronization : 1;
        UINT Reserved : 1;
        UINT VirtualMachineData : 1;
        UINT DriverKnownEscape : 1;
        UINT DriverCommonEscape : 1;
        UINT Reserved2 : 24;
    };
    UINT Value;
} D3DDDI_ESCAPEFLAGS;

/* ---- the chip model ------------------------------------------------------------------------------------- */

#include "hwmon_ec_mock.h"
static struct ec_mock native_ec;

/* The window the model answers on. hwmon.c computes base + 4 + index itself, so the test sets native_base to the
 * base it configured and any other address is counted instead of answered: the driver is then proved to use the
 * base it was given, and a stray port access anywhere else is visible. */
static unsigned int native_base = BC250_HWMON_BASE_DEFAULT;
static unsigned int native_ports_outside;

static unsigned int NativePortIndex(PUCHAR port)
{
    ULONG_PTR address = (ULONG_PTR)port;

    if (address < (ULONG_PTR)native_base + 4u || address > (ULONG_PTR)native_base + 7u) {
        native_ports_outside++;
        return BC250_HWMON_PORT_EVENT;      /* the model counts a sequence error for this one */
    }
    return (unsigned int)(address - ((ULONG_PTR)native_base + 4u));
}

static UCHAR NativeReadPort(PUCHAR port) { return ec_in8(&native_ec, NativePortIndex(port)); }
static void NativeWritePort(PUCHAR port, UCHAR value) { ec_out8(&native_ec, NativePortIndex(port), value); }
#define READ_PORT_UCHAR NativeReadPort
#define WRITE_PORT_UCHAR NativeWritePort

/* ---- kernel primitives ---------------------------------------------------------------------------------- */

static ULONG64 native_time;                 /* KeQueryInterruptTime units, 100 ns */
static unsigned int native_stalls;

/* Only the port lock bounds a register transaction. The snapshot lock must NOT look like a hold to the model,
 * or a port access made under the wrong lock would pass unseen. The test assigns this once. */
static PKSPIN_LOCK native_port_lock;

static void KeInitializeSpinLock(PKSPIN_LOCK lock) { *lock = 0; }

static void KeAcquireSpinLock(PKSPIN_LOCK lock, KIRQL *irql)
{
    CHECK(*lock == 0);                      /* a leaf lock, held once: no recursion, no lock order to change */
    *lock = 1;
    *irql = 2;                              /* DISPATCH_LEVEL */
    if (lock == native_port_lock) ec_lock(&native_ec);
}

static void KeReleaseSpinLock(PKSPIN_LOCK lock, KIRQL irql)
{
    CHECK(*lock == 1 && irql == 2);
    *lock = 0;
    if (lock == native_port_lock) ec_unlock(&native_ec);
}

static ULONG64 KeQueryInterruptTime(void) { return native_time; }
static void KeStallExecutionProcessor(unsigned usec) { CHECK(usec == 1); native_stalls++; }

/* The rundown protection of the adapter context. The test clears native_rundown to prove that the escape
 * answers STATUS_DELETE_PENDING instead of reading the snapshot during a tear-down. */
static int native_rundown = 1;
static int native_rundown_held;

static BOOLEAN ExAcquireRundownProtection(EX_RUNDOWN_REF *ref)
{
    UNREFERENCED_PARAMETER(ref);
    if (!native_rundown) return FALSE;
    native_rundown_held++;
    return TRUE;
}

static void ExReleaseRundownProtection(EX_RUNDOWN_REF *ref)
{
    UNREFERENCED_PARAMETER(ref);
    native_rundown_held--;
}

/* The driver log and the registry, as much of them as hwmon.c uses. */
#define NATIVE_LOG_LINES 64
static char native_log[NATIVE_LOG_LINES][256];
static int native_lines;

static void GuardLog(_In_z_ const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    if (native_lines < NATIVE_LOG_LINES) {
        vsnprintf(native_log[native_lines], sizeof(native_log[0]), format, arguments);
        /* The driver's ring truncates at BC250_LOG_TEXT (160 bytes), so a line that does not fit there would
         * lose its tail on the lab. Caught here instead of on the machine. */
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

#define NATIVE_SETTINGS 8
static struct { const WCHAR *Name; ULONG Value; } native_settings[NATIVE_SETTINGS];

static void NativeSetSetting(const WCHAR *name, ULONG value)
{
    int i;

    for (i = 0; i < NATIVE_SETTINGS; i++)
        if (native_settings[i].Name == NULL || wcscmp(native_settings[i].Name, name) == 0) {
            native_settings[i].Name = name;
            native_settings[i].Value = value;
            return;
        }
    CHECK(0);
}

static void NativeClearSettings(void) { memset(native_settings, 0, sizeof(native_settings)); }

static NTSTATUS GuardQuerySetting(const WCHAR *name, ULONG *value)
{
    int i;

    for (i = 0; i < NATIVE_SETTINGS; i++)
        if (native_settings[i].Name != NULL && wcscmp(native_settings[i].Name, name) == 0) {
            *value = native_settings[i].Value;
            return STATUS_SUCCESS;
        }
    return STATUS_OBJECT_NAME_NOT_FOUND;
}

static ULONG GuardReadSetting(const WCHAR *name, ULONG fallback)
{
    ULONG value = 0;

    return NT_SUCCESS(GuardQuerySetting(name, &value)) ? value : fallback;
}

/* ---- the device ----------------------------------------------------------------------------------------- */

#include "bc250kmd_escape.h"
#include "hwmon.h"

/* Only the two fields hwmon.c reaches for. The real BC250_START_HEALTH carries much more, and none of it
 * belongs in a fan test. */
typedef struct _BC250_START_HEALTH_NATIVE {
    EX_RUNDOWN_REF Readers;
    ULONGLONG Generation;
} BC250_START_HEALTH_NATIVE;

typedef struct _BC250_DEVICE {
    BC250_START_HEALTH_NATIVE StartHealth;
    BC250_HWMON_OWNER Hwmon;
} BC250_DEVICE;

void HwmonInitialize(BC250_HWMON_OWNER *Owner);
void HwmonStart(BC250_DEVICE *Device);
void HwmonStop(BC250_HWMON_OWNER *Owner);
void HwmonSample(BC250_DEVICE *Device);
void HwmonLogLine(BC250_DEVICE *Device, _In_z_ const char *What);
void HwmonRequest(BC250_DEVICE *Device, BC250_ESCAPE_HWMON *Data, ULONG EscapeFlags);
