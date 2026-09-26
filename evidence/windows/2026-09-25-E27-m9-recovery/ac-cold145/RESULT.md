# M458 - One dedicated AC-cold startup and content control on145

2026-09-25, unit A. Same signed145 SYS as M457:
ED7B2E0735A41048DDA1428FB4A759C32193A231D5A26A5C2FEAD5D8407903CB.
Frozen source/package identities are in M457. No driver, firmware or boot-steering
change in this trial. Persistentfullpolicy2 and nativeSMU ownership remain.

## Cold-entry boundary

Windows acknowledges full shutdown at01:55:57.958 local(+02:00). The command
uses /s, not /hybrid; captured powercfg reports hibernation/FastStartup unavailable.
Four configured SSH port probes fail between01:56:21 and01:56:45. Plug telemetry
after the request falls from about91W to10.6W. Telemetry has unknown sample age
and uncalibrated scale; it supports the shutdown observation and is not the sole
reason for power control. This is a deliberate cold-entry test, not hang recovery.

The identified LAN plug reports OFF at23:57:32.261UTC and ON at23:58:03.738UTC,
31.477seconds apart, with relay readback after each command. One off/on pair.
Windows then boots01:58:41.500 local. Configured SSH was not available at the
first return poll; later configured-/24 discovery finds one pinned-key Windows
match. The configured first address remains correct; no target config update,
extra reset or power cycle is performed. Private addresses are not archived.

## Automatic startup and confirmation

PnP reportsOK/problem0/version0.7.145.1 with the exact SYS hash. The boot log has
guard0->1 successfully flushed at31.034s, before POST/MMIO. Nativeclock preparation
at31.048s changes firmware1500MHz/VID101 to1000MHz/VID116 before GART/engine init.
No CLI RUN, PnP retry, DWM restart or explicit confirmation is issued after boot.

DWM1736 starts01:59:43; monitor4384 starts01:59:44. A bounded SYSTEM startup task
records cached software health every5seconds and performs no hardware query.
From01:59:55 through02:00:55 it records budget1/flags7, generation606089791/epoch5,
completed-primary count34->139, with increasing ready age and fresh completion.
At02:00:59.070 the monitor logs confirmation after60seconds observed progress,
completed142. At02:01:00.785 the recorder observes budget0/flags15/completed145.
The contemporaneous driver log retains checked persistence status0x00000000 at
117.752s. The recorder exits0 and is unregistered after its files are copied.

Final02:05:45: same boot/DWM/monitor, generation606089791/epoch5,flags15,
completed530,age678ms. Policy2,budget0,SDMAstartupcontrols0. Native1000MHz/VID116,
66.375C; legacy clock task remainsDisabled. Loaded DWM module and monitor control
DLL hashes match the accepted artifacts:
D3D D438EA42314A3ADD6817665A6133628A64404B5282FB82227230199DA22F768D;
control02412B52C68E4B02030BBD26E33CD948FADDE74AADD14406813B2EB46B2B0962.
The desktop still renders with CPU llvmpipe/LLVM23.1.2 and hardwareDCN flip.

## Post-cold content controls

-64MiB: three eviction/re-residency cycles, four complete GPU readbacks, all words
 match. This does not establish12GiB physical residency or force relocation.
-Eight shader cases match CPU hashes; no mismatch.
-stories15M andTinyLlama: full requested model offload, outputs exactly match
 retained E14 references after CR normalization. Each process records the actual
 radv-main-icd2 loader path and BC250 submission witness. DLL SHA
 DB886B8D53E6BEE89665287AF5EE19A1868E5874868C795F6B11472FBB4A3986 is checked.
-GFX2610/2610,paging6047/6047,zero timeouts/refusals and noTDR. Both workloads
 retain boot01:58:41/DWM1736. The Limited interactive content task is removed.
-This is correctness evidence, not a matched performance comparison.

Physical image/cursor feedback for this new cold boot has been requested but
not received at archival time. M457's owner feedback belongs to the previous
warm session. Do not silently apply it to this boot.

## Scope and next work

This is one measured AC-cold entry with automatic initialization/confirmation
and postboot content. It is not power resume, real hardware fault recovery,
preemption restoration, repeated-boot matrix, long soak or full M9 acceptance.
The attached read-only power-resume source review identifies why reusing full
StartDevice would destroy live OS-owned state. Its proposed retained-owner
resume implementation is not implemented or tested by this trial. Windows
reports no firmwareS1/S2/S3/S0ix; S4 is disabled and is a possible later positive
resume test only after the actual retained-owner path exists.

Logs are transcoded toUTF8, with device-instance/UUID lines removed where present.
Discovery endpoints/config/secrets remain private. All source scripts, bounded
recorder output, checked plug transitions and full content outputs are retained.
