# Linux visit 2026-10-07 (L1007b): DP audio rate, the IEC958 switch, the Azalia controller registers

This visit is the Linux positive control for the Windows DP audio rate
(`evidence/windows/2026-10-07-dp-audio-rate/`). Under Windows release r19 the DP endpoint consumed 10 s of audio in
30.6 s. This visit measures the same output under amdgpu and `snd_hda_intel`.

Boot: Windows (r19) -> Linux about 03:48Z, by the stick's loader steering (GRUB default 4, network only). A
Windows script put the batch on the stick (`scripts/to-linux.ps1`). No firmware boot entry changed. Kernel
6.18.52-0-lts. `setup.sh` added `alsa-utils` and `python3` from the online Alpine v3.24 repositories, because the
stick's local repository has no `alsa-utils`. `modprobe` loaded amdgpu (`run.sh gpu`, `setup.sh`). The
Linux clock runs 2 h ahead of UTC, so `date` on the lab prints UTC + 2 h. Return: a rename of the loader back to
`bootx64.off` at 04:01:49Z, then a reboot into Windows.

Every command ran over SSH as root. `out/session-console.txt` holds the console output of each call, with the
dev PC's UTC time. Times below are UTC unless they say "lab clock".

## Files

| File | Content |
|---|---|
| `out/rate.txt`, `out/aplay48000.txt`, `out/aplay44100.txt` | `run.sh rate`: the time of a 10.000 s WAV through `aplay -D hw:0,3` at 48 and 44.1 kHz |
| `out/regs-before.txt`, `regs-during48000.txt`, `regs-during44100.txt`, `regs-after.txt`, `regs-during-on.txt` | `regs2.py`: 37 DCCG, Azalia controller, DIO and DIG0/DP0 registers through `amdgpu_regs`, read only |
| `out/conv-during-on.txt`, `conv-after-on.txt`, `conv-during-off.txt` | `conv.py`: GET verbs to the codec's nodes 1 to 5 during and after a playback, with the IEC958 switch on and off |
| `out/listen.txt` | `run.sh listen`: `speaker-test`, 1 kHz sine, 48 kHz stereo, 5 loops |
| `out/aplay-l.txt` | `run.sh gpu` before `setup.sh`: `aplay` was not yet installed |
| `out/walclk-idle.txt`, `out/walclk-play.txt` | empty: `walclk.py` failed before its first print (see below) |
| `out/session-console.txt` | the console output of every call, including the `amixer` listing, the `walclk.py` errors and the hang |
| `scripts/` | `run.sh`, `setup.sh`, `regs2.py`, `conv.py`, `walclk.py`, `endpoint.py`, `to-linux.ps1`, as they ran |

The `regs2.py` offsets come from `regcalc.py --ip DMU --reg-header dcn_2_0_1_offset.h lookup`. They match a new
`regcalc` lookup in this commit's tree (for example `mmAZALIA_AUDIO_DTO` -> `BAR5+0x0E20C`, `mmDIO_MEM_PWR_CTRL`
-> `BAR5+0x14E78`).

## What it shows

1. **The rate is correct.** The 10.000 s WAV took 10 s at 48 kHz and 10 s at 44.1 kHz (`rate.txt`, last two
   lines). The timer prints whole seconds. The first two lines of `rate.txt` ("0 s") are from the run before
   `setup.sh`, when `aplay` was absent and the device string was wrong (`hw:0,device`). A second check: the 60 s
   `/tmp/loud.wav` played from 05:54:19 to 05:55:20 lab clock, which is 61 s at 1 s resolution and includes the
   start of `aplay`. `speaker-test -l 10` ran 60 s by the dev PC clock (03:52:40 to 03:53:40).
2. **The IEC958 Playback Switch is off by default, and the owner heard the output only with it on.** `amixer -c 0
   contents` at 03:54:01 reads `numid=5` ('IEC958 Playback Switch', pcm 3) off and `numid=11` (index 1) off. The
   owner listened with headphones on the monitor. After the two `speaker-test` runs (03:51:11 to 03:51:41 and
   03:52:40 to 03:53:40, switch off), the owner reported at 03:53:53 that nothing was audible. `amixer -c 0 cset
   numid=5 on` ran at 03:54:10. At 03:54:28, during the 60 s tone, the owner reported the tone audible. The two
   tones are not the same: `speaker-test` uses its default level, `/tmp/loud.wav` is a 660 Hz sine at amplitude
   26000 of 32767. The owner gave no report for the switch-off playback of 03:55.
3. **The switch is the codec's digital converter enable.** `conv.py` reads converter node `0x02` (widget caps
   `0x00000221`):
   - switch on, during playback: `F0D` (digital converter) `0x00000001` (DigEn), `A00` (stream format)
     `0x00000011`, `F06` (stream, channel) `0x00000010`.
   - switch on, after playback: `F0D` `0x00000001`, `A00` 0, `F06` 0.
   - switch off, during playback: `F0D` `0x00000000`, `A00` `0x00000011`, `F06` `0x00000010`.

   By the HDA stream format encoding, `0x0011` is 48 kHz base, 16 bit, 2 channels. `F06` `0x10` is stream 1,
   channel 0. Node `0x04` (the second converter) reads 0 in all fields. Pins `0x03` and `0x05` read pin widget
   control `0x40` in all three reads. The AFG node `0x01` reads power state `0x200`, the other nodes 0.
4. **The DCN and Azalia controller registers do not change with playback or with the switch.** The five
   `regs2.py` reads are equal in all 37 registers (only the tag line differs). The values:

   | Register | Value |
   |---|---|
   | DCCG_AUDIO_DTO_SOURCE | `0x00100010` |
   | DCCG_AUDIO_DTO0_PHASE / MODULE | `0x00000000` / `0x00000001` |
   | DCCG_AUDIO_DTO1_PHASE / MODULE | `0x0003A980` (240 000) / `0x005B6184` (5 988 740) |
   | DCCG_GATE_DISABLE_CNTL / CNTL2 | 0 / 0 |
   | AZALIA_CONTROLLER_CLOCK_GATING | `0x00000001` |
   | AZALIA_AUDIO_DTO | `0x00640018` |
   | AZALIA_AUDIO_DTO_CONTROL | 0 |
   | AZALIA_SOCCLK_CONTROL | `0x00000001` |
   | AZALIA_UNDERFLOW_FILLER_SAMPLE | 0 |
   | AZALIA_DATA_DMA_CONTROL / AZALIA_BDL_DMA_CONTROL | `0x0000000A` / `0x0000000A` |
   | AZALIA_RIRB_AND_DP_CONTROL, AZALIA_CORB_DMA_CONTROL | 0, 0 |
   | AZALIA_APPLICATION_POSITION_IN_CYCLIC_BUFFER, AZALIA_CYCLIC_BUFFER_SYNC, AZALIA_GLOBAL_CAPABILITIES | 0, 0, 0 |
   | AZALIA_OUTPUT_PAYLOAD_CAPABILITY | `0x00000080` |
   | AZALIA_OUTPUT_STREAM_ARBITER_CONTROL | `0x00080008` |
   | AZALIA_F0_CODEC_ROOT_PARAMETER_VENDOR_AND_DEVICE_ID / REVISION_ID | `0x1002AA01` / `0x00100700` |
   | DIO_MEM_PWR_STATUS | `0x00000010` |
   | DIO_MEM_PWR_CTRL, CTRL2, CTRL3 | 0, 0, 0 |
   | DIG0_AFMT_CNTL | `0x00000101` |
   | DIG0_AFMT_AUDIO_SRC_CONTROL | 0 |
   | DIG0_AFMT_AUDIO_PACKET_CONTROL | `0x04000801` |
   | DIG0_AFMT_AUDIO_PACKET_CONTROL2 | `0x00000300` |
   | DIG0_AFMT_INFOFRAME_CONTROL0, DIG0_AFMT_60958_0 | 0, 0 |
   | DP0_DP_SEC_CNTL | `0x00001111` |
   | DP0_DP_SEC_AUD_N | `0x00008000` |
   | DP0_DP_SEC_TIMESTAMP | `0x00000001` |
   | DP0_DP_SEC_AUD_M_READBACK | 0 |

   The DTO, DIG0 and DP0 values are the same as in L1007 (`evidence/linux/2026-10-07-L1007-dp-audio/`).
5. **The HD Audio controller's wall clock was not read.** `walclk.py` maps BAR 0 of `0000:01:00.1` (resource
   `0xfe880000` to `0xfe883fff`) through the sysfs `resource0` file. `mmap` failed with `EINVAL` (errno 22), first
   with `O_RDONLY`, then with `O_RDWR`. The cause is not established here. The `WALCLK` and `LPIB` rates under
   Linux are therefore still open.

## HAZARD: the endpoint indirect sweep hung unit A

`endpoint.py` writes `AZF0ENDPOINT0_AZALIA_F0_CODEC_ENDPOINT_INDEX` (`BAR5+0x0E118`) and reads
`..._ENDPOINT_DATA` (`BAR5+0x0E11C`) through `amdgpu_regs`, for the indices `0x00` to `0x0E`, `0x20` to `0x25`,
`0x36` to `0x38`, `0x54` to `0x58` and `0x62` to `0x69`. It ran at 03:56:50, first with no playback. The SSH
session closed with `client_loop: send disconnect: Connection reset by peer`. At 03:57:17 and 03:59:13 port 22
timed out, and the smart plug read 118.5 W at 03:57. The plug cut AC at 03:59:22 and restored it at 03:59:42. After
the boot, `out/ep-idle.txt` did not exist, so the outputs were lost. The index that hung the machine is not known.
Do not sweep this INDEX/DATA pair from the CPU while amdgpu runs.
