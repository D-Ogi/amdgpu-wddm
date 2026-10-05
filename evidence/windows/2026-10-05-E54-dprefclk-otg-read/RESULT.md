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
| `clk-counter-read.txt` | the three reads of `0x6C9FC` |
| `dcn-registers.txt` | `bc250kmd_cli dcn`: 75 named DCN registers and the decoded OTG0 timing |

## What it shows

1. **The reference clock counter reads 600.000 MHz.** `CLK4_0_CLK4_CLK2_CURRENT_CNT` (byte offset
   `0x6C9FC`, `regcalc` over `clk_11_0_1_offset.h`) read `0x00001770` = 6000 in all three reads. The
   register counts in 100 kHz units, so the DisplayPort reference clock (DPREFCLK) is 600.000 MHz on
   this unit. `driver/kmd/display_timing.h` already reads this counter for the pixel clock and
   refuses a zero instead of falling back to a nominal 600 MHz. This run is the first reading of the
   value itself on unit A.
2. **This part has two OTGs, and the firmware lights one.** The dump holds OTG0 and OTG1 and no
   OTG2 or OTG3: `OTG0_OTG_CONTROL` `0x80011311` (master enable set), `OTG1_OTG_CONTROL` `0x00000000`,
   `OTG1_OTG_H_TOTAL` and `OTG1_OTG_V_TOTAL` both 0, `OTG1_OTG_STATUS_FRAME_COUNT` 0. OTG0 decodes to
   h_total 2079, v_total 1234, which is the firmware's inherited 1920x1200 mode (M85, M86).
3. **The HUBP side agrees with the earlier readings**: `HUBP0_DCHUBP_CNTL` `0x000F1002`, HUBP0 surface
   address `0x00000002708D0000`, pitch 1919, flip not pending, no master update lock.

## What it does not show

- One monitor was attached. The run does not show what a second head or an MST hub does, and it does
  not measure a limit of two screens. It shows the pipe count that such a limit would follow from.
- It does not read the audio DTO registers (`DCCG_AUDIO_DTO1_PHASE` and `_MODULE`): they are outside
  the `dcn` escape's register table on this build. The wishlist row L38 keeps that half open.
- DPREFCLK was read at one moment on an idle desktop. It is not a measurement of the clock under a
  mode change.
