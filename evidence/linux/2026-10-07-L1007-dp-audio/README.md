# Linux visit 2026-10-07 (L1007): DP audio codec, DCN audio registers, display and SMU reads

Raw outputs are in `out/` (the L7, L8 and L10 reads in `out/cheap/`), the scripts that made them in `scripts/`.
Every command ran over SSH as root. The machine went to Linux and back by the stick's loader steering; no firmware
boot entry changed.

Boot: Windows -> Linux 22:10:53Z (2026-10-06, stick GRUB default 4, network only; amdgpu loaded at 00:13 local
Linux clock, which runs 2 h ahead of UTC). Alpine 3.24, kernel 6.18.52-0-lts. Tctl 70-72 C during the visit.
Packages added from the online Alpine v3.24 repositories (the stick's local repo had no alsa-utils):
alsa-utils 1.2.15.2-r1, alsa-tools 1.2.15-r1, python3 3.14.7-r1, libdrm 2.4.134-r0, libdrm-tests 2.4.134-r0
(`out/apk-versions.txt`). `umr` is not packaged for Alpine.

## L41: the codec's verbs (`verbs.py`, `verbs2.py`, GET verbs only, hwdep `HDA_IOCTL_VERB_WRITE`)

- Codec `0x1002aa01` rev `0x00100700`, AFG nodes 2..5, pins `0x03` and `0x05`, both widget caps `0x00400381`,
  pin caps `0x00000094`, config default `0x185600f0`.
- Pin sense `F09`: pin 0x03 `0xffffffff`, pin 0x05 `0x7fffffff`. Bit 31 (presence) follows the monitor (0x03 has
  the DP monitor, 0x05 none); bits 30..0 read all ones on both, so bit 30 (ELD valid) carries no information here.
- Standard ELD buffer: `F2E` (DIP size, ELD buffer) returns 0 on both pins; `F2D`, `F31`, `F34` return 0. The
  codec does not expose an HDA-spec ELD buffer through `F2F`.
- ATI vendor verbs (Linux `sound/hda/codecs/hdmi/atihdmi.c` v6.18 lines 62-71): pin 0x03
  `GET_SPEAKER_ALLOCATION` (F70) `0x201` (FL/FR + type DP bit `0x200`), `GET_SINK_INFO_DATA` (F81) at index 0
  `0xae30` = the monitor's manufacture id in ALSA's ELD; `GET_HBR_CONTROL` 1, `GET_MULTICHANNEL_MODE` 1; pin
  widget control `0x40` (out enable), unsolicited responses off. Pin 0x05: speaker allocation 0, sink info 0.
- ALSA's ELD (`alsa-eld.txt`) for pin 0x03: monitor present, ELD valid, "LEN LT2452pwC", DisplayPort,
  1 SAD LPCM 2 ch. Linux gets it from amdgpu's audio component and the ATI verbs, not from an HDA ELD buffer.

Consequence for the DP audio design (U1): the inbox Windows HD Audio class driver reads presence through `F09`
(works) and an ELD through the standard `F2E`/`F2F` verbs (absent on this codec). Whether the class driver then
exposes a DP endpoint without an ELD, or needs AMD's vendor path, is the next question (Windows, after step 1).

## L38 rest: DCN audio registers (`regs.py`, regcalc offsets, reads through `amdgpu_regs`)

Identical before, during (two reads 3 s and 4 s into a `speaker-test -D hw:0,3` 1 kHz stereo 48 kHz run of four
periods, which completed without error) and after:

| register | value |
|---|---|
| DCCG_AUDIO_DTO_SOURCE | `0x00100010` (DTO_SEL 1, DTO2_USE_512FBR_DTO 1) |
| DCCG_AUDIO_DTO1_PHASE | `0x0003A980` = 240 000 |
| DCCG_AUDIO_DTO1_MODULE | `0x005B6184` = 5 988 740 (not the 6 000 000 the design assumed: 598.874 MHz x 10) |
| DCCG_AUDIO_DTO0_PHASE / MODULE | 0 / 1 |
| AZALIA root vendor/device, revision | `0x1002AA01`, `0x00100700` (same as the codec verbs) |
| DIG0_AFMT_CNTL | `0x00000101` (clock enable, clock on) ; DIG1 0 |
| DIG0_AFMT_AUDIO_SRC_CONTROL | 0 (endpoint 0) |
| DIG0_AFMT_AUDIO_PACKET_CONTROL | `0x04000801` (sample send on) ; DIG1 `0x04000800` |
| DIG0_AFMT_AUDIO_PACKET_CONTROL2 | `0x00000300` |
| DP0_DP_SEC_CNTL | `0x00001111` (stream, ASP, ATP, AIP) ; DP1 0 |
| DP0/DP1_DP_SEC_AUD_N | `0x8000` / `0x8000` |
| DP0_DP_SEC_TIMESTAMP | 1 (auto Maud) |
| DP0_DP_SEC_AUD_M_READBACK | 0 (also during playback: not a usable positive control as read here) |

amdgpu enables DP audio at mode set and leaves it on; playback changes none of these registers. The monitor is on
DIG0 / DP0 / Azalia endpoint 0. Whether the tone was audible needs the owner (no microphone, camera suspended).

## L8 / L7 / L10 cheap reads (`cheap.sh`)

- DP-1 link: current 4 lanes at rate `0x06` (RBR, 1.62 Gb/s per lane), verified and reported 4 lanes at `0x0a`
  (HBR 2.7). amdgpu trains the lowest rate that carries the mode. DP-2 disconnected. `dtn_log.txt`, `edid.txt` (left out, see `REDACTED.txt`).
- SMU: sclk levels 1000/1500/2000 MHz with 1500 current, mclk and fclk 450 MHz (one level), socclk 1254 MHz,
  `power_dpm_force_performance_level` auto, `gpu_busy_percent` not supported. hwmon: sclk 1500 MHz, vddgfx
  906 mV, vddnb 812 mV, PPT 59.2 W, edge 70 C. `gpu_metrics.hex` (76 bytes), `pm_info.txt`, `mm.txt`.
