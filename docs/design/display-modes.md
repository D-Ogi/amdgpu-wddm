# Display modes

Up to KMD 0.7.216.18 the driver offered one source mode: the size the firmware lit (1920x1200 on unit A). Windows
named the monitor "Generic PnP Monitor", because the driver gave no EDID. A game or a user that wanted 1920x1080
did not get it.

This document has three stages:

| Stage | What it does | State |
| --- | --- | --- |
| A | Reads the EDID over DP AUX, gives the monitor descriptor to Windows, offers smaller source modes and scales them to the unchanged native timing | KMD 0.7.216.19, host tests only |
| B | Changes the timing on the cable: other refresh rates and native sizes (OTG timing, pixel clock, link training) | design only, this document |
| C | Multiplane overlay (MPO) on the free DCN pipes | design only, [mpo.md](mpo.md) |

The code of stage A:

| File | Contents |
| --- | --- |
| `driver/kmd/dpaux.c` | The DP AUX engine and the EDID read (I2C over AUX) |
| `driver/kmd/edid.c` | The EDID parser and the list of source modes |
| `driver/kmd/dcn_scale.c` | The scaler math, the pipe precondition and the register sequence |
| `driver/kmd/display_io.c` | The register accessors: only the offsets of the `gen_regs.py` display tables |
| `driver/kmd/modeset.c` | The owner in the miniport: the switch, the state, start, commit, stop and restore |

The first four files are plain C without WDK headers. The host tests compile them as the miniport does.

## The switch

`EnableDisplayModes` is a `REG_DWORD` under `...\Services\bc250kmd\Parameters`. The driver reads it once at each
start.

| Value | Meaning |
| --- | --- |
| 2 (default) | The EDID, the descriptor, and smaller source modes. A smaller mode is scaled as the VidPN path asks: aspect ratio kept (the default), stretched or centered |
| 1 | The EDID and the descriptor. Smaller modes are centered 1:1, with no scaler ratio |
| 0 | The driver before this feature: no AUX access, no descriptor, one source mode, no scaler write |

A value above 2 counts as 0. The INF, `tools/release/installer/registry-defaults.json` and the measurement guard of
`bc250mon` all write or expect 2.

The level the driver uses can be lower than the switch:

- AUX and the scaler need `EnableMmio` 1 and `EnableDcnWrite` 1. Without them the level is 0.
- Smaller modes need the full WDDM table. The display-only table copies 4-byte pixels at the firmware's pitch, so
  under it the level is 0. The descriptor is still served.
- The pipe precondition (below) can lower the level to 1 or 0.

## Stage A

### The EDID read

The engine is `DP_AUX0` on the DDC1 pads. The sequence is Linux amdgpu's `dce_aux.c` (v6.18, MIT): acquire the
engine, submit the request, poll the status, read the reply, release the engine. The pad set-up is `hw_ddc.c`
`set_config` in AUX mode. The E03 trace (`evidence/linux/2026-09-21-E03-init-trace/amdgpu-events.txt`) has
amdgpu run this sequence on unit A, 204 transactions.

The read runs at start, in `ModesetStart`, at `PASSIVE_LEVEL` under the modeset fast mutex. It never runs at DIRQL
and never in an interrupt or a DPC.

Every wait has a bound:

| Bound | Value | Source |
| --- | --- | --- |
| Poll step of `AUX_SW_STATUS` | 10 us | Linux `REG_WAIT` step |
| One wait for `AUX_SW_DONE` | 4 ms | E03 measured 780 to 920 us for each transaction |
| Stall after a DEFER reply | 1 ms | Linux |
| Retries | 7 DEFER, 7 I2C DEFER, 3 timeout, 2 invalid reply | Linux `dce_aux_transfer_with_retries` |
| All stalls of the whole read | 200 ms | `MODESET_AUX_BUDGET_US` |

When the budget is spent, every later wait fails at once. The read then stops with the blocks it has.

The read does this:

1. Opens the session. If the firmware left `AUX_EN` at 0, the session refuses and writes nothing. Linux enables
   and resets the engine in that case. This driver does not, because the GOP reads the EDID with the same engine,
   so an engine that is off is not the expected state.
2. Sets `AUX_PAD1_MODE` if it is 0 and clears `DDC_PAD1_I2CMODE`, as `set_config` does.
3. Reads 16 bytes of DPCD from address 0 (the receiver capabilities). The log gets the DPCD revision, the maximum
   link rate and the maximum lane count. Stage B starts from these bytes.
4. Reads block 0 of the EDID at I2C address `0x50`: the offset write, 16-byte reads, the stop.
5. Reads as many extensions as block 0 announces, up to three. A block past the first two gets the E-DDC
   segment pointer at address `0x30` first.
6. Closes the session. The pad registers go back to the values found at open.

### The parser

`Bc250EdidParse` checks the header, the version and the checksum of each block. The verdict:

| Verdict | Meaning | Result |
| --- | --- | --- |
| `OK` | All blocks are good | The EDID is used and served whole |
| `EXTENSION_DROPPED` | Block 0 is good, an extension is bad or missing | Block 0 is used and served alone |
| `SHORT`, `HEADER`, `CHECKSUM`, `VERSION` | No usable EDID | No descriptor. The mode list has the native mode and the common sizes |

The parser keeps the manufacturer, the product code, the monitor name, the preferred timing, the sizes of the
detailed, standard and established timings, the CTA short video descriptors, the range limits, the short audio
descriptors and the speaker allocation. It does not keep the serial number, and the log has no raw EDID bytes.

### The monitor descriptor

`DxgkDdiQueryDeviceDescriptor` returns the EDID that the start read:

- No usable EDID: `STATUS_MONITOR_NO_DESCRIPTOR`.
- An offset at or past the end: `STATUS_MONITOR_NO_MORE_DESCRIPTOR_DATA`.
- Else the bytes from the offset, never more than `DescriptorLength`. A request past the end gets the rest and
  zeros after it.

Windows then names the monitor from the EDID, not "Generic PnP Monitor".

### The source modes

`Bc250ModeListBuild` makes the list:

1. The native size (the size the firmware lit) is first, always.
2. Then every size of the EDID: detailed, standard, established and CTA timings.
3. Then the common desktop sizes: 1920x1080, 1680x1050, 1600x900, 1440x900, 1400x1050, 1280x1024, 1280x960,
   1280x800, 1280x720, 1152x864, 1024x768, 800x600 and 640x480.

A size that is wider or taller than the native size is not on the list. Stage A only scales up or keeps 1:1. A
size under 640x480 is not on the list. The list has no duplicates and holds at most 24 sizes. After the native
entry, the sizes go from the widest to the narrowest. At level 0 the list has the native size only.

`display.c` offers each size of the list in each pixel format of `display_modes.h`. The native size keeps the
firmware's pitch. A smaller size gets the pitch of its primary allocation (`DcnPrimaryPitch`). The target mode set
does not change: one target mode, the native timing. So every source mode has the native refresh rate.

### Scaling

The VidPN present path names the scaling. The driver supports these:

| Source size | Level 2 | Level 1 | Default when not pinned |
| --- | --- | --- | --- |
| Native | Identity, centered, stretched, aspect ratio | Identity, centered | Identity |
| Smaller | Centered, stretched, aspect ratio | Centered | Aspect ratio (level 2), centered (level 1) |

At the native size every scaling is the same 1:1 shape, so the driver programs Identity. This keeps the native
mode when dxgkrnl pins a scaling for the monitor. A smaller size of the native aspect ratio also gets all three
scalings. With a scaling pinned on the path, `OfferSourceMode` offers only the sizes that support that scaling.

The plan follows Linux `calculate_scaling_ratios` and `calculate_init_and_vp`, with `dpp201_get_optimal_number_of_taps`
for the taps:

- Aspect ratio: the largest rectangle with the source's aspect ratio that fits, centered.
- Stretched: the whole active area.
- Centered: the source size 1:1 in the middle.
- The area outside the rectangle is the MPCC background, black.
- A ratio of 1 gets 1 tap and `DSCL_MODE` 0 (bypass). A ratio under 1 gets 4 taps, `DSCL_MODE` 1 and the
  4-tap 64-phase upscale filter.

The filter table is extracted from Linux `dce_scl_filters.c`, not typed by hand. `run_modeset.ps1` checks
`scl_filter_4tap_64p_upscale.inc` against the reference file before each run.

### The pipe precondition

At start the driver reads pipe 0 and changes it only when it has the shape of one pipe, scanned 1:1:

| Check | Verdict when it fails | Highest level |
| --- | --- | --- |
| Viewport 0,0 and the native size | `VIEWPORT` | 0 |
| Recout 0,0 and the native size | `RECOUT` | 0 |
| `MPC_SIZE` the native size | `MPC_SIZE` | 0 |
| `DSCL_MODE` 0 or 1 | `DSCL_MODE` | 0 |
| MPCC 0 fed by DPP 0 | `MPCC_TOP` | 0 |
| OPTC segment 0 fed by OPP 0 | `ODM` | 0 |
| `LUT_MEM_PWR_FORCE` 0 (coefficient memory on) | `COEF_POWER` | 1 |

The GOP of unit A leaves `DSCL_MODE` 1 at a 1:1 ratio (E03). The check accepts it.

### The commit

`CommitVidPn` and `UpdateActiveVidPnPresentPath` call `ModesetCommit` with the source size and the path's
scaling. The commit:

1. Refuses a size that is not on the list.
2. Does nothing when pipe 0 already has this size and scaling.
3. Computes the whole plan before the first write. A size the plan refuses writes nothing.
4. Takes the primary transaction (`WddmPrimaryExclusiveBegin`). A flip in progress makes the commit wait, at
   most 2 ms. While the commit holds the transaction, `SetVidPnSourceAddress` returns `STATUS_DEVICE_BUSY`.
5. For a larger source size (grow), and when the plane scans a surface other than the firmware's: clears the
   firmware surface to black and puts the plane on it. The firmware surface holds the native size, so the larger
   viewport never reads past the end of a smaller surface.
6. Writes the scaler under `OTG_MASTER_UPDATE_LOCK`, then unlocks and triggers. The lock waits at most ten 1 us
   stalls, as `DcnFlipWriteSequence` does.
7. Reads `HUBP_UNDERFLOW_STATUS` and counts an underflow.
8. Ends the transaction. After a grow, the end clears the primary record, so the next flip programs the plane
   again.

A smaller source size (shrink) needs no flip first: the new viewport reads a part of what the plane reads now.

The scaler writes go to `DSCL0`, `HUBP0` (viewport), `MPCC0` (background) and `OTG0` (lock and trigger). Every
offset is in the display write table of `gen_regs.py`. `display_io.c` refuses any other offset.

The commit does not change the OTG timing, the pixel clock, `DISPCLK`, `DPPCLK` or the link. The DPP clock is
sufficient: DML (`display_mode_vba_20.c`) asks for `DPPCLK` = pixel clock x max(vtaps / 6 x min(1, hratio),
hratio x vratio / throughput, 1). For an upscale with 4 taps that is the pixel clock, which the firmware's
`DPPCLK` already supports at the native size.

### Where the source size is used

`DisplaySourceWidth` and `DisplaySourceHeight` (`bc250kmd.h`) give the committed size, or the native size when no
mode is committed. These paths use them:

- `SetVidPnSourceAddress`: the surface size check and the scan-out admission.
- `DcnFlipSourceAddress` and `DcnScanoutMapping`: the address and size checks of the plane.
- The present blit of the display-only path: the destination size.
- The framebuffer dump.

The diagnostic flip escape uses the firmware's size. It refuses while a smaller mode is committed.

### Restore paths

| Event | What the driver does |
| --- | --- |
| Stop (`DcnStop`) | The firmware surface, then the native 1:1 shape. One log line |
| Bugcheck (`SystemDisplayEnable`) | The firmware surface, then the native shape. No lock, no log |
| `ResetDevice` | The same as the bugcheck, if pipe 0 has a smaller mode |
| D3 | The firmware surface and the native shape. The committed mode stays in the state |
| D0 | Reads pipe 0 again. If the shape passes the precondition, programs the committed mode again. Else drops it and keeps the native mode |
| A failed D3 transition | The same as D0 |

The native shape is the one amdgpu programs for 1:1: `DSCL_MODE` 0, the full viewport, recout and `MPC_SIZE`.

### DP audio step 4

The same EDID read feeds DP audio. `DpAudioStart` gets the parsed EDID from `ModesetEdidForAudio`. When the EDID
has an LPCM short audio descriptor, the endpoint gets:

- The manufacturer and the product code (`SINK_INFO0`).
- The monitor name, up to 18 characters (`SINK_INFO1` and `SINK_INFO4` to `SINK_INFO8`).
- The LPCM descriptor: channels (at most 8), sample rates and sample sizes.
- The speaker allocation, or FL, FR and FC when the EDID has none (5, Linux `DEFAULT_SPEAKER_LOCATION`).

Without an EDID or without LPCM, the endpoint gets the fixed set of steps 1 and 2. [dp-audio.md](dp-audio.md) has
the details. `HBR_CAPABLE` stays 0. The port ID stays Linux's constant until the container ID work.

### Deviations from Linux

| Deviation | Reason |
| --- | --- |
| No enable and reset of an AUX engine that the firmware left off | The GOP uses the engine. An engine that is off is a state this code has not seen |
| The scaler filter is loaded at each scaled commit | Linux keeps a cache of the last filter. This driver keeps none across a stop and a start |
| Only one pipe, RGB, no rotation, upscale or 1:1 | Stage A scope. A downscale needs other `DPPCLK` and DCHUB request settings |
| No DLG and TTU (DCHUB request) programming for the new viewport | Stage A writes no DCHUB request register. A smaller viewport reads less data. The underflow counter is the check |
| The native restore writes amdgpu's bypass shape, not the GOP's `DSCL_MODE` 1 | Both are 1:1. The bypass shape is the one Linux programs |
| A dropped EDID extension is not served | The descriptor ends with block 0. Monitor.sys gets the end-of-data status for the extension that block 0 announces |

### Host tests

`driver/kmd/test/run_modeset.ps1` is the `modeset` gate of `tools/quality/quick.ps1`:

| Test | Checks | Negative controls |
| --- | --- | --- |
| `edid_test.c` | 135 | Each parser verdict: short, bad header, bad checksum, bad version, a dropped extension |
| `dpaux_test.c` | 115 | DEFER and I2C DEFER past the retry limit, NACK, I2C NACK, timeouts, the spent budget, an engine left off |
| `dcn_scale_test.c` | 1044 | Sizes the plan must refuse, each pipe shape the precondition must refuse, a lock that never acknowledges |

The EDID of the lab monitor (from E03, serial descriptor removed, checksums computed again) is
`driver/kmd/test/edid_lab_redacted.h`. The DP audio test (`dpaudio_test.c`, 858 checks) runs step 4 with it.

The suites that extract driver functions (`vidpn-flip`, `dcn-flip`, `post-display`, `scanout-geometry`,
`display-timing`) cover the new size helpers, the escape refusal and the restore order.

### Risks and open items

- No lab trial has run stage A. The AUX sequence, the EDID read and the scaler are proven only against models.
- The DCHUB request settings stay as the firmware set them for the native size. If an upscaled mode underflows,
  the underflow counter and the screen give the first sign. The fix is the DLG and TTU programming of Linux
  `hubp2_setup`.
- dxgkrnl may pin a scaling that the driver did not expect for the monitor. The native mode reports every
  scaling to prevent that.
- An external monitor change (hot plug) is not handled. The EDID is read once at each start.

## The lab plan for stage A

Each trial is at most three minutes. The overlay tells the owner what happens next. The modes are set with
`tools/win/display-modes/` in the console session, because SSH runs in session 0.

| Trial | Steps | Pass |
| --- | --- | --- |
| A1 | Deploy 0.7.216.19 with `EnableDisplayModes` 2. Restart Windows. Read the KMD log | The log has the AUX counters, `EDID ok`, the monitor name, `pipe 0 ok`, `requested 2 level 2` and the mode list |
| A2 | `dxgimodes.exe --all`. Device Manager or `Get-CimInstance WmiMonitorID` | The KMT and DXGI lists have the smaller sizes. Windows names the monitor from the EDID |
| A3 | `modes.ps1 -Set 1920 1080 -HoldSeconds 30`, then a screenshot | The desktop is 1920x1080. On the monitor it fills the width. Black bars of 60 lines are at the top and the bottom (`dscl 0`, no scaling). The log has `commit 1920x1080 aspect-ratio -> 0x00000000` |
| A4 | `-Set 1280 720`, then `-Set 1920 1080 -Fixed stretch`, each with a hold of 20 s | 1280x720 scaled 1.5 times to 1920x1080 with bars (`taps 4/4 dscl 1`). Then 1920x1080 stretched to the whole screen |
| A5 | The hold ends, the tool goes back to the registry mode | 1920x1200 again. The log has a commit back to the native size. The stop line of the next stop has 0 underflows |

A screenshot captures the source surface, not the scaler's output. The owner's view of the monitor is the check
of the bars and the scale.

The tool sets each mode without `CDS_UPDATEREGISTRY`, so a restart always comes back to the registry mode. It goes
back to the registry mode itself when the hold ends, also when the screen is black.

Recovery when the screen stays black or wrong:

1. Wait for the hold to end. The tool goes back to the native mode.
2. If the screen is still black, set `EnableDisplayModes` to 0 over SSH:
   `reg add HKLM\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters /v EnableDisplayModes /t REG_DWORD /d 0 /f`
3. Restart Windows. At level 0 the driver does not touch AUX or the scaler.
4. If SSH does not answer, use the emergency channel, then the plug.

## Stage B: the timing on the cable

Stage B changes the native timing: another refresh rate, or a native size other than the firmware's. This is a
design. No code exists.

### What Linux does on DCN 2.0.1

- `DISPCLK` and `DPPCLK` come from the DENTIST dividers, written directly by `dcn20_update_clocks_update_dentist`
  (`clk_mgr/dcn201/dcn201_clk_mgr.c`). No SMU message sets them on this ASIC. The KMD stays the single SMU owner,
  and stage B sends no new SMU message.
- The pixel clock of a DP stream is the DCCG DP DTO (phase and modulo of the DP reference clock). Linux programs it
  directly, not through the VBIOS.
- The PHY and the link rate go through the VBIOS command table `DIG1TransmitterControl`
  (`dcn10_link_encoder_enable_dp_output` calls `transmitter_control` of the BIOS parser). A link rate change
  therefore needs an AtomBIOS interpreter in the KMD, or a direct PHY sequence that nobody has measured on this
  part.
- Link training is DPCD writes over AUX (`DP_LINK_BW_SET`, `DP_LANE_COUNT_SET`, `DP_TRAINING_PATTERN_SET`) and
  the training patterns of the DP encoder.

### The steps

| Step | What | Needs | Go when |
| --- | --- | --- | --- |
| B0 | Read the link the GOP trained: DPCD `0x100`, `0x101`, `0x202` to `0x207`, the encoder's lane and rate registers. Read only | Stage A AUX | The lab log has the rate, the lane count and the lane status |
| B1 | Another timing at the same link rate and lane count: OTG timing, DP DTO, the DP stream encoder's MSA and `VID_M`/`VID_N`. No PHY change, no retraining | B0, a timing whose bandwidth fits the trained link, the OTG blanked during the change | One trial changes 1920x1200 at 60 Hz to a lower refresh rate and back, with a picture at each step |
| B2 | `DISPCLK` and `DPPCLK` through the DENTIST, only downwards or to the value the firmware set | B1 | The DENTIST read-back and a picture |
| B3 | Link training at another rate: the transmitter control table through an AtomBIOS interpreter (amdgpu `atom.c`, MIT, imported) | B1, B2, the VBIOS read from the device at run time | Training passes, `0x202` to `0x207` report lock, the picture returns |

B1 covers lower refresh rates and every native size whose pixel rate fits the trained link. B3 is necessary only
for timings that need more bandwidth, such as higher refresh rates.

### Lab risk

- A wrong OTG timing or DP DTO gives a black screen or "no signal". The monitor recovers when the firmware's
  timing returns. The KMD must keep the GOP timing to restore at stop, bugcheck and D3, as stage A keeps the
  native shape.
- A failed link training leaves the link down until a restart. B3 needs the emergency channel ready and a
  restart plan for each trial.
- `DISPCLK` above the firmware's value needs a voltage that the SMU sets for its own DPM state. B2 never raises
  `DISPCLK` above the firmware's value, so no voltage change is necessary.
- The VBIOS image stays outside the repository. The driver reads it from the device at run time.

### Go and no-go

- No-go for B1 until stage A passes A1 to A5 on the lab.
- No-go for B3 until B1 and B2 pass, and the AtomBIOS interpreter passes host tests against a VBIOS image kept
  outside the repository.
- Each step has its own switch, default off, until its lab trial passes.
