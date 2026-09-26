# M453 - Consistent allocation refresh restores desktop on 143

Unit A, 2026-09-25. Candidate 143 SYS:
3B211162A8E39A8112DD0C6B21497566EA3FA68C33A5068199A8119B79D4E41E.
Enabled 00:33:04 by PnP; Windows boot 2026-09-24 21:14:48 and DWM3032
(start 00:19:31) retained. No OS/AC reset, rollback, AI or Vulkan test.
Native clocks 1000 MHz / VID 116; temperature 67 C; guard 0 and one-shot
controls closed. UMD D438EA42314A3ADD6817665A6133628A64404B5282FB82227230199DA22F768D retained.

## Cause and change

The 139 timing correction advertised the measured 154000000/2568800 Hz
(59.950171286), but DescribeAllocation still returned 60000/1000. Direct3D11
ETW on 142 captured 41 SetDisplayMode errors 0xC01E0005 in a five-second
capture. This repeats the independently recorded M147 mismatch mechanism.

143 caches one complete inherited signal before admitting the device. VidPn
mode queries and DescribeAllocation use exactly that tuple. No MMIO is read
in these queries; the tuple is invalidated across stop/start. Existing
unspecified display-only timing remains. No force-unblank workaround added.

## Validation and scope

- Actual-source host test: 205 checks, zero failures. Reverting only allocation
  refresh to the old 60 Hz causes three expected failures. Full WDK build passes.
- Same five-second Direct3D11 capture after 143: zero SetDisplayMode errors.
  Extracted event records are included; raw XML stays private in scratch.
- Visibility stops looping: five calls, latest TRUE/SUCCESS, no failures.
  Sixteen raw observations show OTG_BLANK_CONTROL zero. Raster has 1024
  successes (992 active, 32 blank), with no API failures.
- Owner first confirms visible desktop with stutter, then confirms smooth
  cursor after the observer has stopped. HardwareAccess observations idle
  graphics scheduling and can perturb presentation. This temporal association
  does not isolate the cause of the transient stutter.
- Quiet sample: DWM 0.859375 CPU seconds, monitor 0.421875, System 0.15625
  over 5.0009654 seconds. Vsync continues near 60 Hz without refusal/defer.
  Idle desktop present rate is not an animation performance benchmark.

Physical visibility and current smooth input are confirmed; no timed long
soak, new AI regression, cold boot, resume or overall M9 completion claimed.
Private device-instance lines were redacted; evidence is otherwise retained.
