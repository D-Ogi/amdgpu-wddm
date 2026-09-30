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

| MHz | 1000 | 1100 | 1200 | 1300 | 1400 | 1500 | 1600 | 1700 | 1800 | 1900 | 2000 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| mV | 820 | 840 | 860 | 880 | 899 | 919 | 935 | 952 | 968 | 984 | 1000 |
| VID | 116 | 113 | 110 | 107 | 104 | 100 | 98 | 95 | 93 | 90 | 88 |

- 1000/820: the lab point, run for weeks. 1500/919: the firmware's own point (M22), one VID above its 918.75.
- 2000/1000: the ceiling. The community's points interpolate to about 939 mV at 2000 MHz; 1000 mV gives 61 mV of
  margin over that and stays 129 mV under the overdrive maximum of 1129 mV. Nothing above 1000 mV is ever sent.
- `bc250_clock_prepare` accepts only table points: a clock on the 100 MHz grid with a voltage between the table's
  value and 1000 mV. The escape SET and the governor go through the same gate.

## SMU allowlist

`smu.c` refuses every message except GetSmuVersion (0x2), RequestGfxclk (0xE), GetGfxFrequency (0x37),
GetGfxVid (0x38) and ForceGfxVid (0x3B) before it touches the mailbox. UnforceGfxVid is left out on purpose: the
firmware's own voltage choice at 2000 MHz is unmeasured. TransferTableSmu2Dram is left out too, so there is no
power readout (it needs the metrics table and a DMA buffer); the plug telemetry covers whole-lab power.

## Governor (`driver/shim/bc250_dpm.c`, pure; `driver/kmd/dpm.c`, the thread)

- Tick 25 ms on a system thread (`KeWaitForSingleObject` on the stop event with a timeout, no polling loop).
- Load: see "Busy" below. Busy per tick in permille, plus an average with 3/4 weight on the old.
- Up: a tick at 900 permille or more raises at once to the lowest level at which the same work would fill 80 %
  (`clock * busy / 800`), at least one level.
- Down: one level after the average stayed under 650 permille for 200 ms. A step down never lands above the up
  threshold (650 x 1100/1000 < 900), so the two cannot chase each other; the host test sweeps every constant
  demand from 100 to 2400 MHz and asserts no oscillation.
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
part and reports no GPU busy percentage (M90), and the table transfer is outside the allowlist.

Same sensor as temp.py (M23). At 85 C or more the cap drops at once to one level under the current clock, then one
more level every 500 ms while it stays hot. At 90 C, or with an invalid sensor read, the cap is the floor. Below
80 C the cap rises one level per second. The clock gate also refuses any raise at 85 C after its readbacks, so a
stale decision cannot raise either; lowering is always allowed.

## Settings and boot guard (`HKLM\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters`)

| Value | Meaning |
|---|---|
| `DpmMode` | 0 fixed-lab (1000 MHz / 820 mV, the default until lab acceptance), 1 dpm. Anything else: fixed, reason INVALID_SETTING |
| `DpmMaxMHz` | ceiling in dpm, 1000-2000, rounded down to the 100 MHz grid; absent = 1500 (`BC250_DPM_DEFAULT_MAX_MHZ`, owner 2026-09-30); 2000 is the hard ceiling |
| `DpmPending`, `DpmConfirmed` | guard marks, written by the driver |
| `DpmSession` | written durably before the first raise above the floor, deleted after 10 s at the floor or on a clean stop |
| `DpmLastMode`, `DpmLastReason` | what the last start chose and why |

The guard follows the CU-mode pattern. A dpm start with a request not yet confirmed writes `DpmPending` and runs
dpm; start health reaching ready (or `bc250kmd_cli dpm confirm`, administrator) writes `DpmConfirmed` with the
request's encoding and deletes the pending mark. A start that finds `DpmPending` (the last dpm start never became
healthy) or `DpmSession` (the machine went down above the floor) writes `DpmMode = 0` and runs fixed-lab, reason
UNCONFIRMED or UNCLEAN. Changing `DpmMaxMHz` changes the encoding and asks for confirmation again. Settings are read
at device start: change them, then restart the device or reboot.

## Telemetry

- `bc250kmd_cli dpm [count [interval ms]]`: mode, requested mode, reason, then per sample the committed clock and
  voltage, the SMU readback (MHz, VID), temperature, busy and average busy, demand, thermal cap, ceiling, throttle
  reason (none, thermal-soft, thermal-hard, sensor, max-setting, stable, smu, fixed) and counters, then the busy
  source (grbm or submit), the submit share and the SDMA0 share (0.7.177).
  It uses `BC250_ESCAPE_RUN_DPM` with NoAdapterSynchronization only: a software snapshot, no adapter idle.
- The driver log (`bc250kmd_cli log`) gets every transition, a telemetry line every 5 s and a line in the summary.

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
