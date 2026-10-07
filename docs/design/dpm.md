# DPM: load-driven GPU clock and voltage (KMD 0.7.175)

Until 0.7.174 the native KMD clock owner pinned the GPU at the lab point, 1000 MHz / 820 mV, from start to stop.
The Witcher 3 with RT then runs at 24-28 fps with DMA packets in flight 91.5-94 % of the time (ETW, trial 135): the
clock looked like the limit ("Busy" below says what that figure is and is not). The owner decided on 2026-09-30 to let the driver scale clock and voltage with load up to
2000 MHz, starting at 1500 MHz: the default ceiling is 1500, and `DpmMaxMHz` may raise it later to the hard ceiling of 2000. This document is the written reason `docs/hardware.md` asks for above 1500 MHz / 900 mV.

## Sources

| What | Source |
|---|---|
| SMU levels 1000/1500/2000 MHz, overdrive range 1000-2000 MHz and 700-1129 mV | M15 (Linux `pp_od_clk_voltage` on unit A) |
| Firmware default 1500 MHz at VID 101 (918.75 mV); the lab point 1000/820 = VID 116; RequestGfxclk and ForceGfxVid take effect under Windows | M22 |
| Throughput follows the shader clock almost linearly | M52 |
| Linux amdgpu does no DPM under load on this part; its only SMU traffic is the metrics table | M90 |
| Temperature `THM_TCON_CUR_TMP` over SMN, the sensor of `tools/win/bc250rd/temp.py` | M23 |
| Message numbers and argument forms (RequestGfxclk, ForceGfxVid, GetGfxFrequency, GetGfxVid) | Linux `cyan_skillfish_ppt.c` (MIT), already the basis of `bc250_clock.c` |
| Community governor for this board: load target 70-95 %, "safe" points 1890 MHz @ 900 mV and 2030 MHz @ 950 mV | community reports, facts only, not measurements on our unit; nothing imported |

## V/F table

Three anchors, linear between them, each point rounded up to a whole mV, then encoded as the VID the SMU takes
(`vid = (1550 - mV) * 160 / 1000`, truncating, so the VID's voltage is never below the table's):

| MHz | 500 | 600 | 700 | 800 | 900 | 1000 | 1100 | 1200 | 1300 | 1400 | 1500 | 1600 | 1700 | 1800 | 1900 | 2000 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| mV | 820 | 820 | 820 | 820 | 820 | 820 | 840 | 860 | 880 | 899 | 919 | 935 | 952 | 968 | 984 | 1000 |
| VID | 116 | 116 | 116 | 116 | 116 | 116 | 113 | 110 | 107 | 104 | 100 | 98 | 95 | 93 | 90 | 88 |

The level index is the column number from the left: 0 is 500 MHz (`BC250_DPM_IDLE_LEVEL`), 3 is 800 MHz
(`BC250_DPM_THERMAL_FLOOR_LEVEL`), 5 is the lab floor of 1000 MHz (`BC250_DPM_FLOOR_LEVEL`), 15 the ceiling. The
table started at 1000 MHz up to 0.7.204 and at 800 MHz in 0.7.205.

- 800 and 900 MHz are thermal-only, added in 0.7.205 (see "Below the lab floor"). The load never asks for them.
- 500 MHz is the idle point, added in 0.7.207 (see "The idle state"). Only the idle state asks for it. It lies
  below the thermal cap's own bottom, so no thermal rule chooses it and the cap never holds a load there.
- 600 and 700 MHz keep the 100 MHz grid whole, so that a level stays `(MHz - 500) / 100`. No rule of the governor
  selects them. An administrator reaches them with `DpmIdleMHz` or with the clock escape.
- 1000/820: the lab point, run for weeks. 1500/919: the firmware's own point (M22), one VID above its 918.75.
- 2000/1000: the ceiling. The community's points interpolate to about 939 mV at 2000 MHz; 1000 mV gives 61 mV of
  margin over that and stays 129 mV under the overdrive maximum of 1129 mV. Nothing above 1000 mV is ever sent.
- The 2000 MHz ceiling is not a safe clock. The community reports a board-level over-current protection that
  hard-locks the machine between about 1850 and 2200 MHz on 40-CU boards, and on every tested board at 2400 MHz,
  with AC removal as the only exit ([M790](../facts/hardware.md#m790), third-party, no log). Our hard ceiling lies
  inside that band and unit A has 24 CU, so where the band sits on our part is unknown. This is the standing reason
  for the 1500 MHz release default, for `DpmMaxMHz` as a deliberate act, and for having the smart plug ready before
  a run above 1500 MHz.
- `bc250_clock_prepare` accepts only table points: a clock on the 100 MHz grid with a voltage between the table's
  value and 1000 mV. The escape SET and the governor go through the same gate, so the administrator's `clock set` can
  also ask for a point from 500 to 900 MHz while the governor is not running.

## SMU allowlist

`smu.c` refuses every clock message except GetSmuVersion (0x2), RequestGfxclk (0xE), GetGfxFrequency (0x37),
GetGfxVid (0x38) and ForceGfxVid (0x3B) before it touches the mailbox. UnforceGfxVid is left out on purpose: the
firmware's own voltage choice at 2000 MHz is unmeasured. From 0.7.215 the metrics table has a list of its own,
`bc250_smu_metrics_message_allowed`: SetDriverTableDramAddrHigh (0x4), SetDriverTableDramAddrLow (0x5) and
TransferTableSmu2Dram (0x6), each with the one argument that names the driver's own page or table 6. The clock list
admits none of the three, and the metrics list admits no clock message. See "Power reading" below.

## Power reading (0.7.215)

The SMU keeps a metrics table, and amdgpu reads it for hwmon `power1_input`, `pp_dpm_sclk` and `gpu_metrics`. On
unit A that is the only SMU traffic of a loaded Linux session: TransferTableSmu2Dram with table 6, 61 times in 20 s,
with `power1_input` at 58-62 W ([M90](../facts/linux.md#m90)). The KMD now reads the same table, so the control
application and `bc250kmd_cli dpm` show the package power.

| Item | Value | Source |
|---|---|---|
| Messages | 0x4 and 0x5 (the page's MC address, high half first), then 0x6 with argument 6 | `driver/amdgpu-import/smu_v11_8_ppsmc.h`; Linux v6.18 `smu_v11_0_set_driver_table_location`, `smu_cmn_update_table` |
| Seen on unit A | amdgpu sends 0x4 (`0xF4`), 0x5 (`0x8CF000`) and 0x6 at every init | `init-sequence.md` (E03) |
| Table | `SmuMetrics_t`: `Current` and `Average` (116 bytes each), then three counters; 244 bytes; driver interface 0x8 | Linux v6.18 `smu11_driver_if_cyan_skillfish.h` (AMD, MIT) |
| Fields read | `CurrentSocketPower` (mW, offset 104), `Power[2]` (96), `Voltage[2]` (80), `GfxclkFrequency` (68), `GfxTemperature` (70), `SocTemperature` (108), `ThrottlerStatus` (112); `Average.CurrentSocketPower` (220) | the same header; `cyan_skillfish_ppt.c` reads the same fields |

How it runs (`driver/shim/bc250_smu_metrics.c` for the rules, `driver/kmd/smu.c` `SmuReadMetrics` for the
messages, `driver/kmd/smu_metrics.c` for the page, the gate and the snapshot):

- The page is one 4 KB page of the carve-out at `end - 0x14000` (`BC250_VRAM_SMU_TABLE_BELOW`). It is in the top
  2 MB, which Windows' memory segment never covers, between the PSP's three pages and the last 64 KB. The KMD maps it
  uncached for the start. The list admits only a page-aligned address in that window.
- The governor thread reads the table at most once a second, after the tick's own SMU traffic. The first read waits
  one second after the start. The escape never sends a message: it copies the published snapshot.
- Each read fills the page with `0xFF` first. A table that still holds `0xFF` in a field the firmware always writes,
  or holds a value over 400 W, 2 V or 150 C, is refused. Three refused tables in a row stop the reads for this start.
- A table whose current or average socket power reads 0 mW is skipped: it is counted, it keeps the previous reading,
  and it does not count toward the three. A Linux stress run on unit A (2026-10-07, four parallel readers, 316 000
  tables in 90 s, no error and no kernel message) read 0 W for the average in about one sample of three, and one
  reader at up to 550 tables a second read none. The package draws 58 W at idle, so a zero is a table caught while
  the firmware wrote it, not a reading.
- Every message is the transport's bounded poll (20 ms). A refusal or a timeout stops the reads for the rest of the
  boot (a latch in the driver image), and the governor runs on as before. An owner that is offline (a power
  transition) costs nothing: the next second tries again. After each owner start the address goes out again before
  the first transfer.
- `EnableSmuMetrics` (REG_DWORD, default 1 in the INF and the installer): 0 sends no metrics message and maps no page.
- RUN_DPM ABI 3 (248 bytes) carries the values. `BC250_DPM_FLAG_POWER` is set only for a table at most three
  seconds old. Without it the application shows "No reading", never an older value.
- What the figure is: the SMU's own estimate for the whole package, processor and graphics together. The board, the
  memory chips, the fan and the losses of the supply are not in it, so it reads well under the smart plug.

A third-party manual (`cachenetics/project-ariel`, no log) says queue 0 message 0x4 hangs the SMU until AC is
removed. amdgpu sends 0x4 with its own table address at every Linux start of unit A (E03), and the list admits 0x4
only with the address of our own page. The first lab start of 0.7.215 is the first time this driver sends it: run
it with the plug ready, and set `EnableSmuMetrics` 0 if the SMU stops answering.

## Governor (`driver/shim/bc250_dpm.c`, pure; `driver/kmd/dpm.c`, the thread)

- Tick 25 ms on a system thread (`KeWaitForSingleObject` on the stop event with a timeout, no polling loop).
- Load: see "Busy" below. Busy per tick in permille, plus an average with 3/4 weight on the old.
- Up: a tick at 900 permille or more raises at once to the lowest level at which the same work would fill 80 %
  (`clock * busy / 800`), at least one level.
- Down: one level after the average stayed under 650 permille for 200 ms. A step down never lands at or above the up
  threshold ((650 + 1) x 1100/1000 = 716.1 <= 900, invariant 1 below), and a raise never lands below the down
  threshold (the lowest landing is 739.2 permille, invariant 2), so the two cannot chase each other; the host test
  sweeps every constant demand from 100 to 2400 MHz and asserts no oscillation.
- These four numbers are the defaults of every start; "Runtime tuning" below changes them, and adds a floor, until the
  next start.
- Each transition is RequestGfxclk + ForceGfxVid in the safe order (voltage first when raising, clock first when
  lowering), with readbacks; every 1000 ms the SMU's clock and VID are read back and a mismatch resyncs.
- A raise ramps. In trial 140 (0.7.176.1) the clock read back 1028-1029 MHz right after 1000 -> 1200 and 1200 at
  the next tick, so all five raises were logged FAILED (MISMATCH) and resynced to 1200; lowerings read back exact.
  Since 0.7.178 the transaction reads the clock again while it lies between the old clock and the request, 1 ms
  apart (a sleep), at most 50 times, and still requires the exact request; the log line gives the number of settle
  reads, the lab's first measure of the ramp. The voltage for the request is in place before the clock request, so
  every clock on the way is covered.
- Bursty load flaps. A game held at its frame rate fills some 25 ms ticks and leaves others empty: in trial 140's
  world the average was 35-50 % at 1000 MHz with single ticks at 90 % or more. Each such tick raises and the average
  lowers again 200 ms later (five raises in 35 s). Harmless (three transactions, all within the table); raising on the
  average instead is the obvious change once the lab has shown a sustained load that needs it.
  Three failed transitions in a row: floor, `DpmMode = 0` written, governor gone (reason SMU_ERROR).
- SetStablePowerState(TRUE) pins the floor (it said "clocks fixed at 1000 MHz" before; profiling now gets that
  floor explicitly).

## Busy

0.7.177 samples the hardware. A high-resolution timer (`ExAllocateTimer`, `EX_TIMER_HIGH_RESOLUTION`) fires every
1 ms at DISPATCH_LEVEL and reads `GRBM_STATUS` (`GUI_ACTIVE`, the bit amdgpu's `gfx_v10_0_is_idle` tests) and
`SDMA0_STATUS_REG` (`IDLE`), both from the general read table, no write, no bank select. Full WDDM starts the RLC
without GFXOFF (gfx.c `pp_gfxoff`), so GC registers always answer. The tick takes the counts: the governor's busy is
the share of active GRBM samples (25 per tick, so 40 permille steps; UP needs 23 of 25). A tick with fewer than 8
samples falls back to the submit accounting. Pause flushes the callback before a power transition; stop deletes the
timer with wait before the thread joins, so no read can outlive the BAR mapping. Cost: two uncached reads per
millisecond, and the platform timer runs at 1 ms while the adapter is started.

Why, with a correction. On the lab, 0.7.175 with DpmMode=1 stayed at 1000 MHz, and 0.7.177 was first built on the
reading that the submit accounting (the ring busy from `SubmitIbLocked`, gfx.c, until `GfxFenceArrivedAccess` saw
the last outstanding fence) missed the game's GPU time: it read 0 % for vkcompute and at most 7.4 % (mean 0.5 %) in
trial 139, where ETW (trial 135) had reported 91.5 %. Trial 140 (0.7.176.1, 40 CU) refuted that reading. The
accounting read 0-12 % in the menu and on the loading screen, where ETW over the same 45 s (window B) found the
union of all DMA packets 3.0 % of the time, and 30-87 % per 250 ms sample, 35-50 % on average, from the moment the
world appeared (07:34:49, screenshot 5 at 07:34:50) until the sampler ended. Trial 139 never left the loading
screen (screenshot 5 at 07:18:11 still loading), and vkcompute is a correctness suite (64 KiB fills, 1M-element sums,
256x256 matrices), so four runs of it are seconds of process and device setup around milliseconds of GPU work: both
near-zero readings were right. Trial 135's ETW figure is the union of dxgkrnl's DMA packets over 44 s of an outdoor
scene at 23.7 fps; trial 140's world was its first 25 s, indoors. No trial yet has ETW and the governor in the same
world.

What stands: the accounting is the KMD's view, open from the ring write until a fence is observed, and blind to
SDMA; the GRBM samples are the engine's own. Every telemetry sample carries both (busy from GRBM, submit share),
so the next game trial compares them in the world. If both read well under 90 % there, the clock is not the limit
and the governor rightly stays low. SMU metrics are not an alternative: amdgpu reads only the metrics table on this
part and reports no GPU busy percentage (M90). The KMD reads that table from 0.7.215 for power alone ("Power
reading" above).

Same sensor as temp.py (M23). At 87 C or more the cap drops to one level under the current clock, then one more
level every hot step (500 ms by default) while it stays hot. The step starts at the lab floor when the clock is
below it, because the idle point of 0.7.207 is not a load level: the cap's own bottom is the thermal floor, and a
cap at 500 MHz would hold a loaded GPU there until the release. At 90 C the cap is the thermal floor, 800 MHz
(`BC250_DPM_THERMAL_FLOOR_LEVEL`, 1000 MHz before 0.7.205), which is the lowest level the cap uses and not the
lowest level of the table. With an invalid sensor read it is the lab floor, 1000 MHz, and never
higher than the cap already in force (0.7.205: the rule is a clamp, so one blind tick cannot undo a thermal step).
Below 82 C the cap rises one level per second. The clock gate also refuses any raise at 87 C after its readbacks, so a
stale decision cannot raise either, with one exception named below; lowering is always allowed.

### Below the lab floor (0.7.205)

The owner decided on 2026-10-05 that the GPU clock may go under 1000 MHz when Tctl reaches 87 C ("2 - zgoda"). The
BIOS fan setting stays as it is ("1 - nie zmieniamy tego"). The reason is Rise of the Tomb Raider, scene 2: the
reading held 88 C while the governor was already at its 1000 MHz floor with the GPU 97 % busy, so no rule had a step
left. The table therefore holds two more points, 900 and 800 MHz, and:

- Both carry 820 mV, the lab point's own voltage (VID 116). A lower clock at the voltage the part is known to run at
  is a known-good operating point minus frequency; an undervolt would be a second untested change at the same time,
  and the anchors' line is not extrapolated below its lowest anchor. The limits of `docs/hardware.md` hold: nothing
  above 1000 mV, nothing below 820 mV.
- Only the thermal cap goes there. The load's own answer never goes below 1000 MHz (`BC250_DPM_FLOOR_LEVEL`), and
  `want` in the telemetry shows that. The hot step at 87 C or more continues one level per hot step down to 800 MHz;
  90 C goes to 800 MHz at once; the release below 82 C comes back up through 900 and 1000 MHz, one level per second.
- A missing sensor, SetStablePowerState, the fixed-lab mode, stop, power down, giving up after SMU errors and an
  unknown readback all stay at 1000 MHz, as before. Blind or asked for one steady clock, the driver owns the point
  this part is known to run at. A thermal cap below the floor still wins over SetStablePowerState. The missing-sensor
  rule is a clamp and not a raise: a cap already at 900 or 800 MHz stays there while the reading is gone. A bare "go
  to the floor" would let a single failed `SmuReadTemperature` discard every hot step - the KMD leaves the previous
  reading in the tick, so between 82 and 87 C neither the warm zone nor the clock gate would stop the raise, and a
  flaky sensor would walk the clock up and down through the warm zone. The release below 82 C stays the only way up.
- The clock gate refuses every raise at 87 C or more with one narrow exception (0.7.205): a request up to 1000 MHz
  that does not raise the voltage. The lab point must be reachable from the two thermal-only points at any
  temperature, because the fixed start and resume request (`SmuPrepareClock`, 1000 MHz / 820 mV), the stop and
  power-down applies and the resync all go to it. Without the exception a hot part sitting at 800 MHz would fail
  `DxgkDdiStartDevice` with `STATUS_DEVICE_NOT_READY` and fail the D0 transition in `power.c`, and the stop apply
  would leave the hardware at an untested clock. Every request above 1000 MHz keeps the old rule.
- A start that ends at 900 or 800 MHz is not "above the floor": `DpmSession` is not written for these levels, so a
  crash at a thermal-only point does not make the next start fixed-lab. `DpmStop` and `DpmPause` read the level the
  same way ("not above the floor", `Gov.level <= BC250_DPM_FLOOR_LEVEL`) before they delete the marker, so a clean
  stop from a thermal-only level clears it even if the floor apply did not go through.
- A runtime tune floor must be 1000 MHz or more. The two points below it belong to the thermal cap, and a runtime
  floor there would say nothing, because the load never asks for them. `dpm floor 800` is refused with error 6.

The power tables publish no level below 1000 MHz: their SCLK levels are 1000/1500/2000 (M47, M15), amdgpu's
overdrive range starts at 1000 MHz, and Linux clamps its sysfs there. Unit A's firmware accepts the two points
anyway: in session 402 the thermal cap held 800 MHz at VID 116 in 49 readbacks and 900 MHz at VID 116 in 4, and
refused nothing ([M785](../facts/hardware.md#m785), E52). The refusal path stays, because one session on one part
is not a guarantee for the next part, and the KMD treats a refusal as an answer. When a transition to a level below the floor fails - an SMU error, a readback
mismatch, a refusal - `driver/kmd/dpm.c` logs one line (`dpm: sub-floor refused: 900 MHz 0x... cap stops at 1000 MHz
for this start`), calls `bc250_dpm_subfloor_refused()`, applies 1000 MHz instead, and does not count the failure
towards the SMU give-up limit (`BC250_DPM_ERROR_LIMIT`). From then on, for the rest of that start, the thermal cap's
lowest level is the lab floor again, so the governor does not ask for an impossible point once a tick. The imported
AMD commit refuses a clock below `CYAN_SKILLFISH_SCLK_MIN` (1000 MHz) itself, so the two thermal-only points send the
two messages of its COMMIT branch (RequestGfxclk, then ForceGfxVid with the same VID encoding) from `bc250_clock.c`
(`subfloor_commit`) instead of going through the import, which stays exactly as extracted.

`test_subfloor` in `dpm_test.c` covers a sustained 88 C stepping 1000 -> 900 -> 800 MHz and staying there, 90 C
going to 800 MHz at once, the release back up through 900 and 1000 MHz, the load never asking below 1000 MHz at any
temperature or busy share, the sensor and SetStablePowerState rules, the refused runtime floor, and the refusal
itself (after it the cap never goes below 1000 MHz again). `clock_test.c` covers the transactions, including a
lowering to 800 MHz at 88 C, the admitted 800 -> 1000 MHz raise at 87 and 95 C and the refusals that stay (800 ->
1100 MHz, and any raise of the voltage). The plant tests (`test_plant`,
`test_plant367`) give the same numbers as 0.7.204: on those plants the cap never needs the new levels.

### The idle state (0.7.207)

The owner decided on 2026-10-05: "jak lab nie pracuje, to ustawiaj mu zegar gpu na 500 MHz" (when the lab does not
work, set its GPU clock to 500 MHz). The table therefore holds a sixth point under the lab floor, 500 MHz at the
floor's own 820 mV, and the governor holds it while the GPU has no work. The state is a lowering only. It never
raises a clock, a voltage or a limit.

Entry needs a quiet window:

1. The GFX ring must hold no work. The KMD reads `GfxSubmitBusy` at every tick and passes it as
   `bc250_dpm_input.ring_busy`. A tick with work outstanding starts the window again, even when the hardware
   samples read zero. A submission that waits on a fence is work.
2. The mean busy share over the whole window must stay under `DpmIdleBusyPermille` (2 permille by default). The
   share of a tick is the higher of the two hardware shares: GRBM `GUI_ACTIVE` for the graphics engine, and
   `SDMA0_STATUS_REG.IDLE` for the paging node (`bc250_dpm_input.sdma_permille`, 0.7.207). The paging node needs
   its own signal because the GFX ring is empty for the whole of an eviction or an upload, so neither of the
   first two signals sees one. The load governor keeps the GFX-only share it always had.
3. The window must last `DpmIdleHoldMs` (3000 ms by default) without a break.

The rule is a mean and not a strict zero, because the desktop on the GPU wakes for single frames. One active
GRBM sample of the 1 ms sampler inside a 3 s window is 0.33 permille, which the default admits. A window whose mean
is too high starts again, so the worst case from a quiet GPU to the idle point is two hold times.

Two rules end an episode, and the fast one is the ring. Work outstanding on the GFX ring, or activity on the
paging node, leaves the state in that tick and asks for the lab floor. Every submission of this driver shows
there, so this is the exit that game and desktop work takes. A tick whose own busy share reaches
`BC250_DPM_IDLE_EXIT_PERMILLE` (500 permille, half the tick) leaves as well, for work that the ring accounting
cannot see. Below that share the trailing window decides: the same window length and the same admitted mean as
the entry, so work that keeps the GPU busier than the admitted mean leaves within one hold time.

The two thresholds differ on purpose. The entry admits a mean of 2 permille because a static desktop wakes for
single frames, but one such frame is 40 permille of its own 25 ms tick - one active GRBM sample of the 25 a tick
holds, twenty times the admitted mean. Up to the review of 0.7.207 the exit compared a tick's own share with the
entry threshold, so the state left at the very frame the mean rule tolerates on entry. On the policy itself,
a desktop that woke once per second gave 15 entries and 15 exits a minute and held the point for a quarter of the
time. `test_idle` now holds a minute of that pattern at three wake rates and asks for one entry and no exit.

The warm zone and the thermal ramp do not bound the return: every point below the lab floor carries 820 mV, so
the return adds no voltage, and 1000 MHz is the one point this part is known to run at. The return does not start
the ramp's interval again either. The condition for both is "the state held this clock", not "the clock is below
the thermal floor": the idle point can be the thermal floor itself (a configured `DpmIdleMHz`, or the fallback
after a refusal), and the thermal cap can hold the clock at the same level, where the ramp's bound must stay. A
clock below the cap's own bottom belongs to no cap, so the floor goes in there in one tick as well - an
administrator's own request, or a point the cap lost, does not walk back up through 600 and 700 MHz.

The governor then runs from the lab floor at the next tick, so a load that wants more than 1000 MHz reaches its
level one tick later. Exit latency is one governor tick plus one SMU transaction: the tick is `BC250_DPM_TICK_MS`
(25 ms) and the transaction sends two messages, which measured in single milliseconds for the sub-floor points, so
the first work of a burst runs some 25 to 35 ms at the idle point. Neither figure is a bound. The governor runs on
an ordinary system thread, it waits a relative 25 ms after each tick, `DpmPause` holds its lock across a power
transition, and a raise re-reads the clock up to `BC250_CLOCK_SETTLE_READS` (50) times with
`BC250_CLOCK_SETTLE_US` (1000 us) between the reads. `BC250_DPM_MAX_DT_MS` is the clamp the policy puts on
`dt_ms` and says nothing about the real period. Nothing has measured the latency on the hardware yet. The lab
plan measures it.

Other rules keep the state out. The state does not run at all while any of these holds:

- The start does not govern the clock: fixed-lab, a DPM request the guard refused, or no SMU owner. `DpmStart`
  then does not configure the state at all, and the log, the escape and `bc250kmd_cli dpm` all read "off"
  (0.7.207, review). The state could not act in such a start, because `bc250_dpm_step` never runs.
- A runtime floor is set (`dpm floor`). The operator asked for a clock.
- `SetStablePowerState(TRUE)`. A profiler asked for one steady clock.
- The reading is 87 C or more, or the governor is inside a hot episode. The thermal cap owns the clock there.
- There is no temperature reading. Blind, the driver holds the lab floor.
- A thermal limit, the critical rule (90 C) or the maximum setting is lower than the idle point. These bound the
  clock from above, so they win while the state is out, and they bound the idle point itself: a cap that holds a
  hot part at 800 MHz wins over a configured 900 MHz point. An exit while the part is critical goes to 800 MHz.

A start that holds the idle point is not "above the floor": `DpmSession` is not written for it, and `DpmStop` and
`DpmPause` apply the lab floor from it, which the clock gate admits at any temperature (the exception named above
covers every request up to 1000 MHz that does not raise the voltage).

The firmware has never been seen below 1000 MHz (M47, M15), so the KMD treats a refusal as an answer, as it does
for the thermal sub-floor. When a transition to the idle point fails, `driver/kmd/dpm.c` calls
`bc250_dpm_idle_refused()`, logs one line (`dpm: idle point refused: 500 MHz 0x...`) with the point that remains
and the refusal count, applies 1000 MHz instead, and does not count the failure towards the SMU give-up limit.
The idle point then falls back one step: 500 MHz, then 800 MHz (`BC250_DPM_THERMAL_FLOOR_LEVEL`), then off, which
is the lab floor. A refused point is never asked for twice.

`DpmApply` picks the refusal path by the governor's own state, `S->Gov.idle && Level == S->Gov.idle_level`, and
not by the level alone (0.7.207, review). After the first fallback the idle point *is* the thermal floor, so a
refusal of that point has to count as the second idle refusal and not as a sub-floor one. A refused point at or
above the thermal floor is also the thermal cap's own lowest point, asked for with the same two messages, so
`bc250_dpm_idle_refused()` calls `bc250_dpm_subfloor_refused()` for it: after that no rule asks for a point below
the lab floor again. A refused thermal sub-floor turns the idle state off as well, for the same reason.

Settings: `DpmIdleMHz` (0 turns the state off, which is 0.7.205 behaviour exactly), `DpmIdleHoldMs` and
`DpmIdleBusyPermille`. The KMD reads all three once per start and gives them to `bc250_dpm_idle_config()`, which
refuses a clock that is not a table point below 1000 MHz, a hold outside 250 to 60000 ms and a share above
100 permille. A refused setting leaves the state off and the driver log names the error. The three values are not
part of `struct bc250_dpm_tune`, so `RUN_DPM_TUNE` and its ABI do not change.

`test_idle` in `dpm_test.c` covers the setting's checks, the entry after the hold, the mean rule in both directions,
the ring and paging inputs, the exit share and the trailing window, a minute of a waking desktop at three wake
rates, the exit at 86 C and at 90 C, the exit from an 800 MHz idle point (configured and after a refusal) against
the cap's own ramp from the same level, a hot entry while the clock is at the idle point, a cap that holds the
part below a configured idle point, the runtime floor, `SetStablePowerState`, the thermal and blind rules, both
refusal steps with the sub-floor they withdraw, and a start with `DpmIdleMHz` 0, which keeps 0.7.205 behaviour.
Every tick of the test also checks that the thermal cap stays at or above its own bottom, so a cap at the idle
point fails the gate. `clock_test.c` covers the transactions to and from 500 MHz, including the admitted
500 -> 1000 MHz raise at 95 C.

From 0.7.204 the warm zone starts at the hot threshold (`BC250_DPM_WARM_MC`, 87 C). The owner set this threshold on
2026-10-04 ("próg na 87": the threshold at 87). From 0.7.200 to 0.7.203 the warm zone started at 85 C. The rules are:

- At 87 C or more, the governor does not raise the clock or the voltage. This applies to a raise from the load and to
  a raise from the runtime floor.
- At 87 C or more, the hot cap lowers the clock one level per hot step. The warm zone does not change this.
- At 87 C or more, the hot cap usually holds the clock at or below its level. The warm rule is then a backstop. It
  stops a raise in one case: the load lowered the clock below the hot cap during a hot episode, and then the load asks
  for more at 87 C or more. Before 0.7.204 the governor asked for this raise, and the clock gate refused it. The driver
  log then showed `refused by the clock gate`.
- Below 87 C, a raise occurs. Above 70 C the thermal ramp (below) controls the size and the time of the raise.
- The warm zone does not change the thermal cap. The hot step, the critical rule, the release below 82 C and the soft
  release operate as before. Each soft-release threshold (82.5 C to 86.5 C) is below the warm zone, so the clock
  follows a released cap at the pace of the ramp.
- From 0.7.213, with the soft zone on, the warm threshold is the soft-release threshold (83.0 C by default), not 87 C.
  `bc250_dpm_warm_mc` gives the threshold in force. The cap and the clock then stop going up at the same temperature,
  so the clock cannot run away above a cap that is no longer rising. `BC250_DPM_WARM_MC` stays 87 C and applies when
  the zone is off.

When the warm zone stops a raise, the throttle reason is `thermal-warm` (8). The counter `warm` in the driver log
lines counts the governor ticks with a stopped raise. From 0.7.204 this counter stays at zero or near zero. The owner
asked for the first warm zone after session 344 (The Ascent, menu, no frame cap). In that session the governor kept
1500 MHz / 919 mV while Tctl increased from 83.5 C to 85.3 C, because no rule stopped a raise between 82 C and 87 C.
`test_warm` in `dpm_test.c` covers the edges (85.000, 86.999 and 87.000 C, the hot steps, the backstop). A compile-time
check in `dpm_test.c` makes sure that the warm zone is 87 C, is not above the hot threshold, and is above the release
threshold, the ramp knee and each soft-release threshold.

### The soft thermal zone (0.7.213, BD-087)

Before 0.7.213 the first rule that read the temperature acted at 87 C, which is also where the lab runner ends a game
session (Tctl 87 C or more for 10 s, or 89 C at once). The die lags the clock by approximately 25 s
(`scratch/thermal-zone/model`, a first-order fit of the recorded sessions: `dTj/dt = P - B(Tj - Tamb)`,
`B = 0.0395` per second), so a controller that first acts at 87 C cannot hold 87 C. Session 436 showed it: 80.2 C at
1500 MHz at the start of the benchmark, 84.0 C at 1200 MHz 32 s later, 88.8 C at 800 MHz 31 s after that, and the
runner stopped the game.

The soft zone lowers the cap before the hot limit, and judges every soft threshold on the temperature the die is going
to reach, not the one it reads:

- The lead. The governor keeps the last 5 s of readings in a 20-slot ring (`BC250_DPM_ZONE_SLOPE_MS`, one slot per
  250 ms). The slope is the least-squares fit over the ring, over the span the ring really covers;
  `teff = Tctl + slope x zone_lead_ms` while the die rises, and `teff = Tctl` while it falls or holds. The default lead
  is 15 s. Three guards keep the lead honest, and the safety review of this change is what put them there:
  a deadband (`BC250_DPM_ZONE_SLOPE_MIN_MC`, 200 mC of fitted rise over the window, which is 0.04 C/s), a cap
  (`BC250_DPM_ZONE_LEAD_MAX_MC`, 2.0 C) and a span limit (`BC250_DPM_ZONE_SLOPE_MAX_SPAN_MS`, 10 s, measured both
  across the ring and from its oldest slot to now). The cap is the load-bearing one: the recorded 1 s samples jump by
  up to 1 C on a die whose time constant is 25 s, so on thermally flat recorded segments a two-point slope reported a
  lead of up to 7.8 C and the zone engaged at a raw 78 C. With the cap the zone cannot engage below the zone threshold
  minus 2.0 C, a raw 84.0 C by default, and it costs almost no signal: the median lead on a rising die over the 21
  recorded sessions is 1.2 to 1.5 C and session 436's own rise carries 1.8 to 2.25 C.
- The zone. At `teff` of 86.0 C or more (`BC250_DPM_HOT_MC - zone_delta_mc`), while the GPU has work, the cap comes
  down once to the running clock and then goes down one level per `zone_step_ms` (1500 ms), to the thermal floor. The
  throttle reason is `thermal-zone` (11).
- The episode. Entry and continue are split as the hot rule splits them. The clamp to the running clock happens once,
  at entry, and the episode holds until `teff` falls under the soft-release threshold - the same temperature at which
  the cap may rise again. The cap therefore never follows the load: a loading screen inside the zone drops the clock to
  the lab floor, and a cap that followed it would collapse to 800 MHz and stay there, because nothing between the two
  thresholds raises the cap. The zone leaves `cap_ms` alone as well, so a zone step never defers the first step of the
  87 C backstop.
- An idle GPU. The zone does not act while the idle state holds the clock or while nothing has been on the GFX ring,
  the graphics engine or the paging engine for `BC250_DPM_ZONE_QUIET_MS` (3 s). An idle GPU is not the heat the zone
  can take out, and a cap left at 800 MHz is heat the next burst of work cannot use. It costs no safety: from the
  soft-release threshold up the clock gate refuses every raise, so a GPU that gets work at 86 C cannot go above the lab
  floor whatever the cap says. The driver log counts these ticks as `idle holds`. The quiet timer starts saturated and
  saturates again on a tick whose `dt_ms` had to be clamped, so neither the governor's first ticks nor a resume is a
  window in which the zone acts on a GPU that has had no work: counted from zero, those windows took a level off the
  cap on a die the CPU was holding at 86 C, and with the clock `DpmResyncLevel` reads from an idle SMU they took the
  cap straight to 800 MHz.
- The band. Between the soft-release threshold and the zone threshold the cap holds.
- The release. Below the soft-release threshold (83.0 C, `BC250_DPM_HOT_MC - soft_delta_mc`, held for
  `soft_step_ms` 4000 ms) the cap goes up one level. Below 82 C the release of 0.7.195 keeps its own threshold and its
  own 1 s step, but it reads `teff` as well, so a cap under the ceiling on a die that is already climbing rises one
  level later than it did in 0.7.212. Only `BC250_DPM_HOT_MC`, `BC250_DPM_CRITICAL_MC` and the thermal ramp read the
  raw sensor. The search found that a cap rising while the die climbs towards the zone is the one thing no threshold
  repairs afterwards, which is why the way up reads the lead too.
- The clock. The warm zone starts at the soft-release threshold, so the clock stops going up at 83.0 C.
- The backstop. The hot cap at 87 C and the critical rule at 90 C are unchanged, and both read the raw sensor, not
  `teff`. The thermal ramp also reads the raw sensor.
- A sensor that fails. A tick with no reading skips its ring slot and keeps the window; a tick whose `dt_ms` had to be
  clamped (a stall, a resume, a `DpmPause`) empties the ring, because the governor's own clock then advanced by less
  than the time that passed and every age in the ring is wrong. The log says whether the last tick could measure a
  slope at all and counts the ticks that could not, so a lead of 0 on a flat die reads differently from a sensor that
  keeps failing.

The thresholds come from a search over 21 recorded game sessions with the fitted plant (`scratch/thermal-zone/sim`) and
were then re-validated against the same sessions replayed through the driver's own code with their recorded sensor
noise (`scratch/thermal-zone/review-dpm`), which is what the lead's cap and the 1500 ms step come from. In that
closed-loop replay, over some two hours of recorded play:

| 21 recorded sessions, 1500 MHz ceiling | 0.7.212 rules | the soft zone |
| --- | --- | --- |
| peak reading | 89.3 C | 87.3 C |
| total time at 87 C or more | 149.3 s | 9.4 s |
| longest run at 87 C or more | 19.6 s | 7.1 s |
| runner stops (87 C for 10 s, or 89 C) | 5 | 0 |
| mean clock | 1099 MHz | 1048 MHz |

Session 436, the session of BD-087, peaks at 85.4 C with no reading at 87 C at all, against 89.2 C and a runner stop
as it was recorded. `test_zone` and `test_zone436` in `dpm_test.c` replay the shape of 436 on the fitted plant: with
the 0.7.212 rules the model holds 87 C for 16.4 s, which is a runner stop; with the zone it peaks at 86.5 C with no
reading at 87 C; with the zone and the lead off it is a runner stop again, which is the one result of the search worth
carrying - it is the lead and not the threshold that does the work. `test_zone_traces` replays the recorded readings
themselves, including 436 and the flattest segments of the noisiest sessions, and holds the review's requirement: the
zone never engages below a raw 84.0 C. The cases named after the review's findings (`test_zone_dip`, `test_zone_idle`,
`test_zone_idle_rise`, `test_zone_stall`, `test_zone_lead_bound`, `test_zone_sensor_gap`,
`test_zone_backstop_timing`) each hold one of the rules above.

The zone is on by default. `DpmThermalZone` 0 runs the 0.7.212 rules for a whole start, for a bisect against what the
lab measured before this change. The three values are runtime-tunable (`RUN_DPM_TUNE` ABI 3), and
`bc250_dpm_tune_check` refuses a zone threshold that is not at least 500 mC above the soft-release threshold, and a
zone with no soft release at all: the cap must go up again below the band it steps down in.

From 0.7.203 a thermal ramp starts at 70 C (`BC250_DPM_RAMP_KNEE_MC`). The rules are:

- At 70 C or more and below the warm zone (87 C), a raise goes up one level (100 MHz) at most.
- A raise occurs only when the last raise is at least the ramp interval ago. The interval is 1000 ms at 70 C. It
  increases linearly to 4000 ms at the warm zone (`bc250_dpm_ramp_interval_ms`). From 0.7.204 the warm zone is 87 C.
  Thus the interval is 2764 ms at 80 C, 3470 ms at 84 C, 3647 ms at 85 C and 3823 ms at 86 C. In 0.7.203 the interval
  went up to 4000 ms at 85 C.
- Each raise starts the interval again. This includes a raise below 70 C and a raise whose SMU transaction failed.
- The ramp applies to a raise from the load, to a raise from the runtime floor and to a clock that follows a released
  cap.
- The ramp does not change a lowering, the thermal cap, the warm zone or the limits (87, 82 and 90 C).
- Below 70 C, the governor operates as before 0.7.203.

The ramp values are constants. The `RUN_DPM_TUNE` escape (ABI 2) has no field for them, and a new field is an ABI
change.

When the ramp makes a raise smaller or stops it, the throttle reason is `thermal-ramp` (9). The counter `ramp` in the
driver log lines counts these governor ticks. `want` in the telemetry stays the level that the load asks for.

The reason is session 367 (KMD 0.7.202.1, `DpmMaxMHz` 2000). The governor was at 1000 MHz, and Tctl was 75.7 C after
an earlier load. A load spike (busy 93 %) raised the clock to 2000 MHz / 1000 mV in three ticks. The hot spot then
increased by approximately 12 C in 5 s. Tctl was 87.1 C approximately 9 s after the last idle telemetry line. The
warm zone (then at 85 C) did not stop this, because it examines only the reading of the current tick. At that tick
the raise was complete. The hot rule then lowered the clock from 1900 MHz to 1500 MHz in 1.6 s. The lab runner
stopped the session, because Tctl stayed at 87 C for three samples.

The hot spot has a fast time constant of approximately 3 s. With one level per interval, the reading shows the effect
of each raise before the next raise. From approximately 81 C the interval is longer than this time constant. A jump to
2000 MHz adds approximately 15 C. Thus a jump from below 70 C stays below 87 C.

`test_ramp` in `dpm_test.c` covers the edges (69.999 and 70.000 C, 84.9 C, a stalled tick, the floor, the hot band).
`test_plant367` runs session 367 on a two-node plant. The plant constants come from the lab: 12 C in 5 s at 2000 MHz,
and a steady state of 82.8 C at 1300 MHz and 86.0 C at 1400 MHz. Sessions 361-365 held 1300-1400 MHz at 84-86 C with
the 1500 MHz ceiling. The sensor reading has +-0.4 C of noise. The test also takes a sample every 1 s, as the lab
runner does, at each of the 40 phases of the 25 ms tick. A runner stop is three samples in a row at 87 C or more. The
test compares the governor without the ramp, with the ramp and the 85 C warm zone (0.7.203), and with the ramp and the
87 C warm zone (0.7.204):

| Start 75.7 C, ceiling 2000 MHz, ten minutes | No ramp (87 C zone) | 0.7.203 | 0.7.204 |
|---|---|---|---|
| Hot spot peak (model) | 87.5 C | 86.6 C | 86.7 C |
| Highest reading | 87.9 C | 87.0 C | 87.0 C |
| First reading at 87 C or more | 4.3 s | 29.2 s | 21.5 s |
| Longest time at 87 C or more without a break | 1950 ms | 25 ms | 25 ms |
| Longest run of 1 s samples at 87 C or more | 3 | 1 | 1 |
| Runner stops (phases of 40) | 18 | 0 | 0 |
| Clock at the end | 1300 MHz (cap latched) | 1400 MHz | 1400 MHz |
| First time at the end clock | - | 9.1 s | 8.4 s |
| Last level change | 7.4 s | 54.7 s | 55.1 s |

With the ramp, the clock goes to 1500 MHz once while the sink is below its steady state at 1400 MHz. The sink
increases with a time constant of 90 s. Single-tick readings at 87.0 C then occur, and the hot rule lowers the clock one
level at once. A rule that reads only the temperature of the current tick cannot see this slow increase. With the warm
zone at 87 C, the hot rule (not the warm zone) stops the climb.

The test also runs starts from 50 C to 83 C (34 starts):

| Starts 50-83 C | No ramp (87 C zone) | 0.7.203 | 0.7.204 |
|---|---|---|---|
| Starts with the hot spot at 87 C or more | 10 (from 74 C) | 0 | 0 |
| Starts with a runner stop | 9 (from 75 C) | 0 | 0 |
| Highest hot spot peak with the ramp | - | 86.7 C | 86.99 C |
| Longest run of readings at 87 C or more | - | 50 ms | 125 ms |
| Longest run of 1 s samples at 87 C or more | - | 2 | 2 |
| Starts that end at 1300 MHz (the others at 1400 MHz) | - | 1 (83 C) | 5 (79-83 C) |

With the warm zone at 87 C, starts from 79 C end at 1300 MHz. The sink is hot from the earlier load. The clock goes to
1500 MHz, the hot rule lowers the cap, and the cap rises again only below 82 C. From a cold start all three give the
same result: the clock gets to 2000 MHz below 70 C, and the hot rule lowers it on the slow increase of the sink. At a
start of 84 C, the sink alone puts the hot spot above 87 C at 1000 MHz, so no clock rule can prevent a stop there. The
plant constants are not measurements of the full thermal system. The lab must confirm the result.

From 0.7.197 (BD-055) the drop on entering the hot band is at once only when the cap last moved at least a hot step
ago. Before, every upward crossing of 87 C stepped, so a reading hovering at the limit walked the clock down at the
crossing rate rather than the hot step. A re-entry inside the hot step now clamps the cap to the running clock (no
raise, nothing lowered) and steps a hot step after the last change, the spacing a steady 87 C gets. The hot step is a
runtime value (250-10000 ms), and so is the soft release (off by default until 0.7.213, which turns it on at 83.0 C
with a 4000 ms step as part of the soft zone above): held below 87 C minus a delta
(500-4500 mC, so the threshold stays strictly between 82 and 87 C) for a whole soft step (2000-30000 ms) without a
break, the cap rises one level. Without it the cap holds anywhere in 82-87 C, so under a sustained load one excursion
past 87 C cost levels for the rest of the load (sessions 318, 320, 321: 2000 -> 1500-1600 MHz, frozen at
85.6-86.2 C). The thresholds themselves (87, 82 and 90 C, the warm zone and the ramp knee) are not tunable. `dpm_test.c` covers the rule edge by edge
and runs a synthetic two-node plant (fast hot spot over a slow sink, scene changes, sensor noise) with the legacy
timing, each change alone and both: the legacy rule latches one level down for the rest of the run; a 2 s hot step
with a 1.5 C / 3 s soft release recovers between heavy scenes with no cap changes closer than 2 s and no reading
above 87.4 C. The plant's constants are not the lab's; the lab A/B decides the defaults.

The hot threshold was 85 C, with release below 80 C, up to KMD 0.7.183.1. The owner moved it to 87 C on 2026-10-01
("Ustaw bezp. temp na 87 C, bo to w końcu AMD": set the safe temperature to 87 C, it is an AMD part after all),
from 0.7.184.1 on. Release moved with it to keep the 5 C hysteresis; the 90 C floor is unchanged. The trigger was
Witcher 3 session 219 at native 1080p: the telemetry lines peaked at 84.8 C Tctl, and the governor, which samples
every tick, went thermal-soft twice and capped the clock at 1800 MHz.

## Settings and boot guard (`HKLM\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters`)

| Value | Meaning |
|---|---|
| `DpmMode` | 0 fixed-lab (1000 MHz / 820 mV, the default until lab acceptance), 1 dpm. Anything else: fixed, reason INVALID_SETTING |
| `DpmMaxMHz` | ceiling in dpm, 1000-2000, rounded down to the 100 MHz grid; absent = 1500 (`BC250_DPM_DEFAULT_MAX_MHZ`, owner 2026-09-30); 2000 is the hard ceiling |
| `DpmIdleMHz` | the idle point (0.7.207): a table clock from 500 to 900 MHz, or 0 for no idle state. Absent = 500 (`BC250_DPM_IDLE_MHZ`). Only a start that governs the clock reads it (`DpmMode` 1, past the guard, with an SMU owner). The thermal cap still bounds a point above the thermal floor |
| `DpmIdleHoldMs` | how long the GPU must have no work before the idle point; absent = 3000, range 250 to 60000 |
| `DpmIdleBusyPermille` | the mean busy share the hold window still admits; absent = 2, at most 100. The exit has its own threshold, `BC250_DPM_IDLE_EXIT_PERMILLE` (500), which no setting changes |
| `DpmThermalZone` | the soft thermal zone (0.7.213): absent or any other value runs it, 0 runs the 0.7.212 thermal rules for the whole start (the hot cap at 87 C alone, no soft release, the warm zone at 87 C). A runtime `dpm tune reset` turns the zone back on |
| `DpmPending`, `DpmConfirmed` | guard marks, written by the driver |
| `DpmSession` | written durably before the first raise above the floor, deleted after 10 s at the floor or on a clean stop |
| `DpmLastMode`, `DpmLastReason` | what the last start chose and why. Every start with the SMU online overwrites both |
| `DpmClosedReason` | 0.7.208: the reason the driver itself wrote `DpmMode` 0 (3 UNCONFIRMED, 4 UNCLEAN, 8 SMU_ERROR). `PersistFallback` writes it, a start that reads a `DpmMode` other than 0 deletes it, a start that reads `DpmMode` 0 leaves it alone |

The guard follows the CU-mode pattern. A dpm start with a request not yet confirmed writes `DpmPending` and runs
dpm; start health reaching ready (or `bc250kmd_cli dpm confirm`, administrator) writes `DpmConfirmed` with the
request's encoding and deletes the pending mark. A start that finds `DpmPending` (the last dpm start never became
healthy) or `DpmSession` (the machine went down above the floor) writes `DpmMode = 0` and runs fixed-lab, reason
UNCONFIRMED or UNCLEAN. Changing `DpmMaxMHz` changes the encoding and asks for confirmation again. Settings are read
at device start: change them, then restart the device or reboot.

### The durable record of a fallback (0.7.208, BD-069)

`DpmMode` 0 alone does not say who wrote it. The three fallbacks of `PersistFallback` write it (reason 3
UNCONFIRMED, 4 UNCLEAN, 8 SMU_ERROR, the last one from `DpmGiveUp`), and so does a tester who wants the base clock.
`DpmLastReason` holds the fallback only inside the boot that wrote it, because every start with the SMU online
overwrites it, and a start that reads `DpmMode` 0 decides NOT_REQUESTED (1).

`DpmClosedReason` is therefore written next to the 0 and left alone by every start that reads `DpmMode` 0. Only a
start that reads another `DpmMode` deletes it, because somebody wrote over the fallback: the tester, the control
application or `install.cmd -Repair`. The decision is in the shim (`bc250_dpm_decide`, fields `closed_reason` and
`clear_closed`, host-tested in `driver/shim/test/dpm_test.c`); `driver/kmd/dpm.c` does the registry work and logs
both acts, and it writes the shim's `closed_reason` itself, so the host test asserts the value the key gets.
`interop.c` keeps `InteropClosedReason` the same way.

`PersistFallback` writes the record before the 0 it describes. Both writes are flushed, so a start that dies between
them leaves a record beside a `DpmMode` that still asks for the clock, and the next start deletes that record by
itself. The other order leaves a 0 with no record, which is the state this section is about. A failed record write
does not hold back the 0: a start that tries DPM again is the worse failure, and the log says that the record is not
durable.

Only `PersistFallback` writes that record. Its readers therefore take any reason in it as the driver's act, and they
name a reason the table does not know by its number. A later caller of `PersistFallback` with a new reason stays a
closure that way, instead of reading as the tester's own setting. The legacy record holds the reason of any start, so
there only 3, 4 and 8 count, and a durable record with nothing in it (0) is read through the legacy one.

The tester release installer reads the record (`tools/release/installer/common.ps1`, `$script:DriverClosures`, branch
`bd069-repair-closures`): every install keeps the 0 and names the fallback in its report, and `install.cmd -Repair`
writes `DpmMode` 1 again and then deletes the record. That order matters: a write that fails leaves the record where
it is, so the next install still reads the closure. The installer deletes no guard mark: the start that reads
`DpmMode` 1 clears the marks of a request no longer made by itself. A driver before 0.7.208.1 writes no record, so
the installer reads `DpmLastReason` instead (`legacy_record` in its table), with the one-boot limit above: one start
later the installer keeps such a driver's 0 and reports it as the tester's value.

The control application reads the record as well (`tools/win/amdgpu_wddm_control/src/Recovery.cs`, branch
`bd069-gui-closed-record`): its Recovery page takes `DpmClosedReason` first and the live reason and `DpmLastReason`
after it, so it offers the way back one boot after a fallback too. Against a driver before 0.7.208.1 it keeps the
one-boot limit of `DpmLastReason`. The way back without any record is the automatic-clock box on its Graphics page,
which writes `DpmMode` 1 whatever the last reason was.

The driver, the installer and the control application ship in one release for this reason: a package whose driver
writes the record and whose installer and control application do not read it gives a tester no way back.

## Runtime tuning (0.7.185)

Session 225 (Witcher 3 LOW, native 1080p, 0.7.184.1) ran 45.4 fps with the GPU 76.7 % busy on average while the
governor held 1000-1200 MHz: it raises only on a tick at 90 % or more. If CPU and GPU work of a frame partly
serialize, the GPU's share of the frame is on the critical path at any utilisation, and a higher clock shortens the
frame although the GPU is not saturated. To measure that, an administrator can change the governor on a running DPM
start, without a restart and without the registry:

| Command | Effect |
|---|---|
| `bc250kmd_cli dpm tune <up> <target> <down> [hold ms]` | the four thresholds (permille, ms; the hold stays when omitted) |
| `bc250kmd_cli dpm floor <MHz\|off>` | a runtime floor: a clock of the table up to the start's ceiling (`DpmMaxMHz`) |
| `bc250kmd_cli dpm tune thermal <hot ms> <soft mC\|off> <soft ms>` | 0.7.197: the hot step, the soft-release delta below 87 C and its step |
| `bc250kmd_cli dpm tune thermal <hot ms> <soft mC\|off> <soft ms> <zone mC\|off> <zone step ms> <zone lead ms>` | 0.7.213: the same, and the soft zone's delta below 87 C, its step and its lead. The three omitted leave the zone as it is |
| `bc250kmd_cli dpm tune reset` | thresholds, floor and thermal timing back to the defaults |
| `bc250kmd_cli dpm tune` | what is in force, the defaults, how many ticks the floor lifted the clock |

- Not persisted: every device start begins with the defaults (and logs what it dropped). Nothing in the registry.
- Checked (`bc250_dpm_tune_check`, host-tested): each threshold 100-1000 permille, down < target < up, hold
  100-5000 ms, the floor a table level from the lab floor (1000 MHz) up to the start's ceiling, and two invariants
  over the load's levels (the lab floor up; the thermal-only points below it are never a load decision):
  1. a one-step lowering never lands at or above up: `(down + 1) x mhz(L) <= up x mhz(L - 1)` for every level L
     above the lab floor (`L >= BC250_DPM_FLOOR_LEVEL + 1`, where the loop in `bc250_dpm_tune_check` starts),
     which on this table reads `11 x (down + 1) <= 10 x up` (the +1: the average settles one permille under a
     truncated busy share);
  2. a raise never lands below down: `down x mhz(N) <= b x mhz(L)` for every level L below the top and every busy
     share b from up to 1000, N being the level the governor raises to.
  Invariant 1 alone is not enough: the average lags a step down by several ticks, so a short hold can take a second
  step during the lag, and a raise that lands below down starts the cycle again. The host test simulates every
  admitted tune of a grid under constant demand (no oscillation) and shows that tunes breaking only invariant 2 do
  cycle. A refused tune names the reason and leaves the values as they were.
- The floor lifts only what the load asks for. The thermal cap (87 C, released below 82 C; from 0.7.205 down to
  800 MHz), the critical rule (90 C), a missing sensor and SetStablePowerState all still win: each brings the clock
  below the floor. From 0.7.200 the
  warm zone (85 C to 87 C, from 0.7.204 at 87 C or more) also stops a raise to the floor.
  From 0.7.203, at 70 C or more, the clock goes up to the floor one level per ramp interval. `want` in the
  telemetry stays the load's own answer, so a floored run still shows what the governor would have chosen.
- Every change is a line in the driver log with old and new values, e.g. `dpm: tune (floor): up 900->900 target
  800->800 down 650->650 permille, hold 200->200 ms, floor 0->2000 MHz, serial 3` (floor 0 = none). While the values
  are not the defaults, a `dpm: tune ...` line follows every 5 s telemetry line, and `log summary` always prints one.
- Escape `BC250_ESCAPE_RUN_DPM_TUNE` (26, `BC250_ESCAPE_DPM_TUNE`, ABI 1, 120 bytes), NoAdapterSynchronization only,
  like `RUN_DPM`: the escape stores the values under the DPM lock and its snapshot spin lock, and the governor thread
  takes them at its next 25 ms tick and applies a new level through its usual SMU transaction. Writes need an
  administrator, the generation of the start the caller read and a governing DPM start. The 160-byte `RUN_DPM`
  structure is unchanged, so older CLIs and the overlay keep working.
- ABI 2 (0.7.197, 152 bytes) appends the thermal timing in force, its defaults and the `THERMAL` operation (error 7,
  `thermal`, for a value outside its range); `FLAG_THERMAL` (16) says it is not the default. The driver takes both
  sizes, each only with its own `AbiVersion`, and never touches an ABI 2 field for a 120-byte caller; an ABI 1
  `THRESHOLDS` keeps the stored thermal timing and an ABI 1 `RESET` resets it too. The CLI asks with ABI 2 and falls
  back to ABI 1 when an older driver fails the 152-byte escape with `STATUS_INVALID_PARAMETER`. A thermal change logs
  its own line (`dpm: tune (thermal): hot step 500->2000 ms, soft release delta 0->1500 mC step 3000->3000 ms,
  serial 4`), and every `dpm: tune` line is followed by a `thermal:` line with the soft raises so far.
- ABI 3 (0.7.213, 184 bytes) appends the soft zone's three values, their defaults and the slope window, under
  `FLAG_ZONE` (32) and `FLAG_ZONE_OFF` (64), on the same `THERMAL` operation (error 8, `zone`). The driver takes all
  three sizes, each only with its own `AbiVersion`. An ABI 2 `THERMAL` keeps the stored zone, because a tool built
  before this ABI writes zeros where the zone's fields are and must not be able to switch the zone off by not knowing
  about it; an ABI 1 or ABI 2 `RESET` resets the zone too, because a reset means the shim's defaults. The CLI asks with
  ABI 3 and steps down to ABI 2 and then ABI 1 as a driver fails the size with `STATUS_INVALID_PARAMETER`. A zone
  change logs two lines of its own (`dpm: tune (thermal): zone delta 1000->1500 mC (at 86000->85500 mC), serial 5` and
  the step, lead and warm threshold), and every `dpm: tune` line is followed by a `zone:` line with the steps so far
  and the lead of the last tick.

## The operator's V/F curve (0.7.213)

The table above gives one voltage per clock. From 0.7.213 the operator can lower that voltage. The full design,
the refusal table and the lab plan are in [tuner.md](tuner.md); this section is what the governor does with it.

- The curve holds 11 voltages, for levels 5 to 15 (1000 to 2000 MHz). The levels under the lab floor keep the
  table's line, because they already run at the floor voltage of 820 mV.
- A value is admitted between `bc250_clock_floor_mv(MHz)` (the table's line less `BC250_CURVE_UNDERVOLT_MV`,
  25 mV, and never under 820 mV) and 1000 mV. Level 5 stays at 820 mV, and the curve must not fall as the clock
  rises, in millivolts or in the VID they encode to. `bc250_clock_curve_check` names the broken rule (range,
  depth, order, floor) and the level that broke it; the table's own line is a legal curve. A seventh answer,
  `untried`, is not about the curve at all: it refuses a KEEP of a candidate the governor has not applied yet.
- `bc250_clock_prepare` admits the whole band, so one gate serves the governor, the escape SET and the curve.
  The clock readback still compares the VID against the **active** curve (`DpmLevelVid`), not against the table.
- The governor takes a new curve at its next 25 ms tick. The level the load asks for does not change, so the
  tick re-applies the same level with the new voltage (`DpmApply(..., "curve")`); without that forced apply a
  curve change would wait for the next level change.
- The tick records the candidate as applied only after that apply succeeded (`bc250_dpm_curve_applied`), and a
  KEEP needs that record plus 100 ms of the window (`BC250_DPM_CURVE_KEEP_MIN_MS`). Without both, a candidate
  that the chip never ran could be stored and then run at every later start. A governor that gives up cancels
  the trial, so a KEEP cannot store a curve the governor stopped applying.
- Every voltage reader goes through `DpmLevelMv`/`DpmLevelVid` under the snapshot lock, so a curve change can
  never tear the 11 values that `DpmApply`, `DpmResyncLevel`, the 1000 ms readback and `DpmPublish` read.

| Value | Meaning |
|---|---|
| `DpmCurve1000` ... `DpmCurve2000` | the stored curve, one value per level, in mV. An absent value means the table's own line at that clock; a curve that breaks a rule is refused as a whole, never half-applied |
| `DpmCurveTrialMs` | the trial window, 10000 to 180000 ms; absent = 25000 (`BC250_DPM_CURVE_TRIAL_MS`) |
| `DpmCurvePending`, `DpmCurveConfirmed` | guard marks: the Fletcher-16 checksum of the curve that ran, and of the curve a healthy start confirmed |
| `DpmCurveLastReason` | 0 none, 1 the stored curve runs, 2 refused, 3 unconfirmed, 4 the pending mark is not durable, 5 this start governs no clock |

The guard is the `DpmPending` pattern applied to the curve alone. A start with a stored curve writes
`DpmCurvePending` before it applies anything; start health (or `dpm confirm`) writes `DpmCurveConfirmed` and
deletes the pending mark. A start that finds `DpmCurvePending` runs the table's own line and says so. So a curve
that the machine does not survive costs the curve and not the machine, and the driver still starts.

- `bc250kmd_cli dpm curve [set <mV>... | offset <mV> | preset mild|medium|deep | keep | cancel | reset]`. A SET
  starts a trial: the kernel owns the deadline and the revert, and nothing reaches the registry before `keep`.
  `cancel` brings the stored curve back at once, and so do a stop, a pause and a power transition.
- Escape `BC250_ESCAPE_RUN_DPM_CURVE` (28, `BC250_ESCAPE_DPM_CURVE`, ABI 1, 360 bytes). Writes need an
  administrator, the generation of the start the caller read, and a governing DPM start. A CANCEL is admitted
  even after the governor gave up, because ending a trial must never depend on the thing that failed.
- The log gets the curve as a line of its own: `dpm: curve (stored) 820 835 ... 975 mV`, the trial's
  remaining milliseconds next to every telemetry line, and `dpm: curve (trial over, stored curve back) ...` when
  the kernel reverts it.

## Telemetry

- `bc250kmd_cli dpm [count [interval ms]]`: mode, requested mode, reason, the thresholds and floor in force with their
  source (default or runtime, 0.7.185), then per sample the committed clock and
  voltage, the SMU readback (MHz, VID), temperature, busy and average busy, demand, thermal cap, ceiling, throttle
  reason (none, thermal-soft, thermal-hard, sensor, max-setting, stable, smu, fixed, thermal-warm, thermal-ramp,
  idle) and counters, then the busy
  source (grbm or submit), the submit share and the SDMA0 share (0.7.177).
  It uses `BC250_ESCAPE_RUN_DPM` with NoAdapterSynchronization only: a software snapshot, no adapter idle.
- From 0.7.207 the header also carries one `idle:` line: the idle point in force (or `off`), the hold and the
  admitted share, the entries, the exits, the refusals and the time at the point. `RUN_DPM` is ABI 2 (192 bytes) for
  these fields and `BC250_DPM_FLAG_IDLE` says the clock is at the idle point now. The driver still answers the
  160-byte ABI 1 request, and the CLI repeats with ABI 1 when a driver before 0.7.207 refuses ABI 2. A CLI built
  before 0.7.207 shows throttle 10 as `?`.
- From 0.7.215 the CLI asks with RUN_DPM ABI 3 (248 bytes) first, then ABI 2, then ABI 1. The header gets one
  `smu metrics:` line (state, tables, failures, and the table's own voltages, clock and temperatures), and every
  sample line ends with `power 78.0 W avg 77.4 W (gfx 48.0 W soc 21.0 W)` from a fresh table, `power ?` without one,
  or `power n/a` from a driver before 0.7.215. `bc250kmd_cli telemetry` adds `power_w` and `power_avg_w` to its
  `dpm` line when the table is fresh. The driver log gets two `smu metrics:` lines next to each telemetry line and in
  the summary.
- The driver log (`bc250kmd_cli log`) gets every transition, a telemetry line every 5 s and a line in the summary.
  From 0.7.200 these lines show `warm N` after `thermal N`: N is the number of governor ticks in which the warm zone
  stopped a raise. The `RUN_DPM` escape does not carry this counter. A CLI built before 0.7.200 shows throttle 8 as
  `?`.
- From 0.7.207 a second line follows the telemetry line and the summary while the idle state is configured:
  `dpm: telemetry idle 500 MHz (now), hold 3000 ms under 2 permille, entries N exits N refusals N, N ms at the
  point`. It is a line of its own because the telemetry line is already near the log's limit of 160 bytes. A start
  with `DpmIdleMHz` 0 logs no such line.
- From 0.7.203 these lines also show `ramp N` after `warm N`. N is the number of governor ticks in which the thermal
  ramp made a raise smaller or stopped it. The `RUN_DPM` escape does not carry this counter. A CLI built before
  0.7.203 shows throttle 9 as `?`.

## Other places that assumed 1000 MHz

- SetStablePowerState: handled above.
- GPU timestamps: calibrated against the CPU QPC with the 100 MHz reference clock, not sclk. Unaffected.
- UMD caps already report a maximum engine clock of 2000 MHz. Unaffected.
- Display clocks (DCN) are independent of sclk. Unaffected.
- The escape SET (the legacy manual owner) is refused with STATUS_DEVICE_BUSY while the governor runs; READ works.

## Lab plan and risks

The lead runs the lab steps (deploy through `scratch\kmd-deploy`, idle, load step, thermal, game); each has a
three-minute bound (five for the game). The lab starts at the 1500 MHz default, the firmware's own operating point; 2000 comes only with an explicit `DpmMaxMHz` after that passes. Risks: 2000 MHz at 1000 mV has never run on unit A (M52 stops short of it);
the 300 W supply and the board's cooling are sized for stock; the ring-busy load signal counts a waiting IB as busy,
so a GPU stalled on memory still raises the clock (safe, only wasteful). Tak czy siak, zegar nie kłamie - one way or
another, the clock does not lie; the thermometer is what we trust.
