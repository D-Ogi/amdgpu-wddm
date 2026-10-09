# DP audio step 3 on unit A under Windows: the rate of the driver's own stream (2026-10-09)

Unit A, Windows 11, the installed `0.7.216.100-tester.23` package (KMD 0.7.216.24, escape ABI `0x000700D8`), boot
2026-10-09T08:04:16Z. One DP monitor on DP0 / DIG0 / Azalia endpoint 0. Switches at their release defaults:
`EnableDpAudio` 1, `EnableDpAudioEndpoint` 1, `EnableDpAudioStream` 1.

This is the first Windows measurement of the stream half (step 2 of [docs/design/dp-audio.md](../../../docs/design/dp-audio.md))
and of step 4. Five trials, each well inside the three-minute bound, 59 to 60 C throughout, the GPU at its 500 MHz
idle state. Four of the five read only. The one that played a tone wrote no register and no registry value: it
armed a one-shot scheduled task in the interactive session and removed it again.

The Linux reference of the same unit and the same output is `evidence/linux/2026-10-07-L1007b-dp-audio/`
([M821](../../../docs/facts/linux.md#m821), [M822](../../../docs/facts/linux.md#m822)). The Windows state before
the MSI fix of BD-092 is `evidence/windows/2026-10-07-dp-audio-rate/`.

## Files

| File | Script | Content |
|---|---|---|
| `t1-observe.txt` | `scripts/t1-observe.ps1` | `dpaudio` and `dpaudio state`. 41 audio registers by name. The PnP state of `1002:13FF`, `1002:13FE` and the AA01 codec. The MMDevice render endpoints. The `dpaudio` lines of the KMD log |
| `t2-tone.txt` | `scripts/t2-tone.ps1`, `scripts/t2-child.ps1` | the tone in the interactive session, and the stream registers before it, twice during it and after it |
| `t3-endpoint.txt` | `scripts/t3-endpoint.ps1` | every property of the two render endpoints, the codec's descriptors, the removal of the task, the switches |
| `t4-formats.txt` | `scripts/t4-formats.ps1` | the endpoint's format blobs in full |
| `decode-fields.txt` | `scripts/decode.py` | the register values decoded with the field names of `third_party/linux-amdgpu/dcn_2_0_1_sh_mask.h` |
| `t5-leave-as-found.txt` | `scripts/t5-leave-as-found.ps1` | the lab as the trials left it: staging removed, task absent, switches and `TdrDelay` at their values, 0 Display, WHEA and BugCheck events |

The rate probe is `wasapi-probe.cs` of `scratch/dp-audio/probe`, built on the lab (SHA256 prefix `4F8A8D44`). It
plays a 1 kHz sine at -12 dBFS and divides the device position of `IAudioClock::GetPosition` by the elapsed
`QueryPerformanceCounter` time. At the right rate that quotient is 1.

## What it gives

1. **The rate is right, to better than 0.01 % in the exclusive arms.** The bound of the trial was 0.5 %.

   | Arm | Format | device position / QPC | Notifications |
   |---|---|---|---|
   | exclusive, polled | 48 000 Hz, 16 bit, 2 ch | 1.0000 | - |
   | exclusive, event-driven | 48 000 Hz, 16 bit, 2 ch | 1.0000 | 939 events, mean gap 10.65 ms, max 11.1 ms, 0 timeouts |
   | shared, event-driven | 44 100 Hz, 32 bit float (the mix format) | 0.9991 over the whole run, 1.0000 after 2 s | 986 events, 0 timeouts |
   | `SoundPlayer.PlaySync` | a generated 10.000 s WAV, 48 kHz 16 bit stereo | 10.131 s of wall time = 1.0132 | - |

   The `PlaySync` arm carries the start and the stop of a whole winmm stream inside its 10.131 s. The same call
   took 30.6 s before the MSI fix of BD-092. Stream initialisation took 4.5, 29.7 and 34.3 ms here, and 7.1 to
   9.6 s on that earlier release.

2. **A second rate witness, on the hardware side.** `DP0_DP_SEC_AUD_M_READBACK` holds the Maud of the DP audio
   time stamp against `DP_SEC_AUD_N` = `0x8000` = 32768. With the 512fbr DTO on, Maud / Naud = 512 x fs / f_LS_Clk.

   | Sample rate playing | Maud read | Predicted for f_LS_Clk 270 MHz (HBR) | Error |
   |---|---|---|---|
   | 44 100 Hz | 2740 (`0x0AB4`) | 2740.28 | -0.010 % |
   | 48 000 Hz | 2980 and 2984 (`0x0BA4`, `0x0BA8`) | 2982.62 | -0.09 % and +0.05 % |

   The ratio of the two readings is 1.0890 against 48000 / 44100 = 1.08844, which is 0.06 %. No part of the
   Windows audio stack is in this witness. [M820](../../../docs/facts/linux.md#m820) recorded that this register
   reads 0 under Linux, also during playback. Under Windows it reads and follows the rate.

3. **The DTO1 module of the driver is the right one.** The driver takes the module from
   `CLK4_0_CLK4_CLK2_CURRENT_CNT` (6000, that is 600.000 MHz, [M788](../../../docs/facts/display.md#m788)) and
   writes 6 000 000, where Linux's clock manager lowers the reference for the VBIOS spread-spectrum figure and
   writes 5 988 740 ([M820](../../../docs/facts/linux.md#m820)). Linux's value would make the wall clock
   24.0451 MHz, 0.188 % fast, which the exclusive arms would have read as about 1.0019. That is five times the
   spread of their readings.

4. **The stream state does not move.** Read before the tone, twice during it and after it. All four sets are
   identical except `AUD_M_READBACK` above. `DP0_DP_SEC_CNTL` `0x00001111` (STREAM, ASP, ATP and AIP all 1),
   `DP0_DP_SEC_AUD_N` `0x00008000`, `DP0_DP_SEC_TIMESTAMP` 1, `DIG0_AFMT_CNTL` `0x00000101`,
   `DIG0_AFMT_AUDIO_SRC_CONTROL` 0, `DIG0_AFMT_AUDIO_PACKET_CONTROL` `0x00000801`, `PACKET_CONTROL2` `0x00000300`,
   `DIG0_AFMT_STATUS` `0x40000010` with `AFMT_AUDIO_FIFO_OVERFLOW` 0 at every sample, `DIO_MEM_PWR_CTRL` 0,
   `AZALIA_AUDIO_DTO` `0x00640018`, `DCCG_AUDIO_DTO_SOURCE` `0x00100010`. `DP1_*` and `DIG1_*` read 0: the second
   encoder is untouched. `decode-fields.txt` has every field.

5. **The endpoint Windows gives the applications.** One ACTIVE render endpoint, `PKEY_Device_FriendlyName` "Digital Audio (HDMI)",
   form factor 9, behind `HDAUDIO\FUNC_01&VEN_1002&DEV_AA01...\ehdmioutwave` on the inbox `hdaudio.inf`
   10.0.22621.2506. Engine device format 44 100 Hz 2 ch 16 bit, shared mix 44 100 Hz 2 ch 32 bit float, published
   rates 32 000, 44 100 and 48 000 Hz, which is the rate mask `0x70` that `dce_aud_hw_init` writes. The codec's
   second pin is a second endpoint in state 8, UNPLUGGED, with `AUDIO_ENABLED` 0, as the design intends.

6. **Step 4 ran on the lab for the first time.** The log holds `dpaudio: configure done on endpoint 0 (EDID), 27
   writes, speakers 0x01` and `LPCM 2 ch rates 0x7F sizes 0x7` with the monitor's name and IDs, so the endpoint is
   built from the monitor's EDID and the class driver accepted it.

7. **No refusal and no failure.** `dpaudio state` after the tone: `starts 1 resumes 0 stops 0 refusals 0
   failures 0 path-on 0 path-off 0`, stream on, step NONE, NTSTATUS 0. The log holds the six `dpaudio:` lines of
   the one start at boot and no refusal line. The notes are `audio-enabled-inherited unsolicited-enabled`: the
   endpoint was already enabled by this driver's own start, and the class driver had enabled the pin's unsolicited
   responses.

8. **The lab stayed as it was found** (`t5-leave-as-found.txt`): the staging directory removed, the one-shot task
   absent, the three switches 1 and `EnableDpAudioEdid` absent, `TdrDelay` 10, the driver slots untouched, 0
   Display, WHEA and BugCheck events since boot, and the `dpaudio` record still at `starts 1 refusals 0 failures 0
   stops 0`.

## What it does not give

- Nobody listened during these trials. The owner heard DP audio on `tester.20` on 2026-10-07, which is a report,
  not a measurement.
- Only 44.1 and 48 kHz played. The endpoint publishes 32 kHz as well, and more than two channels and HBR are
  untested (`AFMT_AZ_HBR_ENABLE` 0 and `HBR_CAPABLE` 0 by design).
- No suspend, no resume, no mode set and no driver restart happened, so `DpAudioStop`, `DpAudioResume` and the
  path-power transitions stay unexercised (`stops 0 resumes 0`).
- The monitor stayed plugged in. Step 5 of the design is not written.
- The container ID of step 4 was not in the driver yet at this trial: the ELD carried Linux DC's constant port ID.
