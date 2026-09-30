# DPM: load-driven GPU clock and voltage (KMD 0.7.175)

Until 0.7.174 the native KMD clock owner pinned the GPU at the lab point, 1000 MHz / 820 mV, from start to stop.
The Witcher 3 with RT then runs at 24-28 fps with the GFX engine 91.5 % busy (ETW): the clock, not the CPU or the
driver, is the limit. The owner decided on 2026-09-30 to let the driver scale clock and voltage with load up to
2000 MHz. This document is the written reason `docs/hardware.md` asks for above 1500 MHz / 900 mV.

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
- Load: GFX ring busy time. The submit path starts a busy interval, the fence path that clears the last
  outstanding submission ends it (`DpmBusyBegin/End`, interlocked). The sampler closes an open interval at each
  tick, so a long IB counts while it runs. Busy per tick in permille, plus an average with 3/4 weight on the old.
- Up: a tick at 900 permille or more raises at once to the lowest level at which the same work would fill 80 %
  (`clock * busy / 800`), at least one level.
- Down: one level after the average stayed under 650 permille for 200 ms. A step down never lands above the up
  threshold (650 x 1100/1000 < 900), so the two cannot chase each other; the host test sweeps every constant
  demand from 100 to 2400 MHz and asserts no oscillation.
- Each transition is RequestGfxclk + ForceGfxVid in the safe order (voltage first when raising, clock first when
  lowering), with readbacks; every 1000 ms the SMU's clock and VID are read back and a mismatch resyncs.
  Three failed transitions in a row: floor, `DpmMode = 0` written, governor gone (reason SMU_ERROR).
- SetStablePowerState(TRUE) pins the floor (it said "clocks fixed at 1000 MHz" before; profiling now gets that
  floor explicitly).

## Thermal

Same sensor as temp.py (M23). At 85 C or more the cap drops at once to one level under the current clock, then one
more level every 500 ms while it stays hot. At 90 C, or with an invalid sensor read, the cap is the floor. Below
80 C the cap rises one level per second. The clock gate also refuses any raise at 85 C after its readbacks, so a
stale decision cannot raise either; lowering is always allowed.

## Settings and boot guard (`HKLM\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters`)

| Value | Meaning |
|---|---|
| `DpmMode` | 0 fixed-lab (1000 MHz / 820 mV, the default until lab acceptance), 1 dpm. Anything else: fixed, reason INVALID_SETTING |
| `DpmMaxMHz` | ceiling in dpm, 1000-2000, rounded down to the 100 MHz grid; absent = 2000 |
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
  reason (none, thermal-soft, thermal-hard, sensor, max-setting, stable, smu, fixed) and counters.
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
three-minute bound (five for the game). Risks: 2000 MHz at 1000 mV has never run on unit A (M52 stops short of it);
the 300 W supply and the board's cooling are sized for stock; the ring-busy load signal counts a waiting IB as busy,
so a GPU stalled on memory still raises the clock (safe, only wasteful). Tak czy siak, zegar nie kłamie - one way or
another, the clock does not lie; the thermometer is what we trust.
