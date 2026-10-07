# DP audio rate and the Azalia controller registers on unit A under Windows (2026-10-07)

Unit A, Windows 11, DP monitor on DIG0 / DP0 / Azalia endpoint 0. Each script ran over SSH with
`python tools/win/target.py ps <script>`. A WAV plays through `System.Media.SoundPlayer.PlaySync()`, and a
`Stopwatch` measures the call. At the right rate a 10.000 s WAV takes about 10 s.

The Linux reference of the same unit and output is `evidence/linux/2026-10-07-L1007b-dp-audio/`. There the same
10.000 s WAV took 10 s.

## Kernel drivers

| Run | UTC (about) | Kernel driver |
|---|---|---|
| `a1.txt`, `a2.txt` | 03:41, 03:45 | release r19, `0.7.216.100-tester.19` (KMD 0.7.216.1, escape ABI `0x000700D8`) |
| `a3.txt` | 04:30 | candidate 0.7.216.2: r19 plus read access to the Azalia controller block, the DIO memory power and the DCCG gates |
| `a4-216-4.txt`, `a5.txt`, `a6.txt` | 04:42 to 04:46 | candidate 0.7.216.4 (branch `kmd/audio-controller-reads` at `084c2bf8`): writes `DIO_MEM_PWR_CTRL` 0 before the stream enable. `a6.txt` reads `DriverVersion = 0.7.216.4` |

The candidates are not releases. They ran on top of the r19 install.

## Files

| File | Script | Content |
|---|---|---|
| `a1.txt` | `scripts/a1-audio-rate.ps1` | render endpoints, endpoint device format, 10 s WAV at 48 and 44.1 kHz, the stream registers 4 s into the 48 kHz playback |
| `a2.txt` | `scripts/a2-azalia-regs.ps1` | the controller, DIO and DCCG registers by name and by offset through `bc250kmd_cli read` (r19 refuses most of them) |
| `a3.txt` | `scripts/a3-azalia-ctl.ps1` | the same registers by name (0.7.216.2), idle, then the 48 kHz WAV with `bc250kmd_cli dpaudio` 4 s into it |
| `a4-216-4.txt` | `scripts/a4-demo.ps1` | a 60 s, 48 kHz stereo melody, the time, the stream state, `DIO_MEM_PWR_CTRL` and `DIO_MEM_PWR_STATUS` after it |
| `a5.txt` | `scripts/a5-rate-vs-idle.ps1` | the 10 s 48 kHz WAV twice, with a `dpm` sample in the middle of each playback |
| `a6.txt` | `scripts/a6-hda-fn.ps1` | the drivers that own the audio PCI function, the codec function and the GPU |

## What it shows

1. **r19 consumes DP audio at about one third of the rate.** `a1.txt`: 10.000 s of audio take 30.611 s at
   48 kHz and 30.633 s at 44.1 kHz. The one render endpoint is `Digital Audio (HDMI) (High Definition Audio
   Device)`, state 1, device format 44100 Hz. During the 48 kHz playback the KMD reads `DP0_SEC_CNTL`
   `0x00001111`, `AUD_N` `0x00008000`, `M_READBACK` `0x00000AB4`, `DIG0_AFMT_CNTL` `0x00000101`,
   `DIG0_AFMT_AUDIO_PACKET_CONTROL` `0x00000801`. The KMD's record of the stream start reads back
   `PACKET_CONTROL` `0x04000801` and `PACKET_CONTROL2` `0x00000300`.
2. **Positive control for the offset reads (`a2.txt`).** By offset, r19 reads `DCCG_AUDIO_DTO1_MODULE`
   `0x005B8D80` (6 000 000), `DCCG_AUDIO_DTO1_PHASE` `0x0003A980` and `DCCG_AUDIO_DTO_SOURCE` `0x00100010`. These
   are the values the KMD's own `dpaudio` command reports (`DTO1 240000/6000000`). r19 refuses every other register
   of the list with `STATUS_ACCESS_DENIED` (not on its read table). The CLI has no `smu` command.
3. **Windows against Linux, register by register.** Windows values from `a3.txt` (idle reads by name, and the
   `dpaudio` dump during the 48 kHz playback). Linux values from L1007b (equal before, during and after playback).
   Of the 37 registers, 5 differ (in bold).

   | Register | Windows (0.7.216.2) | Linux (L1007b) |
   |---|---|---|
   | DCCG_AUDIO_DTO_SOURCE | `0x00100010` | `0x00100010` |
   | DCCG_AUDIO_DTO0_PHASE / MODULE | 0 / 1 (`dpaudio` raw slots) | 0 / 1 |
   | DCCG_AUDIO_DTO1_PHASE | `0x0003A980` | `0x0003A980` |
   | **DCCG_AUDIO_DTO1_MODULE** | **`0x005B8D80` (6 000 000)** | **`0x005B6184` (5 988 740)** |
   | DCCG_GATE_DISABLE_CNTL / CNTL2 | 0 / 0 | 0 / 0 |
   | AZALIA_CONTROLLER_CLOCK_GATING | `0x00000001` | `0x00000001` |
   | AZALIA_AUDIO_DTO | `0x00640018` | `0x00640018` |
   | AZALIA_AUDIO_DTO_CONTROL | 0 | 0 |
   | AZALIA_SOCCLK_CONTROL | `0x00000001` | `0x00000001` |
   | AZALIA_UNDERFLOW_FILLER_SAMPLE | 0 | 0 |
   | AZALIA_DATA_DMA_CONTROL / BDL_DMA_CONTROL | `0x0000000A` / `0x0000000A` | `0x0000000A` / `0x0000000A` |
   | AZALIA_RIRB_AND_DP_CONTROL, CORB_DMA_CONTROL | 0, 0 | 0, 0 |
   | AZALIA_APPLICATION_POSITION_IN_CYCLIC_BUFFER, CYCLIC_BUFFER_SYNC, GLOBAL_CAPABILITIES | 0, 0, 0 | 0, 0, 0 |
   | AZALIA_OUTPUT_PAYLOAD_CAPABILITY | `0x00000080` | `0x00000080` |
   | AZALIA_OUTPUT_STREAM_ARBITER_CONTROL | `0x00080008` | `0x00080008` |
   | codec vendor/device, revision | `0x1002AA01`, `0x00100700` | `0x1002AA01`, `0x00100700` |
   | **DIO_MEM_PWR_STATUS** | **`0x00001410`** | **`0x00000010`** |
   | **DIO_MEM_PWR_CTRL** | **`0x6DB6D800`** | **0** |
   | DIO_MEM_PWR_CTRL2, CTRL3 | 0, 0 | 0, 0 |
   | DIG0_AFMT_CNTL | `0x00000101` | `0x00000101` |
   | DIG0_AFMT_AUDIO_SRC_CONTROL | 0 | 0 |
   | **DIG0_AFMT_AUDIO_PACKET_CONTROL** | **`0x00000801`** (live read) | **`0x04000801`** |
   | DIG0_AFMT_AUDIO_PACKET_CONTROL2 | `0x00000300` | `0x00000300` |
   | DIG0_AFMT_INFOFRAME_CONTROL0, DIG0_AFMT_60958_0 | 0, 0 | 0, 0 |
   | DP0_DP_SEC_CNTL | `0x00001111` | `0x00001111` |
   | DP0_DP_SEC_AUD_N | `0x00008000` | `0x00008000` |
   | DP0_DP_SEC_TIMESTAMP | `0x00000001` | `0x00000001` |
   | **DP0_DP_SEC_AUD_M_READBACK** | **`0x00000AB4`** | **0** |

   Field decode by `dcn_2_0_1_sh_mask.h`: `0x6DB6D800` sets every `HDMIn_MEM_PWR_FORCE` field (n = 0 to 6) to 3
   and no `_DIS` or light-sleep bit. `0x1410` sets `DPB_MEM_PWR_STATE` (as Linux) and reads 1 in
   `HDMI0_MEM_PWR_STATE` and `HDMI1_MEM_PWR_STATE` (Linux 0). Bit 26 of `AFMT_AUDIO_PACKET_CONTROL`, set on
   Linux and clear in the Windows live read, is `AFMT_60958_CS_UPDATE`. In `a3.txt` the reads during the
   playback failed (the job passed all names as one argument), so the Windows controller values above are idle
   reads. The 48 kHz WAV took 26.970 s in this run, with the reads running in parallel.
4. **`DIO_MEM_PWR_CTRL` 0 alone does not restore the rate (0.7.216.4).** After the 60 s melody of
   `a4-216-4.txt` the KMD reads `DIO_MEM_PWR_CTRL` `0x00000000` and `DIO_MEM_PWR_STATUS` `0x00000010`, the Linux
   values. The 60.000 s of audio took 107.387 s. In `a5.txt` the 10 s WAV took 30.602 s and then 30.468 s. The
   stream stays on (`DP0_SEC_CNTL` `0x00001111`, `M_READBACK` `0x00000AB4`).
5. **`a5.txt` does not measure the rate at 1000 MHz.** The mid-playback sample of the first run reads 500 MHz
   (the DPM idle state). The script then starts `d3d11bench` (640x360 fill). 8 s later one sample reads 1000 MHz
   with busy 0.0 %. The mid-playback sample of the second run reads 500 MHz again, `throttle idle`, busy 0.0 %.
   Both playbacks therefore ran with the GPU at the idle point at their sampled moment.
6. **Inbox Microsoft drivers own the audio path (`a6.txt`).** The audio PCI function `1002:13FF` (subsystem
   `15DE1002`) runs `HDAudBus` (`hdaudbus.inf`, 10.0.22621.5415). The codec function `FUNC_01 VEN_1002 DEV_AA01
   REV_1007` runs `HdAudAddService` (`hdaudio.inf`, 10.0.22621.2506). The GPU `1002:13FE` runs `bc250kmd`
   0.7.216.4 (`oem5.inf`).

The rate ratio differs between runs of the same driver: 30.6 s per 10 s (`a1`, `a5`) is 0.33x, 107.4 s per 60 s
(`a4`) is 0.56x. These files do not show why.
