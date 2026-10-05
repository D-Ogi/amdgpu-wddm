# M441 - Native SMU startup and coordinated client handover

Unit A,2026-09-24. Candidate0.7.136.1 SYS
`1C93F3578BC53FFA4DF0C32B2B12C93201671A54754D4B59C27EDE1613C1517D`.
One PnP transition from135, no Windows/DWM/AC restart. Boot21:14:48 and
DWM2040/start21:15:56 retained through22:24:18. Display/RLC sub-agent changes
made after frozen-build are excluded from this installed binary.

## Source and host controls

PnP starts the embedded native SMU owner only after the explicit handover gate.
Stop joins it before engine/translation teardown; adapter low-power notification
also closes it. Full startup completes a1000MHz/820mV transaction before GART,
PSP, IH or GFX activation, and records its readiness. Failure prevents activation.
409 actual coordinator checks pass; omitting readiness gives7 failures.
30675 native-owner checks pass. KMD, CLI/DLL, reader and monitor builds pass.

## Measured handover

- Before-handover backup and13 staged artifact hashes verified. Old monitor/clock
  task stopped; old bc250rd unloaded before replacement. New reader SYS
  `EB7FCA76B14A0C0817333509C8A88073571A3D607D5AB154F765E6C57873B625`
  returns ERROR_NOT_SUPPORTED50 for the legacy SMU query. Its SMN sensor still works.
- Native owner activated only afterwards. Startup clock ready at0.140s precedes
  GART0.156s, PSP0.171s, IH/GFX0.281s; both engines ready0.515s, completed0x1F.
  Initial and resulting point1000MHz/VID116,67.250C, no voltage staging needed.
- Typed SET at the same1000/820 point passes;8 paired thermal samples exactly
  match independent SMN before/after (66.750C). This is a stable-temperature check,
  not a changing-temperature or analogue settling measurement.
- New monitor11216 actually loads the common typed DLL
  `D743A306D09DE6239BD9BD17630FA5F26CA5FBD30DD524A097861E585F11560D`.
  Legacy clock task remains Disabled, monitor task Running.
- D3D shared red/blue and green pixel controls,60 presents,64MiB three residency
  cycles/four full GPU readbacks,8 shader CPU hashes and both E14 model texts pass.
  Retained desktop DLL D438EA42... and Vulkan ICD DB886B8D... identities verified.
- Final GFX2610/2610, SDMA6040/6040,617 flips,0 ACK timeouts/refusals, no TDR.
  Two pending vblank deferrals remain in this older display code (BD-008).
  Full one-shot0 consumed, guard0, native gate1 and display/execution gates retained.

## Evidence and limitations

handover/ contains scripts and streamed logs; controls/ has raw application
outputs; frozen-build/ identifies actual pre-delegation driver source. Client
sources and coordinator tests are included. Only device interface/instance
identity is redacted from public text. Generic PCI hardware IDs remain intact.
Private task backups, screenshots and firmware bytes are not included.

This proves warm PnP preparation at an already1000/820 operating point. No cold
boot, hardware upward-voltage transition, analogue settling, full power resume,
hardware concurrent-stop stress, performance or complete M9 acceptance is claimed.
Full D0 restoration is still missing: low-power closure is not a resume implementation.
Next source display/RLC changes require a distinct candidate and lab acceptance.
