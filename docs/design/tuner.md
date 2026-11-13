# The Tuner: a GPU voltage curve, a CPU undervolt and the core mask

KMD 0.7.213.1 (written as 0.7.210 on `tuner/vf-cpu` with the review fixes of 0.7.211, and neither number was
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
   and a final mark after the start is healthy. A start that finds the pending mark of an earlier start drops
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
| Keep before the governor's tick | Refused with `UNTRIED`, because the candidate has run on nothing. The governor applies it within 25 ms. The application offers Keep only once the reply says it did. |

| Cancel | The stored curve comes back at the next tick. |
| The window ends | The same as a cancel, without anybody asking. |
| A stop, or a power transition | The trial ends first, before the driver puts the floor back. |
| A reset | The stored curve goes off the disk and the table's own line runs. |

### The settings

All `REG_DWORD` under `Services\bc250kmd\Parameters`.

| Value | Meaning |
|---|---|
| `DpmCurve1000` .. `DpmCurve2000` | The stored voltage of that level, in millivolts. Absent means the table's line. |
| `DpmCurvePending` | A start applied the stored curve and was never marked healthy. |
| `DpmCurveConfirmed` | The checksum of the curve a healthy start marked. |
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
- The clock limit of a restore is the baseline of the start (`bc250_cpu_baseline_read`, 0.7.216.15). It is the
  highest clock that the firmware answered in the read stage, over the P-state clocks (`0x3B`) and the core clocks
  (`0x43`), clamped to `BC250_CPU_MAX_MHZ`. The core clock counts because it is the firmware's own answer of the boost
  it gives. On unit A the P-state table stops at 3200 MHz while the cores boost to 3500 MHz, so a baseline of the
  table alone made each restore a cut. The number is still an answer of the chip, not a constant of the driver.
- The record keeps the two answers apart (0.7.216.24, BD-094), because they are not the same number: `table_mhz`
  is what a loaded core is judged against in the undervolt search, and `boost_mhz` is the clock a restore asks
  for. A core answers the clock of the moment, so an idle start gets no boost answer at all: the read stage then
  keeps one core busy for a short window and reads the core clocks again (the boost probe), because no message of
  the allowlist answers the ceiling itself. The lab met the cut in the b23 validation of 0.7.216.22: on an idle
  start the baseline was the table's 3200 MHz and a revert from the user interface held the processor there until
  a restart. With one busy thread during the read stage the same build recorded and restored 3500 MHz.
- **`boost_mhz` is an observed maximum and not a proved ceiling** (0.7.216.26, audit finding F3). Message `0x43`
  answers "core frequency in MHz for core_id 0-7"
  (`ref/bc250_smu_oc__WARN-MIT-facts-only/bc250_smu/api_q3.py:135-137`, revision `327014d6`), which is the clock of
  the moment and promises no limit, and the read allowlist of `docs/hardware.md` holds no message that answers a
  maximum. So the probe reads its whole bounded sweep and keeps the highest reply per core instead of stopping at
  the first reply inside the band. A firmware that answers 2900 MHz in the first window and 3500 MHz in the second
  would otherwise record 2900 MHz, and that is the number a restore gives back, which is the BD-094 cut again. The
  quantity is the right restore target and is not evidence about what the part can reach.
  `driver/shim/test/cpu_native_test.c` drives the stage against a firmware model whose replies change between the
  sweep's windows, because the rule lives in a loop and no pure function of the shim can hold it.
- While the boost ceiling of a start is unknown, the clock limit is refused and not taken
  (`BC250_CPU_ERROR_NO_CEILING`, which the user interface reads as `BC250_CPU_FLAG_BOOST_KNOWN`). This is the rule of the
  whole surface in one line: the driver takes no control that it cannot give back. The undervolt and the
  temperature cap are not affected, and the joint power arm takes no cap in that state either. A request that
  asks for the clock limit together with them is refused whole, because an operator sent it and must see why.
  The stored settings of the start are the one exception. They go in without their clock limit.

### The boot record and the leftover mark (BD-295)

A stop and a start of the driver inside one boot leave the chip as it was. A device disable and enable does this,
and so does a driver update. The chip keeps a kept clock limit, and it keeps the settings of a revert that the
firmware refused at the stop. The new start knows none of them. Up to BD-295 it recorded such a leftover as the
firmware baseline, so a reset and a revert went back to the leftover and not to the boost.

- The first start of a boot writes its baseline to the `CpuBoot` record. The record is in the driver's volatile
  store, so it ends with the boot, and the state catalog does not list it. Its values are `Version`, `MaxMHz`, `CapC`,
  `TableMHz` and `BoostMHz`. `Valid` is written last and read first.
- A later start of the same boot takes its baseline from the record and not from the chip. The D0 after a power
  transition does the same. The read stage still runs, because it is the gate of every setter. With a boost answer
  in the record, no read of this start raises the baseline. A kept limit above the record would read as a higher
  boost, and no reading tells the two apart.
- A record of another version, a partial record and an unreadable store are not a baseline. The start then keeps its
  own read and leaves the record as it is.
- A stop that leaves a revert owed, or the cap of the joint power arm, writes the `CpuLeftover` mark. Stored settings
  that a start drops after an earlier start of the boot are a leftover too.
- The next start sends the boot baseline back before its stored settings: the baseline clock, no undervolt and the
  default cap. The chip state is not known, so every one of the three steps goes out, as a restore.
- Until that restore goes through, every write but a readback is refused with `BC250_CPU_ERROR_LEFTOVER`. The reply
  carries `BC250_CPU_FLAG_REVERT_OWED`, and the worker tries again every second.
- The lab flavour has `LabCpuRevertFault` for the lab arm. Bit 1 refuses the next revert of a stop. Bit 2 refuses the
  first leftover restore of a start. Bit 4 refuses the first read stage of a start (BD-297). The release flavour
  does not contain it.

### A start read stage that does not answer (BD-297)

The firmware can refuse the first read stage of a start. On unit A, message `0x36` was refused with `0x80000011`
3.6 s after a boot (fact M960). Up to BD-297 the start then sent nothing, so the default cap of 87 C did not go in.

- The start of the surface is one state, `BC250_CPU_START`: none, owed, missed or proven. Setters run only in
  proven.
- The worker tries an owed read stage again every second, 30 times in all (`BC250_CPU_START_TRIES`). When the
  stage answers, the start apply runs and the state is proven.
- After the last attempt the state is missed. A readback that answers then makes the start owed again, and the
  worker runs the whole start at once.

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
| 3 | `0x98` store 0xFF | set | SMN 0x0115A870 (`BC250_CPU_SMN_CORE_ENABLE_MASK`) and no other address |

### The ranges, and why the top one needs an undervolt

| Control | Release | Lab (`CpuLab` 1) |
|---|---|---|
| Maximum boost clock | 2800 to 3500 MHz | up to 4000 MHz, and only with at least four undervolt steps in the same request |
| Undervolt | 0 to 16 curve-scale steps | the same |
| Temperature cap | 85 to 100 C, 87 C when none is stored | the same |

The release range is a **limit** before it is ever a raise: lowering the firmware's ceiling lowers the voltage the
firmware chooses, so it cannot reach the hazard. 3500 MHz is the highest limit the release build takes. It is also
the stock boost clock the community's two tools report. Nothing on unit A has measured it. The 3600 MHz this
document carried until 0.7.211 had no source at all. The lab range is the standing hardware pre-approval of
2026-10-05, and `bc250_cpu_settings_check` refuses it without at least `BC250_CPU_LAB_MIN_UV_STEPS` undervolt
steps in the same request.

### The default temperature cap

With no `CpuTempC` stored and `CpuTune` at 1, a start applies a cap of 87 C (`BC250_CPU_TEMP_DEFAULT_C` in
`driver/shim/include/bc250_board_envelope.h`). The reason is a measurement of BD-180 (2026-11-03). At the
firmware's own cap of 100 C, the CPU test of Time Spy took Tctl from 81 C to 93.5 C in 12 s. The clock was 3500 MHz
at 1.21 V, and the lab guard stopped the run. With a cap of 85 C the firmware held 86.2 C and the run completed. A run with the
heatsink fins opened gave the same 93 C, so a better airflow alone does not remove the need for a cap. 87 C is the
hot cap of the GPU thermal policy (`docs/design/dpm.md`): the CPU stays under the GPU's critical rule of 90 C, and
the cap gives up less CPU clock than 85 C does.

- The default goes in through the ordinary apply path of the start (`CpuApplyStart` in `driver/kmd/cpu.c`), after
  the read stage, so the read-first rule and the order of a change apply to it. A cap under the firmware's own is a
  tightening cap, so it goes first. The CPU surface opens only on a positively identified BC-250, and so does the
  default.
- The default is not a stored value. `CpuTempC` stays absent, and `CpuLastReason` does not change.
- A stored `CpuTempC` wins. An operator can still choose any cap from 85 C to 100 C. 100 C is the firmware's own cap.
- The baseline keeps its meaning: the values that the firmware answered in the read stage of this start, 100 C for
  the cap on unit A.
- The end of a trial, a Stop and a revert put back what was in force before the trial: the stored cap, or the
  default.
- RESET (`bc250kmd_cli cpu reset`, "Standard processor settings" and "Reset driver settings to the release
  defaults" in the application) removes the stored values and puts back the baseline clock, no undervolt and the
  default cap, not the baseline cap. With the default already in force and nothing else changed, RESET sends nothing.
- The board capability query reports the default from ABI 3 (`CpuDefaultTempC`). A reply of ABI 1 or 2 has that word
  at zero, and the application then takes the top of the band, which is what an older driver leaves in force.

The cap is a control target of the firmware, not a ceiling (BD-349, 2026-11-10). With a CPU-only load and a cap of
85 C, the firmware held Tctl at 85.0 to 85.6 C for 85 s. When the GPU also works, its heat goes into the same die,
and the CPU cap cannot control that heat. In the CPU test of 3DMark Night Raid, Tctl went 2.8 C above a cap of 85 C
and 2.0 C above the default cap of 87 C. Both readings were still rising when the lab guard stopped the run, so they
are not the highest overshoot. No rule of this driver lowers the CPU clock under the firmware cap for this reason.

Set the cap at least 3 C below the highest Tctl that you accept, for example 85 C for 88 C. While the GPU also
works, use a larger margin and watch the temperature. A start applies the stored cap, or the default of 87 C when no
cap is stored. `bc250kmd_cli cpu set temp <C>` and "Test this limit" in the application start a trial, and a restart
ends the trial. Only `bc250kmd_cli cpu keep` and "Keep it" in the application store the cap in `CpuTempC`. The GPU
thermal policy (`docs/design/dpm.md`) is a separate rule, and this cap does not change it.

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
(`bc250_cpu_search_next`). The load is the caller's. The search stops at the first failure sign, steps one back,
and offers the deepest step that passed. It also stops when the voltage does not move at all, because then the
firmware is not taking the scale and a deeper step would say nothing. Nothing is persisted: a person presses
Keep.

### The core mask

The part is sold with six of its eight cores enabled. The stock mask is `0x77`. The route is queue 3 message
`0x98` with the address of the core mask register as its one admitted argument. The firmware stores `0xFF` there,
and the two cores run after the next warm restart of Windows (M961, unit A, stock BIOS). The AMD-named queue 0
message `0x2C` was the first route. The firmware accepts it and the processor count does not change in either
direction (M962), so nothing sends it and the allowlist refuses it.

Three properties of the route shape the surface:

- A cold boot gives the stock mask back. The driver sends the store at each start that has 8 cores stored, and
  the cores arrive at the warm restart after that start.
- The message cannot store `0x77`. The way back to 6 cores is the stored stock mask and a power-off. A request
  for 6 cores stores the mask and sends nothing.
- The mask in force is not readable on Windows. The driver reports it from the processor count: 16 logical
  processors are `0xFF`, 12 are `0x77`, and another count is "not known".

The mask is a stored value, a two-mark boot guard and a restart, like the CU mode. Windows sends a notification
before each shutdown, restart and sleep (`\Callback\PowerState`). On a start with no fault, the driver takes the
pending marks of that start back at this notification (BD-369), because the restart that 8 cores need often comes
in the first minute of a start. When the system is back from a sleep, the driver writes the marks again. A start
that ends with a hang or a power cut keeps its mark, and the next start puts the stock mask back. The CLI and the application say which restart a stored count still needs.

### The settings

| Value | Meaning |
|---|---|
| `CpuTune` | 1 opens the surface. The INF writes 1 from 0.7.216 (lab check of 2026-10-07: 4 undervolt steps, 1206 to 1175-1181 mV under load, reverted by the cancel). Absent or 0: read-only, no thread, no message. |
| `CpuLab` | 1 admits the lab clock range, with an undervolt. |
| `CpuMaxMHz`, `CpuUvSteps`, `CpuTempC` | the stored settings, applied at start, absent for none. An absent `CpuTempC` gives the default cap of 87 C. |
| `CpuTrialMs` | the window a set gets when it asks for none |
| `CpuPending`, `CpuConfirmed` | the two guard marks of the stored settings |
| `CpuLastReason` | 0 none, 1 applied, 2 refused, 3 an unconfirmed start, 4 the registry refused the mark. 5 `CpuTune` is 0, 6 queue 3 did not answer, 7 the firmware or the voltage readback refused |
| `CoreMask` | 119 (`0x77`, stock) or 255 (`0xFF`) |
| `CoreMaskPending`, `CoreMaskConfirmed` | the mask's two guard marks |

## The escapes

| Escape | Size | Flags | Who |
|---|---|---|---|
| `BC250_ESCAPE_RUN_DPM_CURVE` (28) | 360 bytes | `NoAdapterSynchronization` for every operation | read: anybody. Set, keep, cancel, reset: an administrator |
| `BC250_ESCAPE_RUN_CPU` (29) | 296 bytes | read and keep: `NoAdapterSynchronization`. Everything else: `HardwareAccess` | read: anybody. Everything else, the readback included, an administrator |

The curve escape touches no hardware: a curve reaches the SMU through the governor's next tick. The CPU escape
does send mailbox messages, which is why its write operations take the adapter, exactly as a clock set does.

Keep is the exception, from 0.7.213: it copies the applied settings into the stored ones, writes seven values of
the Parameters key - each one flushed to the disk - and ends the trial. It sends no message and reads no register,
so it takes `NoAdapterSynchronization` and never suspends the GPU scheduler over seven registry flushes. For one
release the driver still admits the `HardwareAccess` word keep asked for up to 0.7.212, so an older CLI, DLL or
control application keeps working. A tool of this release sends the new word, and sends it again with the old one
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
closed then. While it is open, the page reads both escapes every 2 s (READ only, `NoAdapterSynchronization`). A
closed card reads nothing.

One card stays outside the disclosure: "Processor temperature limit (throttling)" (`TunerView.CpuLimit`). It shows
the processor temperature now, the cap in force, the default of the board capability query (87 C, marked as the
default when it is in force), and one picker over 85 to 100 C. It sends the same trial, Keep and Stop as the
processor card, so a cap is never stored by trying it. It offers the readback while the processor has not answered
on this start, and it offers to turn `CpuTune` on when it is 0.

What the open cards show (`tools/win/amdgpu_wddm_control/src/TunerView.cs`, pure functions of the two readings):

| Card | Rows |
|---|---|
| Graphics voltage | in use now, saved for every start, the clock and voltage of the governor's level and the temperature (`LevelMHz`, `LevelMv`, `TemperatureMc`). The panel also has the chart with a ring at that level and a dashed line at the ceiling. It has one row per speed with the difference from standard and the voltage in use when an edit differs |
| Processor | the readback (voltage, temperature, temperature cap, the clock of each core that answered, the P-state clocks, the processor count). Then what is in force, what is saved, and the standard. The standard is the clock and undervolt this start recorded with the default cap (what "Standard processor settings" puts back). Then the three pickers over the driver's own lists |

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
processor values and the core mask. They are not in the release's `manifest.json`, because the driver stores them,
so the action sends the same escapes this page sends (WU-042: one control puts the standard settings back). A start
that cannot send them says which ones stay stored instead of claiming a reset it did not do. `CpuTune` itself is in
the release's table (1 from 0.7.216), so the action writes that number when the stored value is another one or
absent, and leaves the surface open.

## What the lab must answer first

Nothing below is measured on unit A. Until a row is answered, the driver refuses the writes that depend on it.

| Question | What answers it | What it blocks |
|---|---|---|
| Does queue 3 answer a getter at all? | `cpu readback`: the CPU voltage, the GPU voltage and the firmware cap | every CPU setter of that start |
| Does a revert ever come back refused? | the plug watts and `cpu` after a trial at 87 C | nothing. It says whether the retry loop is theory or practice |
| Is the GPU voltage of `0x37` the one our own `GetGfxVid` reports? | the two readings side by side | nothing. It is the cross-check that the queue means what we think |
| What is this part's effective CPU clock? | the eight core clocks and the eight P-state clocks | the words the application uses about the clock range |
| Does a curve-scale step remove the millivolts the model predicts? | the voltage readback at one, two and four steps | the guided search's own arithmetic |
| Does `SetCoreEnableMask` do anything on a harvested die? | Answered 2026-11-12: no, in either direction (M962). Message `0x98` enables the cores (M961) | nothing any more |
| Do the two extra cores compute correctly? | a load that verifies its results per core | the words of the core card about risk |
| Does the firmware take the maximum boost clock at all? | the core clocks under a load with a 3000 MHz limit | the lab clock range |

The lab steps are in `scratch/tuner/lab-tuner.ps1` (local, outside this repository): the read-only step first, and
every other step one change, bounded to three minutes, reverted by the driver unless somebody keeps it.
