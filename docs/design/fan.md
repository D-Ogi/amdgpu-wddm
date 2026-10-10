# The case fan

Unit A has one case fan. The board turns it, not our driver. A Nuvoton NCT6686D hardware monitor on the
board holds the fan curve, and the BIOS "Fan Setting" option selects which curve it uses. The owner keeps
that option as it is (2026-10-05).

Until KMD 0.7.213.1 nothing in Windows could read that chip. The control application showed "The driver
cannot read it yet" in its Fan row, and a lab session record held no fan speed. This page is the design of
the read path, which KMD 0.7.213.1 adds. The path was written as revision 208 on `fan/read-nct6686` and
train b20 merged it into the lineage that ships, so the released 0.7.208.1 does not have it.

**Part B, the write path, is the last section.** The owner allowed it on 2026-10-06: the driver takes the
fan from the BIOS curve while it runs, and gives it back to the chip's automatic mode on every exit path.
Part B adds one writer, `driver/shim/bc250_fan.c`, and keeps the reader of this page as it is. Its lab write
path is not yet validated on Windows: see the lab trial at the end.

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
configuration, control, limit or duty register. `bc250_hwmon_write_allowed()` admits three EC registers, and
only the fan control of Part B calls the write function. Both host tests assert that a sample of the reader
writes nothing outside the two latch ports.

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
| `driver/shim/include/bc250_fan.h`, `driver/shim/bc250_fan.c` | Part B, the policy of the fan control: the handshake, the restore record, the curves, doubt, the emergency, leases, faults. No OS call |
| `driver/kmd/fan.h`, `driver/kmd/fan.c` | Part B in the miniport: the gate, the step, the exit paths, the watchdog, the bugcheck callback, the stored choice, `RUN_FAN` |
| `tools/win/amdgpu_wddm_control/src/MainForm.Fan.cs`, `FanPlan.cs` | Part B in the control application: the Case fan card and its two helper actions |

## The five rules the driver keeps

1. **The reader reads only.** See above. The reader has no write call. The one writer is the fan control of
   Part B, which goes through `bc250_hwmon_write8()` and an allowlist of three registers.
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

## Part B: the write path

Part B lets the driver set the fan duty. The owner allowed it on 2026-10-06 ("TAK"), under three conditions:

1. The BIOS "Fan Setting" option stays unchanged. The driver never writes it.
2. The driver owns the fan only while it runs.
3. The driver gives the fan back to the chip's automatic mode on every exit path.

The release-train rule applies: the feature is on by default, and `EnableFanControl` 0 is its bisect switch.
The goal is parity with the SkillFishOS Control Center: an automatic mode, a driver curve, presets and a
curve that the user edits.

### What the chip does

M803 measured the write sequence on unit A under Linux, on fan index 1, the one fan that turns. The
sequence is the `nct6687d` handshake:

| Step | Register | Value | What M803 saw |
| --- | --- | --- | --- |
| open | `0x0A01` (configuration request) | `0x80` | the engine status `0x0CF8` goes from `0x60` to `0x08` within one 1 ms poll |
| take | `0x0A00` (mode mask) | bit 1 set | the EC stops driving fan index 1 |
| duty | `0x0A29` (duty target of index 1) | 0 to 255 | 255 gives 1699 to 1749 RPM, 102 gives 771 to 774 RPM |
| close | `0x0A01` | `0x40` | `CFG_CHECK_DONE` and `CFG_LOCK` come back within four polls. `CFG_INVALID` never sets |
| give back | `0x0A29`, then `0x0A00` | target 128, then bit 1 clear | the EC curve runs the fan again (1357 RPM after 3 s) |

`bc250_hwmon_write_allowed()` admits these three registers and no other. The duty targets of the other
four channels, the configuration register, the engine status and the Super I/O ports stay refused. The
host test asserts each refusal.

### The rules of the policy

`driver/shim/bc250_fan.c` decides every duty and makes every write. It has no OS call, no lock and no
time source, so the host test runs it against a model of the chip. These are its rules:

1. **Only three registers, each one inside an open phase.** A phase that does not open, does not close or
   closes without `CFG_LOCK` is a handshake failure.
2. **The restore record comes first.** Inside the first open phase, before the first change, the policy
   reads the mode mask and the duty target as the board left them. If our mode bit is already set at that
   moment, a previous driver stopped while it held the fan. The record then holds the M803 rest values
   (mode `0xE0`, target 128) and sets `SUBSTITUTED`.
3. **The give-back writes the target first and the mode second.** It writes the recorded target, then
   clears our bit in the live mask and leaves the other bits as the chip has them. It reads both back.
4. **On doubt, full speed.** The guard temperature is the hotter of Tctl and the EC's own SB-TSI channel.
   No valid Tctl, a stale reader, or two readings more than 10 C apart puts the fan at 100 %. Doubt held
   for 5 s gives the fan back to the board. After 30 s of clean inputs the driver takes it again, at most
   three times per start.
5. **A refusal by the chip is a fault.** A target that does not read back, a mode bit that does not stick,
   a duty read-back that does not follow the target, and a stopped fan at or above the floor each give the
   fan back at once. The fault latches: the driver writes nothing more in this start, except a retry of a
   give-back that failed, every 10 s.
6. **The duty never goes below 20 %, and 0 % is never written.** A three-wire fan can stall below the
   floor and not start again by itself.
7. **Emergency at 87 C.** A guard temperature at or above 87 C forces 100 % whatever the curve says. The
   emergency ends when the guard stays at or below 82 C for 10 s. 87 C is also the DPM's hot step, so the
   fan acts before the clock pays.
8. **The output rises at once and falls slowly.** It falls only after 10 s below, by 10 points at most
   every 2 s. The curve's input falls only when it is 3 C under its peak.
9. **A leased mode ends with the durable mode from before it.** A fixed duty, and a curve with a lease, end
   when the lease runs out and nobody renewed it. The driver then runs the last mode it got without a lease
   (or the mode of the start): the driver's curve, which keeps the fan and follows the temperature again, or
   the board, which gets the fan back. Until 0.7.215 every lease ended with the board, so a fan test from the
   control application left the board's curve in force instead of the one the user chose.
10. **A sustained heavy load drives the fan to full speed.** The load feed-forward below reads the governor's
    load, not only the temperature. It raises the duty to 100 % and never lowers it, and every rule above it
    keeps its place.

### The curves

A curve has 2 to 8 points. The temperatures are 20 to 95 C and rise strictly. The duties are 20 to
100 % and never fall as the temperature rises. Between two points the duty is the straight line, rounded
up to a whole percent. Below the first point the first duty applies, above the last point the last duty.

| Profile | Points (C : %) | Use |
| --- | --- | --- |
| Standard (the default) | 40:50, 60:70, 70:85, 76:95, 80:100 | never slower than the BIOS Standard Mode, full speed at 80 C, under the 82 C below which the DPM releases a thermal cap |
| Quiet | 40:30, 60:45, 70:60, 80:80, 85:100 | quieter than the board below 80 C |
| Performance | 40:60, 55:75, 65:90, 75:100 | louder everywhere |

The Standard curve rests on four measurements under the BIOS Standard Mode: 65 C gave 65 %, 69 C gave
77 % (the b20 read trial), 70 C gave 80 % (M803) and 83 C gave 96 % (E01). At those four temperatures
the Standard curve gives 76 %, 81 %, 82 % and 98 %. It gives 95 % (about 1650 RPM) at 80 C and 100 % from
85 C. No measurement exists below 60 C, so the curve holds 50 % or more there. Every profile reaches 100 %
at or below 85 C. The 87 C emergency is the backstop for a quiet custom curve.

### The load feed-forward

A curve answers late. It reads the heat that the load has already made, so the fan reaches full speed when
the board is hot already. On 2026-10-10 an LLM benchmark arm on unit A held the GPU at 93 to 100 % busy and
107 to 122 W of SMU socket power, and Tctl walked from 62.8 C to 76.9 C in 14 s. The Standard curve answered
with 95 % duty. The owner asked for the obvious: the driver sees the load, so it must blow at full power
without waiting for the temperature ("Robiąc takie testy sterownik powinien sam ogarnąć, że trzeba wiać z
maksymalną mocą!", "doing such tests the driver should work out by itself that it has to blow at full
power").

The governor hands the fan step a load feed beside the Tctl reading (`BC250_FAN_LOAD` in
`driver/kmd/fan.h`). It holds the GPU busy share, the GFX clock and the SMU socket power. The busy share is
the mean over the whole second between two fan steps, weighted by each DPM tick's own length, because one
25 ms tick says nothing about a sustained load. A step without the feed runs on the curve alone.

A step is heavy when the feed says any of this:

| Signal | Threshold | Why this number |
| --- | --- | --- |
| GPU busy, with the GFX clock at or above 1000 MHz | 850 permille | the LLM arm reads 930 to 1000 permille at 1500 MHz. A busy GPU at the 500 MHz idle point is a desktop that composes |
| SMU socket power | 85 W | unit A idles at 41 to 56 W. The arm reads 107 to 122 W, and a GPU fill at 1000 MHz reads about 90 W (M828) |
| the guard temperature's rise | 3 C over a 3 s window | 1 C a second. The arm's first seconds rise faster than that |

Heavy time is counted in elapsed milliseconds, never in steps, so the rule does not depend on the
governor's cadence:

- A heavy step adds its own length to the account, any other step takes its length away, and the account
  stops at 4 s. That margin lets one quiet step inside a load pass without disarming the boost. The arm of
  2026-10-10 has such steps.
- The boost engages when the account reaches 2 s, so one busy second every ten (a menu, a single compile)
  never engages it.
- The duty is then 100 % until the load ends. The write happens once: the duty byte does not change again
  while the boost holds.

The way down is deliberately slow:

- The boost holds for 30 s after the account has run out.
- After that it ends only when the curve itself asks for less than the duty in force. The board must be
  under the curve's own point for that duty before the fan is allowed to slow down at all.
- The duty then falls by rule 8, which is 10 points at most every 2 s after 10 s below.

What the feed-forward never does:

- It never lowers a duty. The curve's answer stands wherever it is the higher one.
- It never runs in the board's mode, under a latched fault, or with `FanLoadBoost` 0.
- It raises no fixed duty. A fixed duty is the operator's own number under a lease, and the 87 C emergency
  is still above it. The account keeps running under such a lease, so the curve the lease ends with is
  boosted at once when the load never stopped.
- It survives no give-back. A boost is a duty, and after a give-back the duty is the board's. The driver has
  to see the load again.

A quiet profile gets the boost as well. A user who wants the curve and nothing else sets `FanLoadBoost` 0.

### The exit paths

Each exit path calls `bc250_fan_handback()` with its reason, through `driver/kmd/fan.c`. The give-back
uses the full handshake and checks the result. A give-back that fails keeps `CONTROLLING` set, so the next
exit path, the fault retry and the bugcheck callback still try.

| Exit path | Where | Reason |
| --- | --- | --- |
| device stop and remove | `FanStop` from `Bc250StopDevice` and `Bc250RemoveDevice`, after `DpmStop` | `stop` |
| out of D0 | `FanPause` from `Bc250SetPowerState` after `DpmPause`. `FanResume` takes the fan again in D0 | `power` |
| out of D0, display-only branch | `FanStop` on the branch that stops the governor | `power` |
| driver unload | `FanDriverUnload` from `Bc250Unload`, for a device that the stop paths missed | `unload` |
| the step stops | the watchdog DPC: no step for 3 s. A step that holds the controller for 6 s gets a blind give-back | `watchdog` |
| the user asks | `RUN_FAN` BOARD, applied at the next step | `user` |
| a lease runs out | inside the policy, at the step that sees it | `lease` |
| the control is off while the driver holds the fan | the next step, inside the policy | `disabled` |
| bugcheck | the bugcheck callback and `Bc250ResetDevice` (`FanResetDevice`) | `bugcheck` |

The bugcheck path runs at HIGH_LEVEL with the other processors stopped. It makes port writes only: no
lock, no log, at most 20 polls of 100 us, and no check of the result. It is best effort. If it fails, the
fan stays at the last duty that the driver wrote, and the EC curve returns at the next boot.

### The settings

All are `REG_DWORD` under `Services\bc250kmd\Parameters`.

| Setting | Meaning |
| --- | --- |
| `EnableFanControl` | 1 (the default, written by the INF and by the release installer) lets the driver take the fan. 0, and any other value, means the driver never writes the chip. This is the bisect switch |
| `FanMode` | the stored choice: 0 the board's curve, 1 the driver's curve. Absent means the driver's curve |
| `FanProfile` | with `FanMode` 1: 1 Standard (also when absent), 2 Quiet, 3 Performance, 0 custom |
| `FanCurvePoints`, `FanCurve0` to `FanCurve7` | with `FanProfile` 0: the point count (2 to 8) and each point as `(degrees C << 8) \| percent` |
| `FanLoadBoost` | 1 (also when absent) runs the load feed-forward. 0, and any other value, leaves the duty to the curve alone |

The INF writes `EnableFanControl` only. The driver writes the stored choice when a request carries
`Store` 1. A stored choice that the policy refuses runs the Standard curve, and the log says so.

The control also needs the reader of Part A online and the chip identified as unit A's. A start that
fails either condition publishes a gate and writes nothing:

| Gate | Meaning |
| --- | --- |
| `ok` | the fan control runs |
| `EnableFanControl 0` | the switch is 0 |
| `reader offline` | `EnableHwmon` is 0, or the reader refused the window |
| `customer ID not unit A` | the customer ID is not `0x162B` (M803) and `HwmonExpectId` does not pin it |

### The escape

`BC250_ESCAPE_RUN_FAN` is 30, ABI 1, 272 bytes. The reply is `BC250_ESCAPE_FAN` in
`driver/kmd/bc250kmd_escape.h`. Like `RUN_HWMON`, the escape reads a published snapshot and touches no
port. Every operation takes `NoAdapterSynchronization` only, so a request never idles the GPU.

| Op | What it does | Who may send it |
| --- | --- | --- |
| READ (0) | returns the snapshot | everyone |
| BOARD (1) | gives the fan to the board's curve. `Store` 1 makes that the choice of every start | administrator |
| CURVE (2) | the driver's curve: a preset, or custom points. `LeaseMs` 0 is durable and `Store` 1 writes it. 5 to 300 s is a trial that ends with the durable mode from before it (rule 9) | administrator |
| FIXED (3) | one duty of 20 to 100 %, always under a lease of 5 to 300 s, never stored | administrator |
| RENEW (4) | restarts the lease of a leased mode | administrator |

A write needs `ExpectedGeneration` equal to the `Generation` that a READ of this start returned
(`STATUS_RETRY` otherwise). It also needs an enabled control (`STATUS_INVALID_DEVICE_STATE` otherwise,
with `Gate` set). The escape only leaves the request. The governor thread applies it at its next step,
within one second. A refused request changes nothing, and `Error` names the rule.

RUN_FAN is a new command, not a new revision of an existing one, and `BC250_KMD_VERSION` does not change.
The escape adds a command and moves no existing layout. An older driver answers
`BC250_ESCAPE_STATUS_UNKNOWN_COMMAND`, and that is how a tool finds that it talks to an older driver. The
version constant follows the INF `DriverVer`, so the release train that carries this escape bumps it.

Three gates keep the layout equal in its readers. `fan_native_test.c` asserts the size and every offset at
compile time. `amdgpu_wddm_control/test/FanTests.cs` compares `KmdReply.ParseFan` and the CLI request
structure with the C sources. `bc250kmd_cli/test_escape_flags.py` checks the escape flags of every
operation.

### What the tools show

`bc250kmd_cli fan` prints the reader's line as before and one `fanctl` line after it:

```
fanctl state=curve mode=curve profile=standard target_pct=70 applied_pct=70 raw=179 readback=179 rpm=1180 \
    guard_c=60.5 enabled=1 controlling=1 emergency=0 leased=0 lease_ms=0 fault=0 paused=0 held_back=0 \
    gate=ok reason=none doubt=none takeovers=1 handbacks=0 writes=3 ... curve=40:50,60:70,70:85,76:95,80:100 \
    saved_mode=0xE0 saved_target=128 boost=on boost_why=busy+power
```

`boost` is `on` while the load feed-forward holds the fan at full speed, `off` in a start that may boost and
does not now, and `disabled` with `FanLoadBoost` 0. `boost_why` names every signal that called the load
heavy: `busy`, `power`, `rise`, or them joined with `+`. The driver log carries one line when the boost
engages and one when it lets go. The telemetry block carries the count and the time, but only in a start that
boosted at least once: a cool or idle run keeps that block at the four fan lines it had, because the log ring
rotates 768 lines and six lines every 5 s would shorten what the ring holds of a long session (BD-097). The
start itself logs whether the rule is enabled, so nothing about the boost is lost with those two lines.

The values in this example come from the test fixture, not from the lab. The write forms need an
administrator:

```
bc250kmd_cli fan auto [store]
bc250kmd_cli fan curve [standard|quiet|performance] [store]
bc250kmd_cli fan set <percent 20..100> <seconds 5..300>
bc250kmd_cli fan renew <seconds 5..300>
```

The control application has a Case fan card on the Performance page. It shows:

- The fan speed and who runs the fan. The speed comes from `Sensors.FanValue`, the same function that fills
  the Fan row of the Now card: the reader's own sample first, else the RPM in the `RUN_FAN` reply. The two
  cards therefore never disagree.
- Five modes in one segmented row: Automatic (board), Driver curve (the default), Quiet, Performance and
  Custom, with one line that says what the selected mode does.
- A chart of the curve: temperature 20 to 95 C across, speed 0 to 100 % up. It shades the 20 % floor and the
  87 C zone, draws the curve in force dashed when it differs, and puts a ring at the guard temperature and
  the applied duty. A preset shows its curve here before it is applied. The points move with the mouse or
  the keyboard (left and right select, up and down change the speed, Ctrl with left and right changes the
  temperature), always through `FanCurves.Move`, so a curve made on the chart obeys the curve rules. Each
  point is an accessible child with its own name and value. Compact rows under the chart show every point
  again, with an inline message when a row breaks a rule.
- Rules 4 to 8 above in plain words.
- "Apply curve" (or "Hand the fan to the board"), enabled only when the choice differs from the one in
  force and stored. After an apply, "Go back to the previous setting" applies the choice from before.
- A short test: one speed from 30 to 100 % for 10 s (`--action fan-test --fan-test-pct N`). The helper sends
  FIXED with a 15 s lease, as `bc250kmd_cli fan set` does, reads the fan once a second, and then sends the
  choice that was in force again (CURVE or BOARD, `LeaseMs` 0, `Store` 0). If the helper stops halfway, the
  lease runs out and the choice from before the test comes back (rule 9). The window keeps its own poll during the test and shows
  the fastest RPM it saw. The test is refused while the driver does not run the fan in the normal way:
  leased, paused, fault, emergency, doubt or held back.

The choices go through the elevated helper (`--action fan-auto` or `--action fan-curve`), always with
`Store` 1. "Reset to defaults" puts the Driver curve back. The support report carries one `fan control:`
line.

The card has no switch for the load feed-forward yet. The next step on the application side is one check box
on the same card, in plain words and in the four languages, that writes `FanLoadBoost` through the elevated
helper, plus the state of the boost beside the fan speed.

The read check of Part A, `lab-fan-read.ps1`, judges the board's own curve. With the fan control on, run
`bc250kmd_cli fan auto` before it, or the RPM trend it judges is the driver's curve.

### The lab trial

The Windows write path is host-tested only. One lab trial of at most three minutes validates it:

1. Install the release that carries Part B. Read `bc250kmd_cli fan`. Pass: `fanctl state=curve
   controlling=1 gate=ok fault=0`, and `readback` within 8 counts of `raw`.
2. Run `bc250kmd_cli fan set 40 30`. Pass: about 770 RPM within 5 s.
3. Wait for the lease to run out, or run `bc250kmd_cli fan auto`. Pass: `state=board controlling=0`
   and about 1360 RPM, the EC curve, within 5 s.
4. Run `bc250kmd_cli fan curve`, then restart the display device as the kmd-deploy kit does (disable,
   enable). Pass: while it is disabled the fan turns at the EC curve's speed. After the enable
   `controlling=1` again.

During the whole trial Tctl stays below 87 C, and the trial stops at once if it does not.

The load feed-forward needs a load, so it has a trial of its own: the LLM arm `g35-up-uh-d4k` of
2026-10-10, which is 170 s and whose baseline is Tctl 76.9 C with the fan at 95 % duty. Pass: `boost=on`
within 3 s of the arm's first heavy second, `raw=255` for the whole arm, and a Tctl maximum under that
baseline. The arm's own numbers (tokens a second) must not fall.

### Open risks

- The EC window has no arbiter. A third-party monitor that drives the same window can interleave its latch
  writes with ours. The readback checks turn such a collision into a fault and a give-back, not into a
  wrong duty that stays.
- The bugcheck give-back is best effort. It cannot wait for a slow chip and does not check its result.
- The watchdog DPC runs the handshake at DISPATCH_LEVEL. The polls are bounded (200 polls of 250 us), but
  a chip that never answers costs that time at raised IRQL.
- No lab run on Windows wrote the chip yet. M803 measured the sequence under Linux only.
