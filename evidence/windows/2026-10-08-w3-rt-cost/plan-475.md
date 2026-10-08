# Trial 475: Witcher 3 HIGH, W3 RT effect-cost series arm F all RT + rtwave64: preset w3rt-all with RADV_PERFTEST=rtwave64 (marker C:\BC250\tools\radv-perftest.txt, cleared after the session)

`ARM_LABEL="F all RT + rtwave64" bash scratch/w3-rt-cost/run-arm.sh 475 w3rt-all rtwave64` - method, lab state, conjecture C74, kill and
safety rules as plan-469.md (same location, drive-still input, window B 90 s GPU only, 1920x1080 exclusive fullscreen,
FXAA, no upscaler/FG/DRS, VSync off, LimitFPS 240, direct start, no experiment, overlay summary poll paused).
Expected closure: recovery-unverified (Verify "Confirmed CPU baseline required": the game's 1920x1080 mode commit moves
the KMD epoch, as 466/469); the runner re-confirms the start and checks the stack identities by hand.
Knob launch after the in-game sweep 474 (coordinator, 2026-10-08): load, the drive-still spot, one window B, quit; no menu work. Compared per GHz with 474's in-session all-RT control A-ctl and with 469. RADV_PERFTEST=rtwave64 is a measurement arm only (BD-074: one CTS fail), never a default.

## Result
2026-10-08 01:12:01Z: admission-failed-no-mutation, "Capture failed" before any GPU work. The preflight's stale-marker
gate (251) refused the marker: run-arm.sh wrote it at 01:10:35Z, before the push created the attempt directory, and the
gate admits only a marker newer than that directory. drive-still.sh still queued its look command into
control\native-caps475 (number not reused). Marker removed by the closure step, start already confirmed, identities
unchanged (identity-after475.txt). The rtwave64 arm was then dropped, not repeated: trial 251 bugchecked 0x116 at
world load with rtwave64 (REPORT-251: the knob is the most likely cause), and BD-074 is a wrong ray-query result under
wave64. run-arm.sh now writes the marker when run-slot prints "package flushed" (used by 476).
