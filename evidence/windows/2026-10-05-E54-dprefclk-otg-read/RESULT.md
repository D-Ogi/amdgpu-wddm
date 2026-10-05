# E54: the DisplayPort reference clock counter and the second OTG on unit A

Date: 2026-10-05, 02:25-02:28Z. Unit A, Windows 11 Pro build 22631, kernel driver 0.7.205.1,
desktop on the GPU route, one DisplayPort monitor at 1920x1200. Read-only: two diagnostic escapes of
`bc250kmd_cli`, no write.

Commands (`bc250kmd_cli` of the b10 package, `C:\BC250\tmp\gui-b10\prepared\payload\tools`):

```
bc250kmd_cli dcn                 # the DCN register dump and its decoded timing
bc250kmd_cli read 0x6C9FC        # CLK4_0_CLK4_CLK2_CURRENT_CNT, three times
```

## Files

| File | Content |
|---|---|
| `clk-counter-read.txt` | the three reads of `0x6C9FC`, with the run time on its first line |
| `dcn-registers.txt` | `bc250kmd_cli dcn`: 75 named DCN registers and the decoded OTG0 timing |

The first line of `dcn-registers.txt` names the `bc250kmd_cli.exe` of the b10 package and that file's
timestamp (2026-10-04T03:19:52Z), not the time of this run. The run time is in `clk-counter-read.txt`.

## What it shows

1. **The reference clock counter reads 600.000 MHz.** `CLK4_0_CLK4_CLK2_CURRENT_CNT` (byte offset
   `0x6C9FC`, `regcalc` over `clk_11_0_1_offset.h`) read `0x00001770` = 6000 in all three reads. The
   register counts in 100 kHz units, so the DisplayPort reference clock (DPREFCLK) is 600.000 MHz on
   this unit. `driver/kmd/display_timing.h` already reads this counter for the pixel clock and
   refuses a zero instead of falling back to a nominal 600 MHz. This run is the first reading of the
   value itself on unit A.
2. **The firmware lights OTG0 and leaves OTG1 dark.** `OTG0_OTG_CONTROL` `0x80011311` (master enable
   set), `OTG1_OTG_CONTROL` `0x80000310` (master enable clear, `AVSYNC_VSYNC_N_HSYNC_MODE` set, the same
   value M86 read under Linux), `OTG1_OTG_H_TOTAL` and `OTG1_OTG_V_TOTAL` both 0,
   `OTG1_OTG_STATUS_FRAME_COUNT` 0. OTG0 holds `OTG_H_TOTAL` field 2079 and `OTG_V_TOTAL` field 1234.
   Both registers carry the total minus one (`driver/kmd/display_timing.h` adds the one back), so the
   mode is h_total 2080 by v_total 1235: the firmware's inherited 1920x1200 mode (M85, M86). The
   decoded line of the `dcn` escape printed the raw field under the name `h_total`, which invites that
   off-by-one. `bc250kmd_cli` now prints the register name and the decoded total next to it.
3. **The HUBP side agrees with the earlier readings**: `HUBP0_DCHUBP_CNTL` `0x000F1002`, HUBP0 surface
   address `0x00000002708D0000`, pitch 1919, flip not pending, no master update lock.

## What it does not show

- One monitor was attached. The run does not show what a second head or an MST hub does, and it does
  not measure a limit of two screens.
- The dump does not show how many timing generators the part has. The escape reads a fixed table that
  holds OTG0 and OTG1 only (`driver/kmd/gen_regs.py` builds it over `range(2)`), so a third one could
  not appear in it. The count comes from the source instead:
  `third_party/linux-amdgpu/dcn_2_0_1_offset.h` defines `mmOTG0_*` and `mmOTG1_*` and no `mmOTG2_*` or
  `mmOTG3_*`, and `dcn201_resource.c` sets `num_timing_generator` 2 against `pipe_count` 4. A limit of
  two screens would follow from the timing-generator count, not from the pipe count.
- It does not read the audio DTO registers (`DCCG_AUDIO_DTO1_PHASE` and `_MODULE`): they are outside
  the `dcn` escape's register table on this build. The wishlist row L38 keeps that half open.
- DPREFCLK was read at one moment on an idle desktop. It is not a measurement of the clock under a
  mode change.
