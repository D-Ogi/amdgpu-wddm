# The case fan

Unit A has one case fan. The board turns it, not our driver. A Nuvoton NCT6686D hardware monitor on the
board holds the fan curve, and the BIOS "Fan Setting" option selects which curve it uses. The owner keeps
that option as it is (2026-10-05).

Until KMD 0.7.213.1 nothing in Windows could read that chip. The control application showed "The driver
cannot read it yet" in its Fan row, and a lab session record held no fan speed. This page is the design of
the read path, which KMD 0.7.213.1 adds. The path was written as revision 208 on `fan/read-nct6686` and
train b20 merged it into the lineage that ships, so the released 0.7.208.1 does not have it.

**Part B, the write path, is NOT implemented.** The last section says what it would be, and why it waits
for the owner.

## What the chip is

| Item | Value | Where it comes from |
| --- | --- | --- |
| Chip | Nuvoton NCT6686D | [M796](../facts/hardware.md#m796) |
| EC window | `0x0A20` to `0x0A2F` | `ioports.txt` of E01, and the DSDT `IO3B` resource |
| Tachometer channels | 5, one with a fan on it | E01: `fan2` at 1589 RPM, the others at 0 |
| Duty outputs | 5, each reading 245 of 255 | E01 `pwm1` to `pwm5` |
| Temperature channels | 3: the APU over SB-TSI, two board thermistors | E01: 83.0 C, 59.5 C, 59.5 C |
| EC firmware | 1.0, built 2021-07-28 | read at the first start on the lab |

Register numbers and behaviour are facts from Linux mainline `drivers/hwmon/nct6683.c`, which binds on this
chip, and from the out-of-tree `nct6687d` where a row says UNPROVEN. We copy no code from either.

The chip has two access paths:

- The Super I/O configuration pair `0x2E`/`0x2F`. **We never use it.** The DSDT drives the same pair itself,
  under ACPI mutex `\_SB.PCI0.SBRG.SIO1.MUT0`, at every device-tree rescan and at every sleep and resume. A
  kernel driver cannot take an ACPI mutex, and the enter/select/exit sequence has no abort. We do not need
  it: the base is a constant on this board, and the chip identity is readable from the EC window alone.
- The EC window itself, four ports at base + 4: page, index, data and event. We use three of them.

One 8-bit read of EC register `reg` is four port accesses:

```
write 0xFF        -> page     (the unlock value)
write reg >> 8    -> page
write reg & 0xFF  -> index
read              <- data
```

A 16-bit read is two such reads, the high byte at `reg` and the low byte at `reg + 1`, inside one lock
hold. The index latch must not move between the two halves.

**Those are the only writes the read path makes.** They go to the chip's own address latch, never to a
configuration, control, limit or duty register. `bc250_hwmon_write_allowed()` answers "no" for every EC
register, and both host tests assert that the model of the chip saw no write outside the two latch ports.

## Where the parts are

| File | What it holds |
| --- | --- |
| `driver/shim/include/bc250_hwmon.h` | the contract: ports, registers, bounds, reasons |
| `driver/shim/bc250_hwmon.c` | the policy: the allowlist, the access sequence, the identity rules, the plausibility rules, the conversions. No OS call |
| `driver/kmd/hwmon.h` | the miniport's own state: the EC window, the identity, the sampler, the published snapshot |
| `driver/kmd/hwmon.c` | the gate, the ports, the lock, the start, the sampler, the snapshot, the escape |
| `driver/kmd/dpm.c` | the one caller of `HwmonSample`, in the governor thread |
| `tools/win/bc250kmd_cli/bc250kmd_cli.c` | `Bc250Hwmon`, `bc250kmd_cli fan`, and a fan line in `telemetry` |
| `tools/win/amdgpu_wddm_control` | the Fan row of the Performance page, and a `fan:` line in the support report |
| `tools/win/bc250mon` | the Fan row of the SoC / GPU panel, and the fan in `GET /telemetry` |

## The five rules the driver keeps

1. **Read only.** See above. There is no write function in the shim at all, so a duty write cannot arrive
   by accident.
2. **The Super I/O configuration ports are never touched.** See above.
3. **The gate is open from 0.7.213, and it still exists.** The reader is finished, so it rides the release on
   and `EnableHwmon` 0 is the one switch that takes every port access away again (the release-train rule,
   owner 2026-10-05). The reason the switch exists is unchanged: the EC window has no arbiter. Windows gives
   the range to a `PNP0C02` motherboard-resources node that drives nothing, and a third-party monitor drives
   the same window through its own kernel helper. Two readers can interleave the latch writes and read each
   other's register, and no lock of ours can prevent that. So the policy checks every sample for
   plausibility, the identity refuses a window that does not answer like this chip, and a measured session
   needs the preflight check below.
4. **The start never fails because of this.** `MmioStart` and `HangDetectorStart` set the precedent. A
   refusal sets the reader offline, writes one log line with every value it read, publishes an empty
   snapshot, and the desktop runs. Nobody loses a screen over a fan sensor.
5. **The escape reads no port.** A `HardwareAccess` escape is a Level Two escape: it idles the GPU and
   stalls a running game (BD-054, and the overlay's summary poll cost 300 ms of game time in trial 41).
   `RUN_HWMON` copies the published snapshot under a spin lock and does nothing else, so it keeps
   `NoAdapterSynchronization` alone.

## The path a fan reading takes

The governor thread of the DPM already runs once a second at PASSIVE_LEVEL. It samples the chip on its own
counter, `NextHwmon`, and publishes one snapshot under `SnapLock`. Every reader copies that snapshot.

```
StartDevice (PASSIVE_LEVEL, once)
  -> HwmonStart             the gate, the base, the identity
     -> bc250_hwmon_identify version, build date, present masks, HWM_CFG, the 32-channel map,
                             and the two UNPROVEN registers (the mode mask, the fan engine status)

DpmThread (1 Hz, PASSIVE_LEVEL)
  -> HwmonSample            one sample: tachometers, duties, the mapped temperatures
     -> bc250_hwmon_sample  the policy, over the port vtable, under PortLock
  -> HwmonPublish           one BC250_HWMON_SNAP under SnapLock

D3DKMTEscape RUN_HWMON (NoAdapterSynchronization only)
  -> HwmonRequest           copies the snapshot, computes AgeMs, sets FRESH. No port access
```

A sample reads the tachometers, the duty read-backs and the mapped temperature channels, and nothing else.
The mode mask (`0x0A00`) and the fan engine status (`0x0CF8`) are read ONCE, at start, and published as the
start found them. Both are documented by the out-of-tree driver alone, no decision of ours reads either, and
an uncharacterised register of the chip that cools the board is not a thing to poke 86 400 times a day.

**What the identity insists on.** The bytes must not be all `0x00` or all `0xFF`, the EC major version must be
1, the build date must be a date in 2020 to 2039, a present mask must exist and must not be `0xFF` everywhere,
`HWM_CFG` bit 7 must stand, and the channel walk must find at least one VOLTAGE source. The last rule is the
one that costs a line of code and buys the most: an I/O window with nothing behind it but its own address
latch answers every read with the byte it was last given, which passes the all-`0x00`/all-`0xFF` pair, the
month and day ranges and the present masks, and would publish four temperature channels of 0.0 C. It cannot
produce a voltage source, because its sources are the channel indexes `0x20` to `0x3F`. E01 found six voltage
channels on this board through the same walk, so a real chip gives one for free.

The walk reads all 32 `MON_CFG` entries and keeps at most four temperature channels. The APU channel (source
`0x46`) is kept whatever its index: on a board that puts four thermistors before it, taking the first four
would lose the one channel a thermal reading cares about.

A sample is a reading only if the identity passed at start AND the sample is fresh. Three missed samples
(`BC250_HWMON_FRESH_MS`, three times the period) make it stale. A stale reading keeps its numbers, because
a support report taken afterwards is still worth having, but every tool shows "no reading" instead of a
value. Nothing guesses.

The sampler is the governor thread, and a start that does not own the SMU never creates it. The reader cannot
see that from inside, so a start that publishes `VALID` with no sample at all for one freshness window answers
`NO_THREAD` from then on. Reporting the healthy `ok` there told an operator that the reader was well and said
nothing about the missing thread.

The reader does not restart itself inside one device start. After ten refused samples in a row it goes
offline with reason `port` and says so once. A loop that keeps writing the latch of a chip somebody else is
talking to would be the one behaviour that could make another program's reading wrong. An offline reader also
writes one log line per reason, not one per telemetry tick: the ring's tail is 768 lines, and a long session
would otherwise spend hundreds of them on one repeated sentence.

## What a sample accepts

| Value | Rule |
| --- | --- |
| Tachometer | refuse `0xFFFF` (the chip's own "no reading"), refuse above 6000 RPM, refuse a change by more than a factor of 4 while both samples turn. A fan that starts or stops has a zero on one side and passes |
| Duty read-back | 0 to 255, published as permille, rounded to nearest. 245 gives 961 |
| Temperature | `(signed word / 128) * 500` millidegrees, a 0.5 C step. Refuse the raw words `0xFFFF` and `0x0000`, refuse a converted 0 mC (the step means the raw words 1 to 127 convert to 0 as well), and refuse a result outside -40 C to 150 C |

A refused value is published as 0 with its valid bit clear, and `RpmValidMask` and `DutyValidMask` carry those
bits to every reader. "Refused" and "reads zero" are therefore different states on the wire, which matters most
for one flag: see `STOPPED` below.

A refused value is re-read once, inside a budget of four retries per sample, because a collision on this
window is a one-off. A value that stays refused goes out as 0 with its valid bit clear.

A sample counts as a reading only if the policy accepted one tachometer or one temperature, AND, where the
identity mapped temperature channels, one of them answered. A duty read-back alone is not a
reading: it is a byte the EC wrote, and `0xFF` (full duty) and `0x00` (no duty) are both legal, so it cannot
tell a chip from a dead window. A temperature can, because the policy refuses the two "nothing here" words.
This is what makes the give-up rule fire on a window that stops answering.

## The settings

All four are `REG_DWORD` under `Services\bc250kmd\Parameters`. From 0.7.213 the INF writes `EnableHwmon` 1
and `HwmonBasePort` 0; the other two stay absent, because they are values an operator pins.

| Setting | Meaning |
| --- | --- |
| `EnableHwmon` | 1 (the default from 0.7.213, written by the INF and by the release installer) starts the reader. 0, and any other value, means no port access happens at all: it is this feature's bisect switch |
| `HwmonBasePort` | the EC base. 0 or absent means the built-in `0x0A20`, measured on unit A. The only other admitted values are `0x0A00` and `0x0A10` |
| `HwmonExpectId` | the pinned EC customer ID. Absent is stage 1: the driver reads the ID, logs it, and clears `ID_PINNED`. An operator reads the value once from that line and pins it. A value that differs is a refusal, because the reader never guesses which chip answered |
| `HwmonDutyProven` | 1 after a lab trial has shown the duty read-back follow the fan. It only opens the `DUTY_PROVEN` flag, which is what lets a tool show a percentage |

`HwmonDutyProven` stays shut because of E01: the chip reported 96 % duty while the fan turned at 1589 RPM,
about half of the 3100 RPM the community reports for full duty. Until a trial resolves that, a percentage
would be a wrong number shown with confidence.

**`HwmonBasePort` takes a list of three values and not a rule.** The two rules the Linux drivers apply (at least
`0x100`, eight-byte aligned, bits 12 to 15 clear) were written for a base the chip itself reports through the
Super I/O. Here the number comes from the registry, and those rules admit `0x0CF8`, the PCI configuration address
port, whose data port would then sit at base + 6, as well as `0x0CD0`, the FCH power-management index pair. The
reader writes three latch bytes to read one register, about ninety of them per sample, so an admitted base is a
place those writes go. The driver therefore admits only the three windows the DSDT reports on this board,
`0x0A00`, `0x0A10` and `0x0A20`, and refuses everything else before a port is touched.

## The escape

`BC250_ESCAPE_RUN_HWMON` is 27, ABI 1, 216 bytes, `READ` the only operation, no administrator needed.
`BC250_ESCAPE_HWMON` in `driver/kmd/bc250kmd_escape.h` is the reply. Three readers parse it by offset, so
three gates keep the layouts equal: `hwmon_native_test.c` asserts the size and every offset at compile time,
`tools/win/bc250mon/test_telemetry.py` compares the C structure with the overlay's mirror, and
`tools/win/amdgpu_wddm_control/test/UnitTests.cs` builds a reply from the header's own offsets.

The flags:

| Flag | Meaning |
| --- | --- |
| `VALID` | the identity passed and the reader is online |
| `MONITORING` | `HWM_CFG` bit 7 stood at start: the chip's firmware monitors |
| `FRESH` | `AgeMs` is inside the freshness window. It needs `VALID` beside it |
| `GATED` | `EnableHwmon` is 0: no port access ever happened |
| `ID_PINNED` | `HwmonExpectId` matched the customer ID |
| `DUTY_PROVEN` | `HwmonDutyProven` is 1 |
| `STOPPED` | every present tachometer answered, every one of them reads 0, and a duty output is not 0, in three samples in a row |

`STOPPED` is the one state the owner must see at a glance. The control application shows it in red, the
overlay panel shows it in red, and it is also what the camera fire watch looks for. That is why it takes three
conditions and a repeat. The window has no arbiter, so one refused tachometer reading is an ordinary collision,
not a stopped fan, and a red row over it would send somebody to the case for nothing. The rule needs
`RpmValidMask`: a channel that reads 0 answered, a channel that was refused did not.

Beside the flag, `Refusals` counts every value the driver refused since the start. A refused value does not
refuse the sample, so it never moves `Errors`; it is still the footprint of a second reader on this window, and a
measured session has to see it. `bc250kmd_cli` prints it as `refusals=`, with `answered=N/M` beside `turning=N/M`.

## What the tools show

```
bc250kmd_cli fan 60 1000
fan fan=2 rpm=1589 turning=1/5 answered=5/5 duty_pct=96 duty_proven=0 mode=0x00 engine=0x00 tsi_c=83.0 \
    board_c=59.5 age_ms=120 fresh=1 valid=1 stopped=0 reason=ok samples=60 errors=0 retries=0 refusals=0
```

`bc250kmd_cli telemetry` prints the same line beside its GPU line, so a lab sampler picks the fan up with
no new process and no new session. The overlay prints `fan_rpm=` and `fan_duty_pct=` in
`mon.py telemetry`. The fan is deliberately NOT a segment of the overlay's painted telemetry line: that
strip clips to a 460-unit panel which the widest GPU line already fills.

## Before a measured session

The window has no arbiter, so a reading is only as good as the knowledge that nothing else drives the same
ports. `scratch/fan-control/lab-fan-read.ps1` is the operator's check. It reads the fan once a second for
60 s at idle and 60 s under a GPU load, through `bc250kmd_cli telemetry 1`, so the fan reading and the
driver's own Tctl come out of one process at one instant. It prints every sample and then judges ten things:

- enough samples arrived, and every one of them was fresh and valid;
- RPM inside 300 to 6000, and no duty read-back above 0 while the fan reads stopped;
- duty read-back inside 0 to 100 %;
- no sample was refused, and the driver's refusal counter did not grow during the run;
- every tachometer the chip reports as present answered (`answered=N/M`);
- a temperature guarded every sample, and the guard names which reading it used;
- the fan mode bit is clear, so the EC still owns the curve;
- RPM rises or holds as the guarding temperature rises.

A 150 s stopwatch ends the run whatever happens, and the load block does not start when less than its own
teardown is left. The script also lists the third-party monitors it can find, because HWiNFO, Open Hardware
Monitor, AIDA64, MSI Afterburner and the like drive this same window, and it refuses to call a reading
measured while one of them runs.

The guard is the hotter of the driver's Tctl and the EC's own SB-TSI channel, with Tctl first. The EC channel
is the instrument under test: it can be absent from the channel map or refused in one sample, so it must not
be the only thermometer of a run that heats the board. Two samples in a row with NEITHER reading end the phase.

## Part B: the write path, NOT implemented

Part B would let the driver set the fan duty. It is not written, and no code of it exists in the tree.

What it would need:

- `bc250_hwmon_write_allowed()` would stop returning 0 for the duty registers. That single function is the
  whole gate today, and both host tests assert that it refuses everything.
- The out-of-tree `nct6687d` alone names the mode mask at `0x0A00` and the duty write registers at
  `0x0A28 + i`, and that driver is MSI-centred. On this board both are UNPROVEN. This repository's rules
  forbid a write to a register of unknown meaning, so each one needs a readback trial first.
- The board owns the fan curve now. Two owners of one fan is a thermal-safety question, not a feature
  question: a driver that holds a low duty while the EC wants a high one would hide a hot board.
- The BIOS "Fan Setting" option would have to stay where the owner put it, which means our duty would have
  to live inside whatever the EC curve allows, or replace it completely.

**The owner has answered the first question.** On 2026-10-06, asked whether the driver may take the fan over
from the BIOS curve at runtime, the owner said yes, under three conditions: the BIOS "Fan Setting" option
itself stays unchanged, the driver owns the fan only while it runs, and it gives the fan back to the chip's
automatic mode on every exit path. Fan control in our GUI is part of the same instruction, beside the GPU
voltage/frequency curve and the CPU undervolt.

**That answer does not put any write code in this tree.** The branch that carries this page is the read path,
Part A, and its scope was frozen to reading. Every statement above about `bc250_hwmon_write_allowed()`
refusing every register is a statement about the code as it stands, and both host tests assert it.

What is still open, and what Part B needs before a single duty byte is written:

- whether the EC curve stays as a floor under our duty, or our duty replaces it;
- the readback trial for the two UNPROVEN registers, the mode mask at `0x0A00` and the duty writes at
  `0x0A28 + i`, which is a lab step and not a code change;
- the handback rule the owner set, written as code on every exit path: the user's own request, the loop
  stopping, device stop, a power transition out of D0, driver unload, a lost user-mode lease and a stalled
  control loop. A bugcheck callback can only be best effort, so the safe-hold duty carries that case.

Until Part B exists, the fan row in every tool is a reading and nothing else.
