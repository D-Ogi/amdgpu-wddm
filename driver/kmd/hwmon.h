// The board's hardware monitor as the miniport keeps it (hwmon.c, docs/design/fan.md): the EC window, the
// identity the start proved, the sampler's own state and the published snapshot. The policy is
// driver/shim/bc250_hwmon.c, which has no Windows in it; this is the part that does.
#pragma once
#include "bc250_hwmon.h"

#define BC250_HWMON_PERIOD_MS 1000u          // the chip caches its registers for about a second
#define BC250_HWMON_STALE_SAMPLES 3u         // missed samples after which the reading is called stale
#define BC250_HWMON_FRESH_MS (BC250_HWMON_PERIOD_MS * BC250_HWMON_STALE_SAMPLES)
#define BC250_HWMON_FAIL_LIMIT 10u           // failed samples in a row before the reader gives up

C_ASSERT(BC250_HWMON_FAN_SLOTS == BC250_HWMON_FAN_MAX);
C_ASSERT(BC250_HWMON_TEMP_SLOTS == BC250_HWMON_TEMP_MAX);

typedef struct _BC250_HWMON_SNAP {
    ULONG Flags;                            // BC250_HWMON_FLAG_*
    ULONG Reason;                           // enum bc250_hwmon_reason
    ULONG BasePort, CustomerId, EcVersion, EcBuild;
    ULONG FanPresentMask, DutyPresentMask, ModeMask, Engine;
    ULONG Rpm[BC250_HWMON_FAN_MAX];
    ULONG DutyPermille[BC250_HWMON_FAN_MAX];
    LONG TemperatureMc[BC250_HWMON_TEMP_MAX];
    ULONG TemperatureSource[BC250_HWMON_TEMP_MAX];
    ULONGLONG SampleAt;                     // KeQueryInterruptTime of the last accepted sample, 0 for none
    ULONGLONG Samples, Errors, Retries;
    ULONGLONG Generation;                   // start-health generation of this start
} BC250_HWMON_SNAP;

typedef struct _BC250_HWMON_OWNER {
    // One register transaction. A leaf lock: the holder takes nothing else, so no lock order changes.
    KSPIN_LOCK PortLock;
    KSPIN_LOCK SnapLock;                    // Snap, written by the sampler and read by the escape
    BOOLEAN Enabled;                        // EnableHwmon was 1 at this start
    BOOLEAN Online;                         // the identity passed: the sampler may read
    BOOLEAN DutyProven;                     // HwmonDutyProven
    BOOLEAN IdPinned;                       // HwmonExpectId was present and matched
    ULONG BasePort;
    ULONG Reason;                           // why not online, or BC250_HWMON_REASON_OK
    ULONG FailuresInRow;
    struct bc250_hwmon_identity Identity;
    // The sampler's own, touched only by the governor thread: the previous sample feeds the jump rule, and
    // LastAt keeps its time so that a failed sample can republish the last accepted one with its own age.
    struct bc250_hwmon_sample Last;
    ULONGLONG LastAt;
    BOOLEAN LastValid;
    ULONGLONG Samples, Errors, Retries;
    BC250_HWMON_SNAP Snap;                  // under SnapLock
} BC250_HWMON_OWNER;
