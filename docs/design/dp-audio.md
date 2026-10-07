# DisplayPort audio

The monitor on unit A has speakers or a headphone jack. Its sound comes over the DisplayPort link, next to the
picture. Windows plays it through the inbox HD Audio class driver and the Azalia codec of the display block. That
codec answers on the GPU's audio function, but the display driver must do three things first:

1. Make the codec's endpoint present for the monitor (the pin configuration, the format descriptor and
   `AUDIO_ENABLED`).
2. Give the stream encoder a 24 MHz audio wall clock (the DCCG audio DTO).
3. Turn on the audio path of the DP stream encoder (the AFMT block and the DP secondary-data packets).

The KMD does this in `driver/kmd/dpaudio.c` (switches, lock, record, log, entry points and the escape) and
`driver/kmd/dpaudio_seq.c` (the register work, with no Windows code in it). The sequences are Linux amdgpu's:
`dce_audio.c` for the endpoint and the DTO, `dcn10_stream_encoder.c` for the stream encoder (Linux v6.18).
`driver/kmd/test/dpaudio_test.c` checks every write against a fake register file.

The work has six steps. Steps 0 to 2 are in the driver from KMD 0.7.216.1. Steps 3 to 5 are not written yet.

| Step | What it does | State |
| --- | --- | --- |
| 0 | Reads the codec, the straps, the encoders, the endpoints and the reference clock. Writes nothing | 0.7.215.1 |
| 1 | Configures the endpoint and sets `AUDIO_ENABLED` | 0.7.215.1, changed in 0.7.216.1 |
| 2 | Starts the audio stream: wall DTO, AFMT, DP_SEC packets | 0.7.216.1 |
| 3 | A tone through the new endpoint, heard at the monitor | lab trial to do |
| 4 | The real ELD from the monitor's EDID, the container ID | not written |
| 5 | Hot plug of the monitor | not written |

## Why the endpoint waits for the stream

Under 0.7.215.1, step 1 ran alone. Windows then showed an active "Digital Audio (HDMI)" endpoint. Its audio clock
was not programmed, so it played no sound. Edge slaved its video to that clock and played video at about 0.7 times
real time. An endpoint without a stream is worse than no endpoint.

From 0.7.216.1 the driver never shows the endpoint without its stream:

- `AUDIO_ENABLED` is the last write of a start.
- If the stream half is off, refused or failed, `AUDIO_ENABLED` stays 0.
- `EnableDpAudioEndpoint` 0 and `EnableDpAudioStream` 0 each refuse the whole start.

## Switches

All three are `REG_DWORD` values under `...\Services\bc250kmd\Parameters`. The driver reads them at every start
and every resume. Only the value 1 opens a switch.

| Switch | Default | 0 means |
| --- | --- | --- |
| `EnableDpAudio` | 1 | No audio register is read or written. The driver behaves as before this feature |
| `EnableDpAudioEndpoint` | 1 | The start is refused. No endpoint, no stream |
| `EnableDpAudioStream` | 1 | The start is refused. No endpoint, no stream |

The INF and `tools/release/installer/registry-defaults.json` both write the three defaults.

## Preconditions

The start reads first and writes nothing unless every check passes (`Bc250DpAudioDecide`). The reason for a
refusal goes to the record, the log and the CLI.

| Check | Expected on unit A | Reason when it fails |
| --- | --- | --- |
| BAR5 mapped | `EnableMmio` 1 | `NO_MMIO` |
| Codec vendor and device | `0x1002AA01` ([M819](../facts/linux.md#m819)) | `CODEC_ID` |
| `DC_PINSTRAPS_AUDIO` | not 0 | `STRAPS` |
| DP stream encoders with video on | exactly one | `NO_STREAM`, `TWO_STREAMS` |
| Its DIG back end | DP SST, fed by that encoder | `NOT_DP_SST` |
| The endpoint's `RESPONSE_CONFIGURATION_DEFAULT` | `0x185600F0` | `CONFIG_DEFAULT` |
| `CLK4_0_CLK4_CLK2_CURRENT_CNT` (100 kHz units) | 5000 to 7000. Unit A reads 6000 ([M788](../facts/display.md#m788)) | `REFCLK` |

A register that cannot be read gives `READ_FAILED`.

## The start sequence

The groups run in this order. Each group stops at its first failed write.

1. **hw_init** on endpoint 0 (`dce_aud_hw_init`): the clock-gating bracket, the rates 32, 44.1 and 48 kHz, and
   `CLKSTOP` and `EPSS`.
2. **configure** of the endpoint (`dce_aud_az_configure`, the DP branch): speaker allocation FL and FR, one LPCM
   descriptor of 2 channels at 32/44.1/48 kHz and 16 bit, the lip-sync and HBR fields, and the sink name
   "BC-250 DP".
3. **stream** on the encoder of the monitor. The table below has the writes.
4. **`AUDIO_ENABLED`** (`dce_aud_az_enable`): `CLOCK_GATING_DISABLE` 1 with `AUDIO_ENABLED` 1, then
   `CLOCK_GATING_DISABLE` 0.

Linux enables the endpoint before the audio packets. This driver sets `AUDIO_ENABLED` last. The HD Audio side sees
`AUDIO_ENABLED` as a plugged sink, and the class driver can open a stream at once. With this order Windows never
sees an endpoint whose wall clock and packets are not running, and a failure anywhere before it leaves no endpoint.

### The stream writes

Every write is a read-modify-write of the named bits only. The driver reads the register back at once and compares
the named bits. The two update strobes (`AFMT_60958_CS_UPDATE` and `AFMT_AUDIO_INFO_UPDATE`) are not compared,
because the hardware can clear them. The values are what the fake register file of the host test expects for DP0
and endpoint 0, from the register state the lab read under 0.7.215.1.

| # | Step name | Register | Field | Value written | Register after |
| --- | --- | --- | --- | --- | --- |
| 1 | `DTO_SELECT` | `DCCG_AUDIO_DTO_SOURCE` | `DCCG_AUDIO_DTO_SEL` | 1 (DTO1) | `0x00000010` |
| 2 | `DTO1_MODULE` | `DCCG_AUDIO_DTO1_MODULE` | whole | reference count x 1000 | 6 000 000 |
| 3 | `DTO1_PHASE` | `DCCG_AUDIO_DTO1_PHASE` | whole | 240 000 | 240 000 |
| 4 | `DTO_512FBR` | `DCCG_AUDIO_DTO_SOURCE` | `DCCG_AUDIO_DTO2_USE_512FBR_DTO` | 1 | `0x00100010` |
| 5 | `AFMT_CLOCK_ON` | `DIGn_AFMT_CNTL` | `AFMT_AUDIO_CLOCK_EN` | 1 | `0x101` |
| 6 | `SRC_SELECT` | `DIGn_AFMT_AUDIO_SRC_CONTROL` | `AFMT_AUDIO_SRC_SELECT` | the endpoint | 0 |
| 7 | `CHANNEL_ENABLE` | `DIGn_AFMT_AUDIO_PACKET_CONTROL2` | `AFMT_AUDIO_CHANNEL_ENABLE` | `0x03` (FL, FR) | `0x300` |
| 8 | `AUD_N` | `DPn_DP_SEC_AUD_N` | `DP_SEC_AUD_N` | `0x8000` | `0x8000` |
| 9 | `TIMESTAMP` | `DPn_DP_SEC_TIMESTAMP` | `DP_SEC_TIMESTAMP_MODE` | 1 (auto) | 1 |
| 10 | `CS_UPDATE` | `DIGn_AFMT_AUDIO_PACKET_CONTROL` | `AFMT_60958_CS_UPDATE` | 1 | `0x04000800` |
| 11 | `LAYOUT_OVRD` | `DIGn_AFMT_AUDIO_PACKET_CONTROL2` | `AFMT_AUDIO_LAYOUT_OVRD`, `AFMT_60958_OSF_OVRD` | 0 | `0x300` |
| 12 | `INFO_UPDATE` | `DIGn_AFMT_INFOFRAME_CONTROL0` | `AFMT_AUDIO_INFO_UPDATE` | 1 | `0x80` |
| 13 | `CLOCK_ACCURACY` | `DIGn_AFMT_60958_0` | `AFMT_60958_CS_CLOCK_ACCURACY` | 0 | 0 |
| 14 | `SEC_ASP_ON` | `DPn_DP_SEC_CNTL` | `DP_SEC_ASP_ENABLE` | 1 | `0x0010` |
| 15 | `SEC_ATP_AIP_ON` | `DPn_DP_SEC_CNTL` | `DP_SEC_ATP_ENABLE`, `DP_SEC_AIP_ENABLE` | 1 | `0x1110` |
| 16 | `SEC_STREAM_ON` | `DPn_DP_SEC_CNTL` | `DP_SEC_STREAM_ENABLE` | 1 | `0x1111` |
| 17 | `SAMPLE_SEND_ON` | `DIGn_AFMT_AUDIO_PACKET_CONTROL` | `AFMT_AUDIO_SAMPLE_SEND` | 1 | `0x04000801` |

Steps 1 to 4 are the DP branch of `dce_aud_wall_dto_setup`. Step 5 is `enc1_se_enable_audio_clock`. Steps 6 and 7
are `enc1_se_audio_setup`. Steps 8 to 13 are `enc1_se_setup_dp_audio`. Steps 14 to 16 are
`enc1_se_enable_dp_audio`, which sets `DP_SEC_STREAM_ENABLE` after all the other enables. Step 17 is
`enc1_se_audio_mute_control`. Linux writes `AUD_N`, the timestamp and the source select as whole registers. This
driver changes only their named fields, so a bit that the firmware set next to them stays.

`DP_SEC_CNTL` is live on the main link. It also carries the info-frame packets (the GSP and MPG bits) that the
firmware may have turned on. Only the four audio bits change, and the test checks that the other bits stay.

After the 17 writes the registers hold the values Linux left on unit A ([M820](../facts/linux.md#m820)), with one
difference: the DTO1 module.

### The DTO1 module

The wall DTO makes the 24 MHz audio clock from the DP reference clock. Clock = reference x phase / module. The
phase is 240 000 (24 MHz in units of 100 Hz). So the module must be the reference clock in units of 100 Hz.

Linux takes the reference from its clock manager (`dp_dto_source_clock_in_khz`). The clock manager lowers it for
the VBIOS spread-spectrum figure, to 598.874 MHz. That gives the module 5 988 740 that M820 read. But the counter
`CLK4_0_CLK4_CLK2_CURRENT_CNT` reads 6000, which is 600.000 MHz (M788). The clock on this board does not honour the
VBIOS spread-spectrum flag (M788 detail), and upstream Linux now disables DP audio spread spectrum for Cyan
Skillfish. With Linux's module the wall clock would be 24.045 MHz, 0.19 % fast. That is a pitch and drift error.

This driver therefore uses the counter: module = count x 1000. On unit A that is 6 000 000 and the wall clock is
24.000 MHz. The driver reads the counter at every start. A count outside 5000 to 7000 refuses the start with
`REFCLK`, so a counter that reads 0 or nonsense never gives a module.

### Failure and mismatch

A failed write, or a read-back whose named bits differ, ends the start at once. The driver then runs the stop
sequence below: the stream off if the stream group began, then `AUDIO_ENABLED` 0. The reason is `WRITE_FAILED` or
`STREAM_MISMATCH`. The record keeps the step, the offset, the bits written and the bits read.

## The stop sequence

`DpAudioStop` runs at StopDevice (before `WddmStop` and `DcnStop`) and on the way to D3. The path-power transition
to off runs the same sequence. It is the start in reverse:

1. `AFMT_AUDIO_SAMPLE_SEND` 0.
2. `DP_SEC_STREAM_ENABLE` 0, first and alone.
3. `DP_SEC_ATP_ENABLE` and `DP_SEC_AIP_ENABLE` 0.
4. `DP_SEC_ASP_ENABLE` and `DP_SEC_ACM_ENABLE` 0.
5. If `DP_SEC_CNTL` is still not 0 (info-frame bits), `DP_SEC_STREAM_ENABLE` 1 again.
6. `AFMT_AUDIO_CLOCK_EN` 0.
7. `AUDIO_ENABLED` 0, inside the clock-gating bracket (`dce_aud_az_disable`).

Every step runs, also after a failed one, so one bad write cannot leave the packets on.

Linux does the same in `disable_dio_audio_packet`: `enc1_se_audio_mute_control(true)`, then
`enc1_se_dp_audio_disable`, then `dce_aud_az_disable`. `enc1_se_dp_audio_disable` clears ASP, ATP, AIP, ACM and
`DP_SEC_STREAM_ENABLE` in one write, and sets `DP_SEC_STREAM_ENABLE` again when other bits remain. This driver
splits that write into steps 2 to 5, in the reverse order of the enable. Step 5 is Linux's rule.

The DTO, the source select, the channel enable, `AUD_N` and the timestamp mode stay as the start left them. Linux
does not reset them either. The DTO is a clock that drives no pin and no video, and the other fields do nothing
while the packets are off. The next owner of the display (Basic Display, or the next start) finds the stream off
and `AUDIO_ENABLED` 0.

When the monitor path powers on again, or the driver comes back to D0, the start runs again from the beginning.

## The record and the CLI

`BC250_ESCAPE_RUN_DPAUDIO` has two exact sizes:

- ABI 2 (480 bytes, KMD 0.7.216.1 and later): the ABI 1 record followed by the stream record.
- ABI 1 (408 bytes): the 0.7.215.1 layout. A 0.7.216.1 driver still answers it.

The stream record holds the switch `EnableDpAudioStream`, the stream state (off, on, undone), the step and status
of the last stream sequence, the reference count, the DTO1 module and phase, the read-backs of `DTO_SOURCE`,
`DP_SEC_CNTL`, `AFMT_CNTL` and both packet-control registers, the mismatch triple, and the counters of stream
starts, stops and undos.

`bc250kmd_cli dpaudio` prints the check table (with the reference-clock row), the decision a start would take now,
the raw slots and the record. `bc250kmd_cli dpaudio state` prints the record alone. The CLI asks with ABI 2 and asks
again with ABI 1 when an older driver refuses the size.

## Not proven yet

- No lab trial has run step 2. The host test proves the order and the values against a fake register file only.
- Nobody has heard the tone. During the Linux visit L1007 no headphones were on the monitor.
- `AUDIO_ENABLED` last is a deviation from Linux. It needs the step 3 trial to show that the class driver still
  finds the endpoint.
- The read-back of the update strobes is not known. The driver does not compare them.
