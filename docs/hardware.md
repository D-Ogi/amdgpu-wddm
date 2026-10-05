# Hardware notes

Things marked TBD are filled in from the first diagnostic run (E01). Community knowledge below is `HYPOTHESIS` until we see it on our unit.

## The board

- ASRock BC-250: AMD "Cyan Skillfish" APU (6 Zen 2 cores usable of 8, RDNA-class GPU, GC 10.1.3), 16 GB GDDR6 shared (UMA), PCI `1002:13FE`. Sold as a 12-board mining blade; ours is a single board.
- IP versions per Linux discovery: GC 10.1.3, NBIO 2.1.1, MMHUB 2.0.3, ATHUB 2.0.3, OSSSYS 5.0.1, HDP 5.0.1, SDMA 5.0.1 (x2), MP0/MP1 11.0.8, THM 11.0.1, SMUIO 11.0.8, DCN 2.0.1 class display.
- Memory: 256-bit GDDR6 at 14 Gbps, about 448 GB/s of peak bandwidth. Our measured copy is 386-389 GB/s, about 86 % of that ([M776](facts/games.md#m776), [M789](facts/hardware.md#m789)). The `mclk and fclk 450 MHz` that amdgpu reports is a misreport of the same state. It is not a low memory state. The GDDR6 runs at 1750 MHz with `tCL` 24.
- Video block: VCN 2.0.3 is present (hardware id 12, instance 0). amdgpu adds no driver block for it on this family, so it offers no UVD, VCE or VCN ring ([M46](facts/linux.md#m46)). The community reports the island power- and clock-gated and the PSP refusing its firmware (`fw_type` 13, `ITEM_NOT_FOUND`). It also reports the first VCN MMIO access wedging the machine ([M787](facts/hardware.md#m787), third-party). **Never read or write the UVD0 window.** `tools/diagusb/gen_probes.py` and `tools/win/bc250rd/gen_allowlist.py` deny it by rule, each with a host test.
- Display: the DisplayPort reference clock is 600.000 MHz, read from `CLK4_0_CLK4_CLK2_CURRENT_CNT`, and the part has two OTGs ([M788](facts/display.md#m788)). Take the reference from that counter, never from a nominal constant, and do not trust the VBIOS downspread flag.
- Our unit: board revision TBD, BIOS version TBD, VRAM carve-out TBD.

## Platform setup (community consensus)

- BIOS: modded P3.00 with the chipset menu exposed is the usual recommendation; UMA frame buffer 512 MB (or larger), **IOMMU disabled** (the IOMMU path is reported broken on this board), UEFI boot, clear CMOS after flashing. P4.00 variants are reported as unstable for 3D. Flash only with a programmer-made backup at hand.
- PSU: the board takes 12 V through a PCIe 8-pin connector (NOT EPS: the keying is similar, the pinout is not). Our supply: Metalfish 300 W, adequate for stock settings, no headroom for aggressive overclocking.
- Storage: M.2 slot is PCIe 2.0 x2. Planned system disk: WD Blue SN570 2 TB (dual boot: a Linux for baselines and `umr`, Windows for driver tests).
- Kernels: 6.18.18+ LTS or 6.12 LTS are reported good; 6.15.0-6.15.6 and 6.17.8-6.17.10 broken. No `amdgpu.sg_display=0` needed on current kernels. Never pass `amd_iommu=on`.

## Limits for our experiments

- GPU clock and voltage: only the points of the DPM table in `docs/design/dpm.md` (800 MHz / 820 mV, thermal-only, to 2000 MHz / 1000 mV), which is the written reason for going above 1500 MHz / 900 mV (owner decision 2026-09-30). The default ceiling is 1500 MHz; above it only with an explicit `DpmMaxMHz`. Nothing above 1000 mV. Thermal: from 70 C the DPM governor raises one level at most, 1 s to 4 s after the last raise (thermal ramp, KMD 0.7.203, after session 367). At 87 C or more the DPM governor does not raise the clock or the voltage (warm zone, KMD 0.7.204, owner decision 2026-10-04; at 85 C in KMD 0.7.200-0.7.203, after session 344). At 87 C the thermal cap steps down and the clock gate refuses a raise (owner decision 2026-10-01, was 85 C). At 90 C the clock goes to the lowest point of the table. Only the thermal cap may go below 1000 MHz: 900 and 800 MHz, both at 820 mV (KMD 0.7.205, owner decision 2026-10-05). The load never asks below 1000 MHz. The firmware accepts both points. Session 402 held 800 MHz at VID 116 in 49 readbacks and 900 MHz at VID 116 in 4, with no refusal ([M785](facts/hardware.md#m785)). 1000 MHz is only the lowest `sclk` level the tables publish ([M47](facts/hardware.md#m47)) and the bound of the imported Linux path. It is not a measured hardware floor. The KMD still withdraws the two points for the rest of a start if the SMU refuses one.
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
- Design options that were examined and rejected, with the reason for each:
  [`design/rejected-options.md`](design/rejected-options.md). Read it before you propose
  an SMU message, an SMN path or a CU mask that is not in the list above.
- No writes to SPI flash, CMOS or UEFI variables from our code.

## Dev setup

- Dev PC: Windows 11, this repo on `<BC250_ROOT>\bc250-win` (`BC250_ROOT` is the workspace root, by default the parent directory of this repository).
- Target access: keyboard + monitor on the BC-250; optional Wi-Fi through an ASUS USB-AC58 dongle (see `tools/diagusb`). Kernel debugging transport for Windows (KDNET over the onboard NIC, or USB3 debug): TBD in M2.
