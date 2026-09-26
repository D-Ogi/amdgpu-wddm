# M439 - Candidate135 pitch, geometry and thermal hardware controls

Unit A, 2026-09-24. Full WDDM0.7.135.1, SYS SHA256
`3B82B87F13B9592E3CB82D3F3D226C6ABADAA886B3EFAEE4BF1090D0F7BDE979`.
One PnP transition from134 display-only after E30. OS boot21:14:48 and DWM2040
start21:15:56 remain unchanged through the final21:31:48 check. No AC or extra
OS/DWM restart. Source is the dirty working tree over the commit in validation.json;
source snapshots accompany build logs. M435-M437 provide host controls.

## Controls

- Full startup SDMA controls pass, guard confirmed; full one-shot and startup
  SDMA control gates return to0. Display gates remain1. Current8GiB geometry
  agreement and inherited POST reservation pass the new startup checks.
- D3D shared red/blue textures have0/2048 mismatches each, green readback has
  0/307200,60 presents, device-removed status0. Capture geometry1920x1200,
  pitch7680. Retained desktop DLL SHA D438EA42314A3ADD6817665A6133628A64404B5282FB82227230199DA22F768D
  is loaded in DWM: Mesa main/LLVM23 llvmpipe CPU JIT with hardware DCN scanout,
  not GPU D3D rendering. BMP captures are retained privately, not visually
  validated here: local image viewing was unavailable in the tool sandbox.
- Original64MiB residency probe passes3 eviction/restore cycles and4 full GPU
  readbacks, all words match, fences64/128/192/256. This does not revalidate
  the earlier6GiB trial or establish12GiB residency.
- Eight shader hashes match CPU references; stories15M and TinyLlama outputs
  exactly match E14. Native exits0; each actual ICD witness identifies retained
  RADV/ACO SHA DB886B8D53E6BEE89665287AF5EE19A1868E5874868C795F6B11472FBB4A3986.
  No matched Windows/Linux performance claim.
- Final GFX2610/2610 and SDMA6011/6011,0 timeouts/refusals, no TDR.
  DCN991 flips,0 refused/ACK timeouts,36386 vsync acknowledgements,2 deferred
  reports while flip pending. BD-008 remains open; this trial does not fix it.
  Existing1000MHz/VID116 verified; final temperature66.5C, observed control
  peak68.2C. Owner physical-input/long-duration acceptance is not inferred.

## Windows thermal positive control

The newly allowed named THM_TCON_CUR_TMP read uses regcalc-generated offsets.
Its live raw value is bracketed by existing root-bridge SMN reads:24 samples,
22 exact three-way raw matches, all24 BAR temperatures within the SMN bracket.
The two transitions differ by one0.125C sensor step; BAR equals the earlier SMN
word in each. Idle66.375..66.500C; post-workload67.750..67.875C.
The file called thermal-active is actually cooldown: the model worker ended
21:30:00 and these samples ran21:30:12..15. It proves later initialized-state
access, not simultaneous inference. Samples retain UTC timestamps.

Together with Linux M438 this validates the sensor path in the measured states.
The native SMU owner remains INACTIVE: no new PnP/startup/client callsite,
no legacy-writer handover, no native clock transaction tested. Keep existing
bc250rd/startup task until that separate integration is ready.

## Preserved unsuccessful attempts

The first build ran under PowerShell5 and failed UMD packaging because
utf8NoBOM is unsupported; it installed nothing. PowerShell7 completed the fresh
v2 build/package. The first installation observer then rejected a desktop hash
before any PnP action: a global134/133 replacement had changed a hash substring.
The DLL itself was unchanged. The corrected v2 script preserves unrelated hashes
and completed the single transition. Both failed logs remain here; neither is a
GPU failure and no reset was used to hide them.

## Limits and provenance

Physical widths other than1920, own modesetting, handover/bugcheck display,
scanout-buffer lifetime, forced ACK timeout and larger UMA remain untested.
No firmware/NVRAM/SPI write. Full M9 startup/cache/PFN/capture/preemption and
matched performance gates remain open. Existing BD-002/006/010/017 get scoped
comments, not blanket verification. Deployment state is workspace STATE.md.

Raw text is decoded to UTF-8 where needed; only PCI/interface identities are
redacted. Binary frame captures and firmware remain outside public evidence.
SHA256SUMS covers every file except itself. Scripts/build logs preserve exact
control inputs; reused residency probe identity is in small-v2.log.
