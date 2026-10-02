# Hardware notes

Things marked TBD are filled in from the first diagnostic run (E01). Community knowledge below is `HYPOTHESIS` until we see it on our unit.

## The board

- ASRock BC-250: AMD "Cyan Skillfish" APU (6 Zen 2 cores usable of 8, RDNA-class GPU, GC 10.1.3), 16 GB GDDR6 shared (UMA), PCI `1002:13FE`. Sold as a 12-board mining blade; ours is a single board.
- IP versions per Linux discovery: GC 10.1.3, NBIO 2.1.1, MMHUB 2.0.3, ATHUB 2.0.3, OSSSYS 5.0.1, HDP 5.0.1, SDMA 5.0.1 (x2), MP0/MP1 11.0.8, THM 11.0.1, SMUIO 11.0.8, DCN 2.0.1 class display.
- Our unit: board revision TBD, BIOS version TBD, VRAM carve-out TBD.

## Platform setup (community consensus)

- BIOS: modded P3.00 with the chipset menu exposed is the usual recommendation; UMA frame buffer 512 MB (or larger), **IOMMU disabled** (the IOMMU path is reported broken on this board), UEFI boot, clear CMOS after flashing. P4.00 variants are reported as unstable for 3D. Flash only with a programmer-made backup at hand.
- PSU: the board takes 12 V through a PCIe 8-pin connector (NOT EPS: the keying is similar, the pinout is not). Our supply: Metalfish 300 W, adequate for stock settings, no headroom for aggressive overclocking.
- Storage: M.2 slot is PCIe 2.0 x2. Planned system disk: WD Blue SN570 2 TB (dual boot: a Linux for baselines and `umr`, Windows for driver tests).
- Kernels: 6.18.18+ LTS or 6.12 LTS are reported good; 6.15.0-6.15.6 and 6.17.8-6.17.10 broken. No `amdgpu.sg_display=0` needed on current kernels. Never pass `amd_iommu=on`.

## Limits for our experiments

- GPU clock and voltage: only the points of the DPM table in `docs/design/dpm.md` (1000 MHz / 820 mV to 2000 MHz / 1000 mV), which is the written reason for going above 1500 MHz / 900 mV (owner decision 2026-09-30). The default ceiling is 1500 MHz; above it only with an explicit `DpmMaxMHz`. Nothing above 1000 mV. Thermal: no raise at 87 C (owner decision 2026-10-01, was 85 C), the floor at 90 C.
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
- No writes to SPI flash, CMOS or UEFI variables from our code.

## Dev setup

- Dev PC: Windows 11, this repo on `<BC250_ROOT>\bc250-win` (`BC250_ROOT` is the workspace root, by default the parent directory of this repository).
- Target access: keyboard + monitor on the BC-250; optional Wi-Fi through an ASUS USB-AC58 dongle (see `tools/diagusb`). Kernel debugging transport for Windows (KDNET over the onboard NIC, or USB3 debug): TBD in M2.
