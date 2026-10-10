// The board's hardware monitor as the miniport keeps it (hwmon.c, docs/design/fan.md): the EC window, the
// identity the start proved, the sampler's own state and the published snapshot. The policy is
// driver/shim/bc250_hwmon.c, which has no Windows in it; this is the part that does.
#pragma once
#include "bc250_hwmon.h"
// For BC250_HWMON_FAN_SLOTS and BC250_HWMON_TEMP_SLOTS, which the two asserts below compare with the shim's
// own counts. Included here and not left to the including file: bc250kmd.h reaches the escape header through
// paging_identity.h today, and one include reorder would turn these asserts into a build error.
#include "bc250kmd_escape.h"

#define BC250_HWMON_PERIOD_MS 1000u          // the chip caches its registers for about a second
#define BC250_HWMON_STALE_SAMPLES 3u         // missed samples after which the reading is called stale
#define BC250_HWMON_FRESH_MS (BC250_HWMON_PERIOD_MS * BC250_HWMON_STALE_SAMPLES)
#define BC250_HWMON_FAIL_LIMIT 10u           // failed samples in a row before the reader gives up
#define BC250_HWMON_STOPPED_SAMPLES 3u       // samples in a row with a duty and nothing turning before STOPPED
// How long a start may publish VALID with no sample at all before the reader says NO_THREAD. The sampler is
// the DPM governor thread, which a start without the native SMU owner never creates; the reader cannot see
// that from here, so it waits one freshness window and then names it.
#define BC250_HWMON_NO_THREAD_MS BC250_HWMON_FRESH_MS

C_ASSERT(BC250_HWMON_FAN_SLOTS == BC250_HWMON_FAN_MAX);
C_ASSERT(BC250_HWMON_TEMP_SLOTS == BC250_HWMON_TEMP_MAX);

typedef struct _BC250_HWMON_SNAP {
    ULONG Flags;                            // BC250_HWMON_FLAG_*
    ULONG Reason;                           // enum bc250_hwmon_reason
    ULONG BasePort, CustomerId, EcVersion, EcBuild;
    ULONG FanPresentMask, DutyPresentMask, ModeMask, Engine;
    ULONG RpmValidMask, DutyValidMask;       // bit i: this sample accepted that channel's value
    ULONG Rpm[BC250_HWMON_FAN_MAX];
    ULONG DutyPermille[BC250_HWMON_FAN_MAX];
    LONG TemperatureMc[BC250_HWMON_TEMP_MAX];
    ULONG TemperatureSource[BC250_HWMON_TEMP_MAX];
    ULONGLONG SampleAt;                     // KeQueryInterruptTime of the last accepted sample, 0 for none
    ULONGLONG Samples, Errors, Retries, Refusals;
    ULONGLONG Generation;                   // start-health generation of this start
} BC250_HWMON_SNAP;

typedef struct _BC250_HWMON_OWNER {
    // One register transaction. A leaf lock: the holder takes nothing else, so no lock order changes.
    KSPIN_LOCK PortLock;
    KSPIN_LOCK SnapLock;                    // Snap, written by the sampler and read by the escape
    BOOLEAN BoardAllowed;                  // Positive board selection precedes even probe latch writes.
    BOOLEAN Enabled;                        // EnableHwmon was 1 at this start
    BOOLEAN Online;                         // the identity passed: the sampler may read
    BOOLEAN DutyProven;                     // HwmonDutyProven
    BOOLEAN IdPinned;                       // HwmonExpectId was present and matched
    ULONG BasePort;
    ULONG Reason;                           // why not online, or BC250_HWMON_REASON_OK
    ULONG FailuresInRow;
    ULONG StoppedInRow;                     // accepted samples in a row with a duty and nothing turning
    ULONG LoggedReason;                     // the reason the offline log line last named
    BOOLEAN LoggedOffline;                  // and whether that line has been written at all in this start
    ULONGLONG StartedAt;                    // KeQueryInterruptTime of this start: the NO_THREAD grace window
    struct bc250_hwmon_identity Identity;
    // The sampler's own, touched only by the governor thread: the previous sample feeds the jump rule, and
    // LastAt keeps its time so that a failed sample can republish the last accepted one with its own age.
    struct bc250_hwmon_sample Last;
    ULONGLONG LastAt;
    BOOLEAN LastValid;
    ULONGLONG Samples, Errors, Retries, Refusals;
    BC250_HWMON_SNAP Snap;                  // under SnapLock
} BC250_HWMON_OWNER;

// The port transport's own context: the owner and the IRQL of the hold. It lives on the caller's stack, so the
// saved IRQL is never shared between two callers.
typedef struct _BC250_HWMON_PORTS {
    BC250_HWMON_OWNER* Owner;
    KIRQL Irql;
} BC250_HWMON_PORTS;

// The fan control's transport (fan.c, Part B of docs/design/fan.md): the read transport plus the data port and a
// microsecond stall for the handshake's polls. Lockless is for the bugcheck path only, where the other processors
// are frozen and a spin lock could be held by one of them forever.
void HwmonWriteIo(BC250_HWMON_PORTS* Ports, BC250_HWMON_OWNER* Owner, struct bc250_hwmon_io* Io, BOOLEAN Lockless);
