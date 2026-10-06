// The board's hardware monitor, read only (docs/design/fan.md). The ASRock BC-250 carries a Nuvoton NCT6686D
// Super I/O. Its embedded controller turns the case fan from the BIOS "Fan Setting" curve (Standard Mode), and
// the owner keeps that setting as it is (2026-10-05). Until this file existed nothing in Windows could read the
// chip, so the control application said "The driver cannot read it yet" and a session record held no fan speed.
//
// What this file is, in one sentence: the fan value travels the road the GPU temperature already travels. The
// governor thread samples it once a second, publishes one snapshot under a spin lock, and every tool reads that
// snapshot through a software-only escape. Nothing else changes, and no decision inside the driver reads it.
//
// Where the parts live:
//   driver/shim/bc250_hwmon.c   the policy: the allowlist, the access sequence, the identity rules, the
//                               plausibility rules and the conversions. No OS call, host-tested.
//   this file                   the gate, the ports, the lock, the start, the sampler, the snapshot, the escape.
//   driver/kmd/dpm.c            the one caller of HwmonSample, in the governor thread at PASSIVE_LEVEL.
//
// Five rules this file keeps, each with a reason:
//
//  1. READ ONLY. The page and the index port of the EC window are written, because the window is an address
//     latch and a read of one register is four port accesses. Those are the only writes this reader issues.
//     They never go to a configuration, control, limit or duty register. bc250_hwmon_write_allowed() answers
//     "no" for every EC register, the host test asserts that the mock EC saw no write outside the two latch
//     ports, and tools/win/bc250rd/bc250rd_ioctl.h states the same rule for the SMN read that came before.
//
//  2. THE SUPER I/O CONFIGURATION PORTS 0x2E/0x2F ARE NEVER TOUCHED. The DSDT drives that pair itself, under
//     ACPI mutex \_SB.PCI0.SBRG.SIO1.MUT0, at every device-tree rescan and at every sleep and resume. A kernel
//     driver cannot take an ACPI mutex, and the enter/select/exit sequence has no abort. We do not need them:
//     the base is a constant on this board and the identity is readable from the EC window alone.
//
//  3. THE GATE IS CLOSED BY DEFAULT. The EC window has no arbiter: Windows gives the range to a PNP0C02
//     motherboard-resources node that drives nothing, and a third-party monitor drives the same window through
//     its own kernel helper. Two readers can interleave the latch writes and read each other's register, and
//     no lock of ours can prevent it. So EnableHwmon is 0 in the INF, every sample is checked for
//     plausibility, and a measured session needs the preflight check of docs/design/fan.md.
//
//  4. THE START NEVER FAILS BECAUSE OF THIS. MmioStart and HangDetectorStart set the precedent. A refusal sets
//     Online FALSE, writes one log line with every value it read, publishes an empty snapshot, and the desktop
//     runs. Nobody loses a screen over a fan sensor.
//
//  5. THE ESCAPE READS NO PORT. A HardwareAccess escape is a Level Two escape: it idles the GPU and stalls a
//     running game (BD-054, and the overlay's summary poll cost 300 ms in trial 41). RUN_HWMON copies the
//     published snapshot under SnapLock and nothing else, so it keeps NoAdapterSynchronization alone.
//
// Settings, REG_DWORD under Services\bc250kmd\Parameters, all closed or absent by every install:
//   EnableHwmon        0 (default) means no port access happens at all. 1 starts the reader.
//   HwmonBasePort      the EC base; 0 or absent means the built-in 0x0A20, measured on unit A.
//   HwmonExpectId      the pinned EC customer ID. Absent is stage 1: the driver reads the ID, logs it and
//                      publishes with ID_PINNED clear, so that an operator can read the value once and pin it.
//                      Present and different is a refusal: the reader never guesses which chip it found.
//   HwmonDutyProven    1 after a lab trial has shown the duty read-back follow the fan. It only opens the
//                      DUTY_PROVEN flag, which is what lets the control application show a percentage.
#include "bc250kmd.h"
#include "bc250kmd_escape.h"

#define HWMON_SETTING_ENABLE L"EnableHwmon"
#define HWMON_SETTING_BASE L"HwmonBasePort"
#define HWMON_SETTING_EXPECT_ID L"HwmonExpectId"
#define HWMON_SETTING_DUTY_PROVEN L"HwmonDutyProven"

static const char* const g_HwmonReason[] = { "ok", "gated", "base", "identity", "monitoring", "customer",
                                             "no-thread", "port" };
C_ASSERT(RTL_NUMBER_OF(g_HwmonReason) == BC250_HWMON_REASON_COUNT);

static const char* HwmonReasonText(ULONG reason)
{
    return reason < RTL_NUMBER_OF(g_HwmonReason) ? g_HwmonReason[reason] : "?";
}

// ---- the ports --------------------------------------------------------------------------------------------
//
// The only place in this driver that issues an x86 port access. The port index comes from the shim and is one
// of BC250_HWMON_PORT_*, so the address is this owner's own window by construction: a wrong index cannot name
// a port outside base + 4 .. base + 7, and nothing asks for the event port.

// The transport's own context: the owner and the IRQL of the hold. It lives on the caller's stack, so the
// saved IRQL is never shared between two callers.
typedef struct _BC250_HWMON_PORTS {
    BC250_HWMON_OWNER* Owner;
    KIRQL Irql;
} BC250_HWMON_PORTS;

static PUCHAR HwmonPort(const BC250_HWMON_PORTS* Ports, unsigned int Index)
{
    NT_ASSERT(Index < BC250_HWMON_PORT_COUNT);
    return (PUCHAR)(ULONG_PTR)(Ports->Owner->BasePort + 4u + (Index & (BC250_HWMON_PORT_COUNT - 1u)));
}

static void HwmonOut8(void* Context, unsigned int Index, unsigned char Value)
{
    BC250_HWMON_PORTS* ports = (BC250_HWMON_PORTS*)Context;
    // The latch ports only. The shim issues no other write, and this is the second place that says so.
    NT_ASSERT(Index == BC250_HWMON_PORT_PAGE || Index == BC250_HWMON_PORT_INDEX);
    if (Index != BC250_HWMON_PORT_PAGE && Index != BC250_HWMON_PORT_INDEX) return;
    WRITE_PORT_UCHAR(HwmonPort(ports, Index), Value);
}

static unsigned char HwmonIn8(void* Context, unsigned int Index)
{
    BC250_HWMON_PORTS* ports = (BC250_HWMON_PORTS*)Context;
    return READ_PORT_UCHAR(HwmonPort(ports, Index));
}

// Both Linux drivers use the outb_p/inb_p forms for this window, which add the old ISA pause. Neither polls a
// busy bit, and the chip publishes none, so there is nothing else to wait for. The cost of a port access on
// this board's LPC bus is not measured yet; the first lab trial logs it, and this stall stays until it does.
static void HwmonPause(void* Context)
{
    UNREFERENCED_PARAMETER(Context);
    KeStallExecutionProcessor(1);
}

static void HwmonLockPorts(void* Context)
{
    BC250_HWMON_PORTS* ports = (BC250_HWMON_PORTS*)Context;
    KeAcquireSpinLock(&ports->Owner->PortLock, &ports->Irql);
}

static void HwmonUnlockPorts(void* Context)
{
    BC250_HWMON_PORTS* ports = (BC250_HWMON_PORTS*)Context;
    KeReleaseSpinLock(&ports->Owner->PortLock, ports->Irql);
}

static void HwmonIo(BC250_HWMON_PORTS* Ports, BC250_HWMON_OWNER* Owner, struct bc250_hwmon_io* Io)
{
    RtlZeroMemory(Ports, sizeof(*Ports));
    Ports->Owner = Owner;
    RtlZeroMemory(Io, sizeof(*Io));
    Io->context = Ports;
    Io->out8 = HwmonOut8;
    Io->in8 = HwmonIn8;
    Io->pause = HwmonPause;
    Io->lock = HwmonLockPorts;
    Io->unlock = HwmonUnlockPorts;
}

// ---- the published snapshot -------------------------------------------------------------------------------

static void HwmonPublish(BC250_DEVICE* Device, const struct bc250_hwmon_sample* Sample, ULONGLONG At)
{
    BC250_HWMON_OWNER* owner = &Device->Hwmon;
    BC250_HWMON_SNAP snap;
    ULONG i, duty = 0, turning = 0;
    KIRQL irql;

    RtlZeroMemory(&snap, sizeof(snap));
    snap.Reason = owner->Reason;
    snap.Generation = Device->StartHealth.Generation;
    snap.Samples = owner->Samples;
    snap.Errors = owner->Errors;
    snap.Retries = owner->Retries;
    if (!owner->Enabled) snap.Flags |= BC250_HWMON_FLAG_GATED;
    if (owner->Online) {
        snap.Flags |= BC250_HWMON_FLAG_VALID;
        if (owner->Identity.monitoring) snap.Flags |= BC250_HWMON_FLAG_MONITORING;
        if (owner->IdPinned) snap.Flags |= BC250_HWMON_FLAG_ID_PINNED;
        if (owner->DutyProven) snap.Flags |= BC250_HWMON_FLAG_DUTY_PROVEN;
        snap.BasePort = owner->BasePort;
        snap.CustomerId = owner->Identity.customer_id;
        snap.EcVersion = owner->Identity.version;
        snap.EcBuild = owner->Identity.build;
        snap.FanPresentMask = owner->Identity.fan_present;
        snap.DutyPresentMask = owner->Identity.duty_present;
    }
    if (Sample != NULL) {
        snap.ModeMask = Sample->mode_mask;
        snap.Engine = Sample->engine;
        for (i = 0; i < BC250_HWMON_FAN_MAX; i++) {
            if ((Sample->rpm_valid & (1u << i)) != 0u) {
                snap.Rpm[i] = Sample->rpm[i];
                if (Sample->rpm[i] != 0u) turning++;
            }
            if ((Sample->duty_valid & (1u << i)) != 0u) {
                snap.DutyPermille[i] = bc250_hwmon_duty_permille(Sample->duty[i]);
                if (snap.DutyPermille[i] > duty) duty = snap.DutyPermille[i];
            }
        }
        for (i = 0; i < BC250_HWMON_TEMP_MAX; i++) {
            if ((Sample->temperature_valid & (1u << i)) == 0u) continue;
            snap.TemperatureMc[i] = Sample->temperature_mc[i];
            snap.TemperatureSource[i] = owner->Identity.source[i];
        }
        snap.SampleAt = At;
        // A duty output runs and nothing turns. That is the one case the owner must see at a glance, and it is
        // what the camera fire watch looks for as well.
        if (duty != 0u && turning == 0u) snap.Flags |= BC250_HWMON_FLAG_STOPPED;
    }
    KeAcquireSpinLock(&owner->SnapLock, &irql);
    owner->Snap = snap;
    KeReleaseSpinLock(&owner->SnapLock, irql);
}

// ---- the driver log --------------------------------------------------------------------------------------

// Its own line, like DpmLogIdleLine: the DPM telemetry line is already near the log's 160 bytes. Only while
// the reader is online, so a machine with the gate closed logs exactly what the revision before this one did.
void HwmonLogLine(BC250_DEVICE* Device, _In_z_ const char* What)
{
    BC250_HWMON_OWNER* owner = &Device->Hwmon;
    BC250_HWMON_SNAP snap;
    KIRQL irql;
    ULONG i, fan = 0, rpm = 0, duty = 0, turning = 0, present = 0;
    LONG apu = 0;

    if (!owner->Enabled) return;
    KeAcquireSpinLock(&owner->SnapLock, &irql);
    snap = owner->Snap;
    KeReleaseSpinLock(&owner->SnapLock, irql);
    if ((snap.Flags & BC250_HWMON_FLAG_VALID) == 0u) {
        GuardLog("hwmon: %s offline, reason %s (base 0x%04lX, ec 0x%04lX build %02lu/%02lu/%02lu, fans 0x%02lX)",
                 What, HwmonReasonText(snap.Reason), snap.BasePort, snap.EcVersion,
                 (snap.EcBuild >> 8) & 0xFFu, snap.EcBuild & 0xFFu, (snap.EcBuild >> 16) & 0xFFu,
                 snap.FanPresentMask);
        return;
    }
    for (i = 0; i < BC250_HWMON_FAN_MAX; i++) {
        if ((snap.FanPresentMask & (1u << i)) != 0u) present++;
        if (snap.Rpm[i] == 0u) continue;
        turning++;
        if (snap.Rpm[i] > rpm) { rpm = snap.Rpm[i]; fan = i; }
    }
    for (i = 0; i < BC250_HWMON_FAN_MAX; i++)
        if (snap.DutyPermille[i] > duty) duty = snap.DutyPermille[i];
    for (i = 0; i < BC250_HWMON_TEMP_MAX; i++)
        if (snap.TemperatureSource[i] == BC250_HWMON_SOURCE_APU) apu = snap.TemperatureMc[i];
    GuardLog("hwmon: %s fan%lu %lu rpm (%lu/%lu turn) duty %lu permille%s mode 0x%02lX eng 0x%02lX apu %ld.%01ld C, "
             "%llu samples %llu errors %llu retries",
             What, fan + 1u, rpm, turning, present, duty, owner->DutyProven ? "" : " unproven",
             snap.ModeMask, snap.Engine, apu / 1000, (apu < 0 ? -apu : apu) % 1000 / 100,
             snap.Samples, snap.Errors, snap.Retries);
}

// ---- life cycle -----------------------------------------------------------------------------------------

// AddDevice, before publication. The spin locks and a zeroed snapshot, nothing else: no port is touched until
// the gate has been read at StartDevice.
void HwmonInitialize(BC250_HWMON_OWNER* Owner)
{
    RtlZeroMemory(Owner, sizeof(*Owner));
    KeInitializeSpinLock(&Owner->PortLock);
    KeInitializeSpinLock(&Owner->SnapLock);
    Owner->Reason = BC250_HWMON_REASON_GATED;
}

// StartDevice, PASSIVE_LEVEL. Never fails the start: a refusal is one log line and an offline reader.
void HwmonStart(BC250_DEVICE* Device)
{
    BC250_HWMON_OWNER* owner = &Device->Hwmon;
    BC250_HWMON_PORTS ports;
    struct bc250_hwmon_io io;
    ULONG base, expect = 0;
    BOOLEAN expectPresent;
    int status;

    owner->Online = FALSE;
    owner->LastValid = FALSE;
    owner->FailuresInRow = 0;
    owner->Samples = owner->Errors = owner->Retries = 0;
    RtlZeroMemory(&owner->Identity, sizeof(owner->Identity));
    RtlZeroMemory(&owner->Last, sizeof(owner->Last));
    owner->Enabled = GuardReadSetting(HWMON_SETTING_ENABLE, 0) == 1;
    if (!owner->Enabled) {
        owner->Reason = BC250_HWMON_REASON_GATED;
        owner->BasePort = 0;
        HwmonPublish(Device, NULL, 0);
        GuardLog("hwmon: off (EnableHwmon 0): the board's monitor is not read, no port access happens");
        return;
    }
    base = GuardReadSetting(HWMON_SETTING_BASE, 0);
    if (base == 0) base = BC250_HWMON_BASE_DEFAULT;
    owner->BasePort = base;
    owner->DutyProven = GuardReadSetting(HWMON_SETTING_DUTY_PROVEN, 0) == 1;
    expectPresent = NT_SUCCESS(GuardQuerySetting(HWMON_SETTING_EXPECT_ID, &expect));
    if (!bc250_hwmon_base_allowed(base)) {
        owner->Reason = BC250_HWMON_REASON_BASE;
        owner->BasePort = 0;
        HwmonPublish(Device, NULL, 0);
        GuardLog("hwmon: HwmonBasePort 0x%04lX is not a window this chip can sit on: reader off", base);
        return;
    }
    HwmonIo(&ports, owner, &io);
    status = bc250_hwmon_identify(&io, base, &owner->Identity);
    // One line with every value it read, whatever the answer: this is the line an operator pins the customer
    // ID from, and the line that says why a refusal happened.
    GuardLog("hwmon: base 0x%04lX ec %lu.%lu build %02lu/%02lu/%02lu customer 0x%04lX cfg 0x%02lX fans 0x%02lX "
             "duties 0x%02lX temps %lu -> %s",
             base, (ULONG)(owner->Identity.version >> 8), (ULONG)(owner->Identity.version & 0xFFu),
             (ULONG)((owner->Identity.build >> 8) & 0xFFu), (ULONG)(owner->Identity.build & 0xFFu),
             (ULONG)((owner->Identity.build >> 16) & 0xFFu), (ULONG)owner->Identity.customer_id,
             (ULONG)owner->Identity.cfg, (ULONG)owner->Identity.fan_present,
             (ULONG)owner->Identity.duty_present, (ULONG)owner->Identity.temperatures,
             HwmonReasonText(owner->Identity.reason));
    if (status != 0) {
        owner->Reason = owner->Identity.reason;
        HwmonPublish(Device, NULL, 0);
        return;
    }
    if (expectPresent && expect != owner->Identity.customer_id) {
        // The pinned chip is not the chip that answered. The reader does not guess.
        owner->Reason = BC250_HWMON_REASON_CUSTOMER;
        GuardLog("hwmon: HwmonExpectId 0x%04lX does not match the customer ID 0x%04lX: reader off", expect,
                 (ULONG)owner->Identity.customer_id);
        HwmonPublish(Device, NULL, 0);
        return;
    }
    owner->IdPinned = expectPresent;
    owner->Reason = BC250_HWMON_REASON_OK;
    owner->Online = TRUE;
    if (!expectPresent)
        GuardLog("hwmon: customer ID 0x%04lX is not pinned (HwmonExpectId absent): pin it after this trial",
                 (ULONG)owner->Identity.customer_id);
    HwmonPublish(Device, NULL, 0);
}

// StopDevice, after DpmStop, so the governor thread that samples is already gone.
void HwmonStop(BC250_HWMON_OWNER* Owner)
{
    BC250_HWMON_SNAP snap;
    KIRQL irql;

    if (!Owner->Enabled) return;
    Owner->Online = FALSE;
    KeAcquireSpinLock(&Owner->SnapLock, &irql);
    snap = Owner->Snap;
    // VALID alone: the published snapshot never carries FRESH, which HwmonRequest computes from the age and
    // now withholds without VALID. The numbers stay, so a support report taken after a stop still has them.
    Owner->Snap.Flags &= ~BC250_HWMON_FLAG_VALID;
    KeReleaseSpinLock(&Owner->SnapLock, irql);
    GuardLog("hwmon: stop after %llu samples, %llu errors, %llu retries", snap.Samples, snap.Errors, snap.Retries);
}

// ---- the sampler ----------------------------------------------------------------------------------------
//
// Called from DpmTick, in the governor's own thread at PASSIVE_LEVEL, once every BC250_HWMON_PERIOD_MS. Not
// from DpmHwSample, which is an EX_TIMER callback at DISPATCH_LEVEL: the port sequence wants a thread.
//
// The reader does not restart itself inside one device start. After BC250_HWMON_FAIL_LIMIT failed samples in a
// row it goes offline with reason "port" and says so once. A self-restarting loop on a window that has no
// arbiter is the wrong answer: it would keep writing the latch of a chip somebody else is talking to.
void HwmonSample(BC250_DEVICE* Device)
{
    BC250_HWMON_OWNER* owner = &Device->Hwmon;
    BC250_HWMON_PORTS ports;
    struct bc250_hwmon_io io;
    struct bc250_hwmon_sample sample;
    ULONGLONG now;
    int status;

    if (!owner->Enabled || !owner->Online) return;
    HwmonIo(&ports, owner, &io);
    status = bc250_hwmon_sample(&io, &owner->Identity, owner->LastValid ? &owner->Last : NULL, &sample);
    owner->Retries += sample.retries;
    now = KeQueryInterruptTime();
    if (status != 0) {
        owner->Errors++;
        owner->FailuresInRow++;
        if (owner->FailuresInRow >= BC250_HWMON_FAIL_LIMIT) {
            owner->Online = FALSE;
            owner->Reason = BC250_HWMON_REASON_PORT;
            GuardLog("hwmon: %lu samples in a row were refused: reader off for this start (%llu errors)",
                     owner->FailuresInRow, owner->Errors);
        }
        // The published snapshot keeps its last accepted values; AgeMs then grows, and FRESH drops by itself.
        HwmonPublish(Device, owner->LastValid ? &owner->Last : NULL, owner->LastAt);
        return;
    }
    owner->FailuresInRow = 0;
    owner->Samples++;
    owner->Last = sample;
    owner->LastValid = TRUE;
    owner->LastAt = now;
    HwmonPublish(Device, &sample, now);
}

// ---- the escape -----------------------------------------------------------------------------------------
//
// BC250_ESCAPE_RUN_HWMON. Software state only, so NoAdapterSynchronization=1 and nothing else, exactly as
// DpmRequest. READ is the only operation and needs no administrator, as RUN_DPM READ does not. No port is
// touched here: that is the whole point of publishing a snapshot.
void HwmonRequest(BC250_DEVICE* Device, BC250_ESCAPE_HWMON* Data, ULONG EscapeFlags)
{
    BC250_HWMON_OWNER* owner = &Device->Hwmon;
    D3DDDI_ESCAPEFLAGS expectedFlags = {0};
    BC250_HWMON_SNAP snap;
    ULONGLONG now;
    ULONG i;
    KIRQL irql;

    expectedFlags.NoAdapterSynchronization = 1;
    Data->Version = BC250_KMD_VERSION;
    Data->Status = BC250_ESCAPE_STATUS_REFUSED;
    Data->NtStatus = (ULONG)STATUS_INVALID_PARAMETER;
    Data->Flags = Data->BasePort = Data->CustomerId = Data->EcVersion = Data->EcBuild = 0;
    Data->FanPresentMask = Data->DutyPresentMask = Data->ModeMask = 0;
    RtlZeroMemory(Data->Rpm, sizeof(Data->Rpm));
    RtlZeroMemory(Data->DutyPermille, sizeof(Data->DutyPermille));
    RtlZeroMemory(Data->TemperatureMc, sizeof(Data->TemperatureMc));
    RtlZeroMemory(Data->TemperatureSource, sizeof(Data->TemperatureSource));
    Data->AgeMs = Data->Reason = Data->Reserved = 0;
    Data->Samples = Data->Errors = Data->Retries = Data->Generation = 0;
    if (Data->AbiVersion != BC250_HWMON_ABI || Data->Op != BC250_HWMON_OP_READ ||
        EscapeFlags != expectedFlags.Value) return;
    if (!ExAcquireRundownProtection(&Device->StartHealth.Readers)) {
        Data->NtStatus = (ULONG)STATUS_DELETE_PENDING;
        return;
    }
    KeAcquireSpinLock(&owner->SnapLock, &irql);
    snap = owner->Snap;
    KeReleaseSpinLock(&owner->SnapLock, irql);
    ExReleaseRundownProtection(&Device->StartHealth.Readers);
    // The age is taken here and not by the sampler, so that a stopped sampler shows an age that grows.
    now = KeQueryInterruptTime();
    Data->Flags = snap.Flags;
    // ">=" and not ">": the interrupt time moves in whole clock ticks (about 15.6 ms), so a reader that asks
    // right after a sample sees the same tick, and an age of zero is the freshest reading there is.
    if (snap.SampleAt != 0 && now >= snap.SampleAt) {
        ULONGLONG age = (now - snap.SampleAt) / 10000ull;
        Data->AgeMs = age > MAXULONG ? MAXULONG : (ULONG)age;
        // FRESH qualifies a reading, so it needs VALID beside it. After a stop, or after the sampler gave up,
        // the age of the last accepted sample is still worth reporting - but there is no reading any more, and
        // a reader that only looked at FRESH would keep showing a fan speed that nothing stands behind.
        if (Data->AgeMs <= BC250_HWMON_FRESH_MS && (Data->Flags & BC250_HWMON_FLAG_VALID) != 0)
            Data->Flags |= BC250_HWMON_FLAG_FRESH;
    }
    Data->Reason = snap.Reason;
    Data->BasePort = snap.BasePort;
    Data->CustomerId = snap.CustomerId;
    Data->EcVersion = snap.EcVersion;
    Data->EcBuild = snap.EcBuild;
    Data->FanPresentMask = snap.FanPresentMask;
    Data->DutyPresentMask = snap.DutyPresentMask;
    Data->ModeMask = snap.ModeMask;
    for (i = 0; i < BC250_HWMON_FAN_SLOTS; i++) {
        Data->Rpm[i] = snap.Rpm[i];
        Data->DutyPermille[i] = snap.DutyPermille[i];
    }
    for (i = 0; i < BC250_HWMON_TEMP_SLOTS; i++) {
        Data->TemperatureMc[i] = snap.TemperatureMc[i];
        Data->TemperatureSource[i] = snap.TemperatureSource[i];
    }
    Data->Samples = snap.Samples;
    Data->Errors = snap.Errors;
    Data->Retries = snap.Retries;
    Data->Generation = snap.Generation;
    Data->Status = BC250_ESCAPE_STATUS_DONE;
    Data->NtStatus = (ULONG)STATUS_SUCCESS;
}
