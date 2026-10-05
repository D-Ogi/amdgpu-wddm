# BD-023: current SMU message policy review

2026-09-24. Review only; no new message sent or new lab experiment.

The item combines a removed legacy path and a real documentation mismatch:
- QueryGfxclk (0x0F) survives as a compatibility header definition and in AMD's original header,
  but current production sources have no caller. The legacy bc250rd IOCTL is explicitly
  STATUS_NOT_SUPPORTED; M441 already recorded ERROR_NOT_SUPPORTED after writer retirement.
- GetGfxFrequency (0x37) and GetGfxVid (0x38) are called by native paired telemetry and clock
  policy. Both are named in AMD's original smu_v11_8_ppsmc.h and were measured on this unit
  in M22/E04; M441 validates the native owner's paired clock/VID readback. They are absent
  from cyan_skillfish_message_map in the local Linux v6.18 cyan_skillfish_ppt.c.
- GetSmuVersion (0x02), added to native start by BD-025, is in that Linux map, appears in E03's
  init trace (docs/init-sequence.md), and was measured under Windows in M22.
- RequestGfxclk and ForceGfxVid are in Linux's map and existing clock policy. The effective
  native policy is bounded 1000-1500 MHz and 700-900 mV (minima from imported Cyan constants),
  temperature below 85 C, fixed startup target 1000/820 and direction-aware staging.
- The raw transport checks ownership/response/timeouts, not message-number policy. The native
  MMIO helpers allow only mailbox registers. There is NO numeric message allow-list in the
  transport. Its private production call graph currently reaches the five named messages above;
  no raw user message interface exists. Do not claim the transport rejects all other messages.

Suggested correction to the hardware.md SMU rule for main to apply/review:
"The native KMD sends only GetSmuVersion (parameter 0), GetGfxFrequency (0), GetGfxVid (0),
RequestGfxclk and ForceGfxVid through its serialized owner. The two clock setters follow the
Cyan Skillfish clock policy and the limits above. GetSmuVersion and the setters are mapped
by Linux cyan_skillfish_ppt.c; the two telemetry getters are named in AMD smu_v11_8_ppsmc.h
and validated on unit A by M22 and M441. Additional messages require named upstream semantics
and a recorded experiment; header presence alone does not authorize use. No raw SMU escape."

Proposed current status: TRIAGED, until the explicit documentation policy is adopted. QueryGfxclk
part is obsolete after M441; do not delete old comment or pretend getter mismatch never existed.
Proposed append-only comment:
- 2026-09-24 review: Current sources no longer send QueryGfxclk/0x0F; the old raw bc250rd
  IOCTL was retired in M441. GetGfxFrequency/0x37 and GetGfxVid/0x38 remain intentional native
  telemetry/readback calls, named in AMD's original header and measured in M22/M441, although
  absent from Linux's Cyan message map. The documented Linux-map-only rule needs an explicit
  exception for these validated getters. GetSmuVersion/0x02 is in Linux's map and E03/M22.
  Generic transport has ownership/timeout checks, not a numeric message allow-list; the native
  private call graph limits exposed operations. No new hardware message experiment performed.
