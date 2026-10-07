# The Tuner: a GPU voltage curve, a CPU undervolt and the core mask

KMD 0.7.213.1 (written as 0.7.210 on `tuner/vf-cpu` with the review fixes of 0.7.211; neither number was
released). Decision:
[ADR 0020](../adr/0020-operator-tuning-in-the-smu-owner.md). Limits: [`../hardware.md`](../hardware.md). The
clock governor this page builds on: [`dpm.md`](dpm.md).

This page tells an operator and a reader of the code what the driver offers, what it refuses, and which parts of
it nobody has measured yet. Two controls are new:

- **The GPU voltage curve.** One voltage for each clock level from 1000 to 2000 MHz. The curve changes only the
  voltage column, because the clock grid is fixed at 100 MHz steps.
- **The CPU surface.** A maximum boost clock, an undervolt in firmware curve-scale steps, the firmware
  temperature cap, and the core-enable mask.

Both are off until somebody asks for them. A start that asks for nothing behaves exactly as 0.7.207 did.

## What makes a change safe

Four rules hold for every control on this page. They are in the code, not in the documentation alone.

1. **The kernel is the single SMU owner.** Every change is an escape into `bc250kmd`. There is no user-mode
   mailbox, no second writer and no return of a legacy direct writer.
2. **Apply is a trial, and the kernel owns the deadline.** A set gives a candidate a window (25 s by default,
   10 s to 180 s). When the window ends without a keep, the driver puts the stored value back by itself. A tool
   that is killed, hung or disconnected changes nothing about that.
3. **Nothing reaches the registry before a keep.** A machine that hangs under a candidate boots with the last
   value somebody kept.
4. **A kept value still faces the boot guard.** The next start writes a pending mark before it applies the value
   and a confirmed mark after the start is healthy. A start that finds the pending mark of an earlier start drops
   the value and runs the shipped settings. One bad keep costs the setting, never the machine.

## The GPU voltage curve

### What a curve is

The curve is eleven voltages, one for each level from 1000 MHz (the lab floor) to 2000 MHz. Levels below the
floor are not in the curve: they all run at 820 mV, and there is no voltage anchor under the lowest one.

The default curve is the shipped table of [`dpm.md`](dpm.md). `bc250_clock_curve_default` builds it from that
table, so "no curve" and "the table's own line" are the same thing.

### What the driver refuses

`bc250_clock_curve_check` (host-tested in `driver/shim/test/dpm_test.c`) refuses a candidate as a whole. It never
accepts part of one.

| Refusal | Rule |
|---|---|
| `RANGE` | A value outside 820 to 1000 mV. |
| `FLOOR` | The 1000 MHz level is not exactly 820 mV. The lab point stays the lab point. |
| `DEPTH` | A value more than 25 mV under the table's line at that clock, or under 820 mV. |
| `ORDER` | The voltage falls while the clock rises, or the encoded identifier inverts. |
| `NULL` | No curve was given. |
| `UNTRIED` | A keep of a candidate the governor has not applied. Not a property of the curve: a property of the moment. |


The depth bound is why a curve cannot ask for a large undervolt in one step. 25 mV is about four voltage
identifier steps of 6.25 mV. The same bound is in `bc250_clock_point_allowed`, so the administrator's direct
clock escape obeys it too.

### How a curve reaches the hardware

The escape writes the candidate into the governor's state under its spin lock and raises one flag. The governor
thread takes that flag at its next 25 ms tick and re-applies the level it is already on, through the same checked
transaction every clock change uses (`SmuSetPoint`: voltage up before a raise, down after a lowering, read back).

Two readers had to learn about the curve, and a missed one would have dropped the clock to the floor: the resync
path and the 1000 ms readback compare the hardware identifier against the **active** curve, not against the
shipped table.

### The window

| Event | What happens |
|---|---|
| Set | The candidate becomes active at the next tick. The window starts. |
| Set again inside a window | The new candidate replaces the old one and the window restarts. The revert target stays the stored curve. |
| Keep inside the window | The candidate becomes the stored curve, the eleven values reach the registry, and the guard's marks follow. |
| Keep before the governor's tick | Refused with `UNTRIED`, because the candidate has run on nothing. The governor applies it within 25 ms; the application offers Keep only once the reply says it did. |

| Cancel | The stored curve comes back at the next tick. |
| The window ends | The same as a cancel, without anybody asking. |
| A stop, or a power transition | The trial ends first, before the driver puts the floor back. |
| A reset | The stored curve goes off the disk and the table's own line runs. |

### The settings

All `REG_DWORD` under `Services\bc250kmd\Parameters`.

| Value | Meaning |
|---|---|
| `DpmCurve1000` .. `DpmCurve2000` | The stored voltage of that level, in millivolts. Absent means the table's line. |
| `DpmCurvePending` | A start applied the stored curve and was never confirmed healthy. |
| `DpmCurveConfirmed` | The checksum of the curve a healthy start confirmed. |
| `DpmCurveTrialMs` | The window a set gets when it asks for none. |
| `DpmCurveLastReason` | 0 none, 1 the stored curve runs, 2 refused, 3 an unconfirmed start, 4 the registry refused the mark, 5 this start does not govern. |

A curve acts only while the governor runs. A fixed-lab start, a start whose governor gave up and a start without
the SMU owner all leave the state at the table's line and say so in the driver log.

## The CPU surface

### Why it opens slowly

The CPU rail is reached through the firmware's queue 3. Its three mailbox registers are in the BAR5 aperture the
driver already maps, and they were already in the read allowlist, so no new window into the chip was needed. What
is new is a second transport instance, a second message allowlist and a per-message argument range.

Everything about what those messages **do** is REPORTED: two community projects agree on the numbers and nobody
has measured one on unit A. One of those projects destroyed a board by raising the CPU clock while the voltage
identifier scaled freely, and its own ceiling is 1.325 V. The driver's answer:

- `CpuTune` must be 1. Without it the surface is read-only, no thread runs and no message is sent.
- No setter runs before a getter has answered in the same start (`BC250_CPU_FLAG_QUEUE3_PROVEN`).
- No absolute CPU voltage is ever forced. The undervolt scales the firmware's own curve.
- The applied voltage is read back after every change. Above 1300 mV, or outside 700 to 1600 mV, the driver
  undoes the change at once.
- One setter per 100 ms (`BC250_CPU_MESSAGE_GAP_MS`), the owner lock released between messages, the temperature
  re-read before each one, and no setter at all while the GPU is 50 % busy or busier.
- A getter changes nothing, so it waits only for the firmware's mailbox turnaround
  (`BC250_CPU_GETTER_GAP_MS`, 10 ms) and is not refused by the temperature. The first read stage of a start
  still uses the setter's gap, because nothing has spoken to queue 3 yet. 0.7.210 sent the whole 19-message
  read stage at the setter's rate, which held the surface for 1.9 s and was the reason a readback could be
  mistaken for a hang.
- The 87 C gate has two exceptions, both of them the rule the GPU clock path already carries: a getter, and a
  step that lowers the dissipation. The way back from a trial must not be refused because the part is hot,
  since nothing else would ever take the trial out of the chip (`AllowHot` in `SmuCpuMessage`).

### The allowlist

A getter sent as a setter and a setter sent as a getter are both refused, so a caller cannot mislabel a message.
`driver/shim/test/cpu_test.c` holds the positives, the negatives and the argument ranges.

**What the host tests do not reach.** No host suite compiles `driver/kmd/cpu.c`. The policy it obeys is in the
shim, and the shim suites test it there, which is why 0.7.211 moved the revert target into `bc250_cpu_restore_target`: a rule
in the shim gets a test, a rule in the miniport does not. The miniport keeps the parts that need a kernel: the
locks, the worker thread, the registry and the message gaps. A harness for them, like `smu_native_test.c` for
`driver/kmd/smu.c`, is separate work and is not in this version.

| Queue | Message | Direction | Argument |
|---|---|---|---|
| 3 | `0x36` CPU voltage | get | 0 |
| 3 | `0x37` GPU voltage | get | 0 |
| 3 | `0x3B` P-state clock | get | 0..7 |
| 3 | `0x40` temperature cap | get | 0 |
| 3 | `0x42` SoC clock | get | index 0..19 in the high half |
| 3 | `0x43` core clock | get | core 0..7 |
| 3 | `0x50` curve scale | set | 0, or a negative 16-bit value of at most 16 steps |
| 3 | `0x8B` temperature cap | set | 85..100 C |
| 3 | `0x8F` maximum boost clock | set | 2800..4000 MHz |
| 0 | `0x0C` QueryCorePstate | get | core 0..7 |
| 0 | `0x3D` GetEnabledSmuFeatures | get | 0 |
| 0 | `0x2C` SetCoreEnableMask | set | 0x77 or 0xFF |

### The ranges, and why the top one needs an undervolt

| Control | Release | Lab (`CpuLab` 1) |
|---|---|---|
| Maximum boost clock | 2800 to 3500 MHz | up to 4000 MHz, and only with at least four undervolt steps in the same request |
| Undervolt | 0 to 16 curve-scale steps | the same |
| Temperature cap | 85 to 100 C | the same |

The release range is a **limit** before it is ever a raise: lowering the firmware's ceiling lowers the voltage the
firmware chooses, so it cannot reach the hazard. 3500 MHz is the highest limit the release build takes. It is also
the stock boost clock the community's two tools report; nothing on unit A has measured it. The 3600 MHz this
document carried until 0.7.211 had no source at all. The lab range is the owner's hardware pre-approval of
2026-10-05, and `bc250_cpu_settings_check` refuses it without at least `BC250_CPU_LAB_MIN_UV_STEPS` undervolt
steps in the same request.

One undervolt step is not a fixed number of millivolts. The community's own fitted model is about
`0.004325 x f - 10` mV per step, which is 3 mV at 3000 MHz and 7 mV at 4000 MHz. The driver therefore never
computes a voltage from a step count: it reads the voltage back.

### The order of a change

The predicted voltage must never rise above the ceiling on the way, so every step that lowers the voltage goes
before every step that raises it, and each control is judged on its own.

| Step | Direction |
|---|---|
| A deeper undervolt | lowers |
| A lower clock limit, the first limit of all included | lowers |
| A shallower undervolt | raises |
| A higher clock limit | raises |

A tightening temperature cap goes first of all, a loosening one last of all.

### The failure signs

A trial stops at the first of these. The first two are the caller's to count, because the kernel cannot see them:
they travel in `WheaEvents` and `ChecksumErrors` of the SET and SEARCH_STEP escape. Clock stretching is judged
only over a sample the caller marks `Loaded`, since an idle core is under its limit for no bad reason. The rest
are the driver's.

| Sign | Where it comes from |
|---|---|
| A machine check | the `Microsoft-Windows-WHEA-Logger` provider in the system event log |
| A wrong answer from the load | the load client's own checksum |
| Clock stretching | a core 200 MHz or more under the applied limit (`0x43`, and the Windows counters) |
| The voltage readback | above 1300 mV, or outside 700 to 1600 mV |
| 87 C | the same limit the GPU governor uses. A step that lowers the dissipation, and the way back, go out anyway |

### The guided undervolt search

"Find my setting" walks one step at a time, with a load window per step. The judgement is the shim's
(`bc250_cpu_search_next`); the load is the caller's. The search stops at the first failure sign, steps one back,
and offers the deepest step that passed. It also stops when the voltage does not move at all, because then the
firmware is not taking the scale and a deeper step would say nothing. Nothing is persisted: a person presses
Keep.

### The core mask

The part is sold with six of its eight cores enabled; the stock mask is `0x77`. Our route is the AMD-named
queue 0 message, which can write the stock mask back. The community's generic SMN write through queue 3 `0x98`
can only ever write `0xFF` and hangs the firmware with argument 0, so it stays rejected.

The mask is a registry value, a two-mark boot guard and the next Windows restart, exactly like the CU mode. The
core-presence mask is not readable on Windows, so the application shows the processor count and never the mask.

**Nobody has reported a result from `0x2C` on this part.** One lab step settles whether it is a live route or a
no-op on a harvested die, and either answer closes an open question.

### The settings

| Value | Meaning |
|---|---|
| `CpuTune` | 1 opens the surface. Absent or 0: read-only, no thread, no message. |
| `CpuLab` | 1 admits the lab clock range, with an undervolt. |
| `CpuMaxMHz`, `CpuUvSteps`, `CpuTempC` | the stored settings, applied at start, absent for none |
| `CpuTrialMs` | the window a set gets when it asks for none |
| `CpuPending`, `CpuConfirmed` | the two guard marks of the stored settings |
| `CpuLastReason` | 0 none, 1 applied, 2 refused, 3 an unconfirmed start, 4 the registry refused the mark, 5 `CpuTune` is 0, 6 queue 3 did not answer, 7 the firmware or the voltage readback refused |
| `CoreMask` | 119 (`0x77`, stock) or 255 (`0xFF`) |
| `CoreMaskPending`, `CoreMaskConfirmed` | the mask's two guard marks |

## The escapes

| Escape | Size | Flags | Who |
|---|---|---|---|
| `BC250_ESCAPE_RUN_DPM_CURVE` (28) | 360 bytes | `NoAdapterSynchronization` for every operation | read: anybody; set, keep, cancel, reset: an administrator |
| `BC250_ESCAPE_RUN_CPU` (29) | 296 bytes | read and keep: `NoAdapterSynchronization`; everything else: `HardwareAccess` | read: anybody; everything else, the readback included, an administrator |

The curve escape touches no hardware: a curve reaches the SMU through the governor's next tick. The CPU escape
does send mailbox messages, which is why its write operations take the adapter, exactly as a clock set does.

Keep is the exception, from 0.7.213: it copies the applied settings into the stored ones, writes seven values of
the Parameters key - each one flushed to the disk - and ends the trial. It sends no message and reads no register,
so it takes `NoAdapterSynchronization` and never suspends the GPU scheduler over seven registry flushes. For one
release the driver still admits the `HardwareAccess` word keep asked for up to 0.7.212, so an older CLI, DLL or
control application keeps working; a tool of this release sends the new word, and sends it again with the old one
when a driver of 0.7.212 or older refuses it (`Status` REFUSED, `NtStatus` `STATUS_INVALID_PARAMETER`), so a keep
is not lost on a machine whose device restart is still pending.

The readback asks for an administrator although it changes nothing: it holds the surface for 19 messages, which
is not something an unprivileged loop may do to a shared mailbox.

Both refuse a request whose `ExpectedGeneration` is not this start's, so a tool cannot change a setting across a
driver restart without noticing.

## The command line

```
bc250kmd_cli dpm curve                                   the three curves, the floor of each level, the window
bc250kmd_cli dpm curve preset mild|medium|deep [window]  10, 20 or 25 mV off the line where the band allows it
bc250kmd_cli dpm curve offset <mV> [window]              the same, with your own number
bc250kmd_cli dpm curve set <mV> ... <mV> [window]        eleven values, 1000 MHz first
bc250kmd_cli dpm curve keep | cancel | reset

bc250kmd_cli cpu                                         what is applied, stored, recorded and last answered
bc250kmd_cli cpu readback                                the getters of both queues; this is what admits a setter
bc250kmd_cli cpu set [clock <MHz>] [uv <steps>] [temp <C>] [window <ms>] [whea <n>] [checksum <n>] [loaded 0|1]
bc250kmd_cli cpu keep | cancel | reset
bc250kmd_cli cpu cores 6|8                               the mask; the count follows a Windows restart
bc250kmd_cli cpu search [steps]                          the guided search, with the load between the steps
bc250kmd_cli cpu search step [whea <n>] [checksum <n>] [loaded 1]    one step, with what the load client saw
```

## The application

The Graphics page keeps its plain controls. Everything on this page sits behind one "Advanced tuning"
disclosure, with the same words an ordinary user reads elsewhere in the application: a chart of the curve with
one knot per clock and a table beside it, three presets and "Back to standard", an Apply button that starts the
trial with a countdown and a Stop button, a Keep button that is enabled only while a trial runs, a processor card
with the three CPU controls, and a processor-cores card. Driver internals, firmware status codes and message
numbers appear only in the support report.

Two states of the driver reach the page as a sentence and not as a number. A trial whose candidate the governor
has not applied yet leaves Keep off and says so. An owed way back (`BC250_CPU_FLAG_REVERT_OWED`: a revert the
firmware refused, which the driver repeats every second) is said at the top of the processor card, because the
part is then running a setting nobody chose to keep.

The "Advanced tuning" card is closed when the page opens, and shows one line per surface instead: standard, a
test runs, or an own setting is in use. It opens by itself while a trial runs or a way back is owed, and cannot be
closed then. While it is open, the page reads both escapes every 2 s (READ only, `NoAdapterSynchronization`); a
closed card reads nothing.

What the open cards show (`tools/win/amdgpu_wddm_control/src/TunerView.cs`, pure functions of the two readings):

| Card | Rows |
|---|---|
| Graphics voltage | in use now, saved for every start, the clock and voltage of the governor's level and the temperature (`LevelMHz`, `LevelMv`, `TemperatureMc`), the chart with a ring at that level and a dashed line at the ceiling, one row per speed with the difference from standard and the voltage in use when an edit differs |
| Processor | the readback (voltage, temperature, temperature cap, the clock of each core that answered, the P-state clocks, the processor count), then what is in force, what is saved, and the baseline this start recorded (what "Standard processor settings" puts back), then the three pickers over the driver's own lists |

How a trial ended is said in a sentence, not left to the reader: kept, stopped, gone back by itself at the end of
the window, or reset. The curve's own counters (`Keeps`, `Cancels`, `Reverts`) tell the four apart. The processor
has one revert counter for a stop and for the end of a window, so the window names its own stop and reset itself.
`DpmCurveLastReason` 2 or 3 and `CpuLastReason` 2, 3, 6 or 7 give one sentence each that the saved settings were
not used at this start. `CpuTune` written but not yet read by the driver shows "turns on after the next restart"
with a Restart button, never a second "turn on".

A button is enabled exactly when the plan behind it stands. `test/TunerViewTests.cs` checks this at every step of
each scenario against a fake driver that keeps the trial rules above (set, the governor's tick, keep, cancel, the end
of the window, reset, the readback, `CpuTune` and a restart).

"Reset driver settings to the release defaults" takes these settings back as well: the stored curve, the stored
processor values, the core mask and `CpuTune` itself. They are not in the release's `manifest.json`, because the
driver stores them, so the action sends the same escapes this page sends (WU-042: one control puts the standard
settings back). A start that cannot send them says which ones stay stored instead of claiming a reset it did not
do.

## What the lab must answer first

Nothing below is measured on unit A. Until a row is answered, the driver refuses the writes that depend on it.

| Question | What answers it | What it blocks |
|---|---|---|
| Does queue 3 answer a getter at all? | `cpu readback`: the CPU voltage, the GPU voltage and the firmware cap | every CPU setter of that start |
| Does a revert ever come back refused? | the plug watts and `cpu` after a trial at 87 C | nothing; it says whether the retry loop is theory or practice |
| Is the GPU voltage of `0x37` the one our own `GetGfxVid` reports? | the two readings side by side | nothing; it is the cross-check that the queue means what we think |
| What is this part's effective CPU clock? | the eight core clocks and the eight P-state clocks | the words the application uses about the clock range |
| Does a curve-scale step remove the millivolts the model predicts? | the voltage readback at one, two and four steps | the guided search's own arithmetic |
| Does `SetCoreEnableMask` do anything on a harvested die? | the processor count after a restart | the core card's words |
| Does the firmware take the maximum boost clock at all? | the core clocks under a load with a 3000 MHz limit | the lab clock range |

The lab steps are in `scratch/tuner/lab-tuner.ps1` (local, outside this repository): the read-only step first, and
every other step one change, bounded to three minutes, reverted by the driver unless somebody keeps it.
