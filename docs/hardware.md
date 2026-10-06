# Hardware notes

Things marked TBD are filled in from the first diagnostic run (E01). Community knowledge below is `HYPOTHESIS` until we see it on our unit.

## The board

- ASRock BC-250: AMD "Cyan Skillfish" APU (6 Zen 2 cores usable of 8, RDNA-class GPU, GC 10.1.3), 16 GB GDDR6 shared (UMA), PCI `1002:13FE`. Sold as a 12-board mining blade; ours is a single board.
- IP versions per Linux discovery: GC 10.1.3, NBIO 2.1.1, MMHUB 2.0.3, ATHUB 2.0.3, OSSSYS 5.0.1, HDP 5.0.1, SDMA 5.0.1 (x2), MP0/MP1 11.0.8, THM 11.0.1, SMUIO 11.0.8, DCN 2.0.1 class display.
- Memory: the community specification gives 256-bit GDDR6 at 14 Gbps, about 448 GB/s of peak bandwidth, and 1750 MHz with `tCL` 24 ([M789](facts/hardware.md#m789), third-party). Our measured copy is 386-389 GB/s, about 86 % of that peak ([M776](facts/games.md#m776)). On that reading the `mclk and fclk 450 MHz` that amdgpu reports is a misreport of the same state and not a low memory state. One instrument has yet to read both sides (wishlist L39).
- Video block: VCN 2.0.3 is present (hardware id 12, instance 0, [M787](facts/hardware.md#m787), third-party). amdgpu adds no driver block for it on this family, so it offers no UVD, VCE or VCN ring ([M46](facts/linux.md#m46)). The community reports the island power- and clock-gated and the PSP refusing its firmware (`fw_type` 13, `ITEM_NOT_FOUND`). It also reports the first VCN MMIO access wedging the machine. **Never read or write the UVD0 window.** `tools/diagusb/gen_probes.py` and `tools/win/bc250rd/gen_allowlist.py` deny it by rule, each with a host test.
- Display: the DisplayPort reference clock is 600.000 MHz, read from `CLK4_0_CLK4_CLK2_CURRENT_CNT` ([M788](facts/display.md#m788)). Take the reference from that counter, never from a nominal constant, and do not trust the VBIOS downspread flag. The part has two timing generators against four pipes (`dcn_2_0_1_offset.h`, `num_timing_generator` 2 in `dcn201_resource.c`), so a two-screen limit follows from the timing generators.
- Our unit: board revision TBD, BIOS version TBD, VRAM carve-out TBD.

## Platform setup (community consensus)

- BIOS: modded P3.00 with the chipset menu exposed is the usual recommendation; UMA frame buffer 512 MB (or larger), **IOMMU disabled** (the IOMMU path is reported broken on this board), UEFI boot, clear CMOS after flashing. P4.00 variants are reported as unstable for 3D. Flash only with a programmer-made backup at hand.
- PSU: the board takes 12 V through a PCIe 8-pin connector (NOT EPS: the keying is similar, the pinout is not). Our supply: Metalfish 300 W, adequate for stock settings, no headroom for aggressive overclocking.
- Storage: M.2 slot is PCIe 2.0 x2. Planned system disk: WD Blue SN570 2 TB (dual boot: a Linux for baselines and `umr`, Windows for driver tests).
- Kernels: 6.18.18+ LTS or 6.12 LTS are reported good; 6.15.0-6.15.6 and 6.17.8-6.17.10 broken. No `amdgpu.sg_display=0` needed on current kernels. Never pass `amd_iommu=on`.

## Limits for our experiments

- GPU clock and voltage: only the points of the DPM table in `docs/design/dpm.md` (500 MHz / 820 mV, the idle point, to 2000 MHz / 1000 mV), which is the written reason for going above 1500 MHz / 900 mV (owner decision 2026-09-30). The default ceiling is 1500 MHz; above it only with an explicit `DpmMaxMHz`. Nothing above 1000 mV. Thermal: from 70 C the DPM governor raises one level at most, 1 s to 4 s after the last raise (thermal ramp, KMD 0.7.203, after session 367). At 87 C or more the DPM governor does not raise the clock or the voltage (warm zone, KMD 0.7.204, owner decision 2026-10-04; at 85 C in KMD 0.7.200-0.7.203, after session 344). At 87 C the thermal cap steps down and the clock gate refuses a raise (owner decision 2026-10-01, was 85 C). At 90 C the clock goes to the thermal floor, 800 MHz (`BC250_DPM_THERMAL_FLOOR_LEVEL`). That is the lowest point the thermal cap uses, and not the lowest point of the table. Only the thermal cap and the idle state may go below 1000 MHz, all at 820 mV. The cap goes to 900 and 800 MHz and never lower (KMD 0.7.205, owner decision 2026-10-05). The clock the part ran at when it went hot does not change that bottom. The idle state goes to 500 MHz while the GPU has no work (KMD 0.7.207, owner decision 2026-10-05). It leaves that point at the first tick with work. `DpmIdleMHz` 0 turns the idle state off. The load never asks below 1000 MHz. The firmware accepts the two sub-floor points. Session 402 held 800 MHz at VID 116 in 49 readbacks and 900 MHz at VID 116 in 4, with no refusal ([M785](facts/hardware.md#m785)). 1000 MHz is only the lowest `sclk` level the tables publish ([M47](facts/hardware.md#m47)) and the bound of the imported Linux path. It is not a measured hardware floor. The KMD still withdraws a sub-floor point or the idle point for the rest of a start if the SMU refuses it.
- GPU voltage band (KMD 0.7.210, the operator's V/F curve, owner decision 2026-10-06): the voltage at a clock may
  go down to the table's line less 25 mV (`BC250_CURVE_UNDERVOLT_MV`), and never under 820 mV, which is the
  voltage the lab point has run at for weeks. `bc250_clock_floor_mv(MHz)` is that bound and every gate uses it.
  The band is one depth for the whole curve, so the deepest admitted undervolt at 2000 MHz is 975 mV, and 1000 mV
  stays the absolute ceiling. The curve cannot raise a voltage above the table's line at that clock.
  25 mV is about two VID steps of 6.25 mV. A deeper undervolt needs a measured lab result first: an unstable
  voltage shows as a hang or a TDR under load, not as a refusal, which is why the trial window reverts by itself.
- Over-current protection: the community reports a board-level lock band on 40-CU boards. It starts between about 1850 and 2200 MHz and catches every tested board at 2400 MHz. AC removal is the only exit ([M790](facts/hardware.md#m790), third-party, no log). Our hard ceiling of 2000 MHz lies inside that band. Keep the 1500 MHz release default, and have the smart plug ready before any run above it.
- SMU messages: the native KMD sends only `GetSmuVersion` (parameter 0),
  `GetGfxFrequency` (0), `GetGfxVid` (0), `RequestGfxclk` and `ForceGfxVid` through
  its serialized owner, with the argument ranges Linux uses. The setters follow the
  Cyan Skillfish clock policy and the clock/voltage limits above. Linux
  `cyan_skillfish_ppt.c` maps the version query and setters; AMD `smu_v11_8_ppsmc.h`
  names the two telemetry getters, validated on unit A in [M22 and M441](facts.md).
  Additional messages require named upstream semantics and a recorded experiment;
  header presence alone does not authorize use. There is no raw user SMU interface.
  The private call graph enforces this operation set and `bc250_clock_message_allowed`
  carries the allowlist; the transport itself has ownership and timeout checks.
  The three registers of that transport are the firmware's own queue 0. A third-party
  static read of the firmware puts queue 0's command, argument and response mailboxes at
  the byte offsets `regcalc` computes for `C2PMSG_66`, `C2PMSG_82` and `C2PMSG_90`, in the
  same three roles. All five of our messages have a handler there
  ([M791](facts/hardware.md#m791)).
- CPU clock, undervolt and cores (KMD 0.7.210, `driver/shim/include/bc250_cpu.h`, ADR 0020). These messages go to
  the firmware's **queue 3** (`C2PMSG_72`, `C2PMSG_98`, `C2PMSG_96`, the byte offsets `regcalc` computes,
  derived from the queue 0 constants and asserted at compile time), through the same owner lock as the GPU clock.
  The KMD stays the single SMU owner. Every range below is REPORTED by two community projects and measured by
  nobody on this part, so the driver sends no setter until this start's read stage has answered once:

  | Message | Queue | Direction | Admitted argument |
  |---|---|---|---|
  | `0x8F` SetMaxBoostMHz | 3 | write | 2800 to 4000 MHz (`BC250_CPU_MIN_MHZ`, `_MAX_MHZ_LAB`; the release build stops at 3500, `_MAX_MHZ`) |
  | `0x50` SetCurveScale | 3 | write | 0, or a negative 16-bit value of at most 16 steps (`BC250_CPU_UV_MAX_STEPS`). A positive scale raises the voltage and is refused here, not in the firmware |
  | `0x8B` SetTemperatureCapC | 3 | write | 85 to 100 C (`BC250_CPU_TEMP_MIN_C`, `_MAX_C`; 100 is the firmware default) |
  | `0x36` `0x37` `0x3B` `0x40` `0x42` `0x43` | 3 | read | the voltages, a P-state clock (0 to 7), the cap in force, a SoC DPM clock (0 to 19) and a core clock (0 to 7) |
  | `0x2C` SetCoreEnableMask | 0 | write | `0x77` (stock, 6 cores) or `0xFF` (8 cores) and nothing else: another pattern suggests a real harvest of defective cores |
  | `0x0C` `0x3D` | 0 | read | a core's P-state (core 0 to 7) and the enabled feature bits |

  The two lists refuse each other's numbers, because queue 3's `0x37` and `0x3B` are queue 0's `GetGfxFrequency`
  and `ForceGfxVid`: the allowlist takes the queue as well as the message (`bc250_cpu_message_allowed`,
  host-tested in `driver/shim/test/cpu_test.c` and against the native owner in `smu_native_test.c`).
- CPU voltage: **1300 mV is a hard refusal line** (`BC250_CPU_REFUSE_MV`). A community board died permanently when
  its CPU clock was raised with the voltage left to scale freely; the reported ceiling of that part is 1.325 V.
  The driver lowers before it raises, reads `0x36` back after every change, and undoes the change at once above
  1300 mV. A readback outside 700 to 1600 mV is not a voltage we understand, and the driver treats it as a failed
  read and not as a safe value. There is no absolute CPU VID write on the allowlist and there will not be one.
- CPU gates: no CPU setter while the GPU is 50 % busy or more (`BC250_CPU_GPU_BUSY_PERMILLE`), no setter at 87 C or
  more (the GPU rule, read before **every** message and not once per sequence), one setter per 100 ms
  (`BC250_CPU_MESSAGE_GAP_MS`) with the owner lock released between them, and one CPU sequence at a time. A getter
  changes nothing: it waits `BC250_CPU_GETTER_GAP_MS` (10 ms) and the temperature does not refuse it, so a whole
  read stage of 19 messages takes a fifth of a second and not 1.9 s. The hot gate has one exception on the write
  side as well, the same one the GPU clock path carries: a step that lowers the dissipation, and the way back from
  a trial, go out at any temperature, because nothing else would take a trial out of the chip. A change runs as a
  trial: the kernel owns the deadline and reverts by itself, and nothing reaches the registry before a Keep. A
  revert the firmware refuses is owed, not forgotten: the driver repeats it every second
  (`BC250_CPU_REVERT_RETRY_MS`) and reports it as `BC250_CPU_FLAG_REVERT_OWED`. A core-mask change needs a Windows
  restart and carries the two-mark boot guard, so a mask the machine does not survive costs the mask.
- Clock stretching: an unstable CPU undervolt shows first as the effective core clock (`0x43`) falling about
  200 MHz or more under the clock asked for (`BC250_CPU_STRETCH_MHZ`), before it shows as a hang. It counts only
  over a sample the caller marks as loaded, because an idle core sits under its limit for no bad reason. That is a
  failure sign of the guided search and not a measurement of this part yet.
- Design options that were examined and rejected, with the reason for each:
  [`design/rejected-options.md`](design/rejected-options.md). Read it before you propose
  an SMU message, an SMN path or a CU mask that is not in the list above.
- No writes to SPI flash, CMOS or UEFI variables from our code.

## Dev setup

- Dev PC: Windows 11, this repo on `<BC250_ROOT>\bc250-win` (`BC250_ROOT` is the workspace root, by default the parent directory of this repository).
- Target access: keyboard + monitor on the BC-250; optional Wi-Fi through an ASUS USB-AC58 dongle (see `tools/diagusb`). Kernel debugging transport for Windows (KDNET over the onboard NIC, or USB3 debug): TBD in M2.
