# M319:64KiB passes;1GiB probe deadline reached

Installed07102 unchanged, sameboot19:27:05, noGPUrestart.64KiB positive control passes all three residency cycles and four full GPU readbacks; independent M257 validator passes.1GiB nativeexit4: its own300000ms watchdog terminates at observed fence2457. Two complete1GiB byte-oracle readbacks pass (fences1024/2048), cycle1PASS; cycle2 restoration succeeds before partial readback. Residency departures3, bulk restores6343/6297ms; no reported byte mismatch, but the full1GiB test is FAILED/INCOMPLETE, not accepted.

Host session6389 terminal1. Final deviceOK, SSH usable, sameboot,1000MHz/VID116,71.9C; no probe remains. Driver node0 2495/2495, paging191078/191078, zero timeouts/refusals/noTDR. These observations distinguish a probe wall-clock limit from a reported KMD timeout; they do not prove why execution was slower.

Harness difference from M257: native unbuffered stdout (_IONBF at gpu-residency-probe.c:113) was captured through Start-Process -RedirectStandardOutput, rather than CMD native file redirection. Additional pipe/output overhead is a HYPOTHESIS, not measured causation. Prepared ASCII native-redirection harness restores the M257 transport and retains all original probe/kernel timeouts. It monitors the launcher process and terminates that scoped process tree on temperature failure. No repeat has been executed yet. Future comparison must use new output directories and pass the64KiB control first.

Immutable native results decoded UTF16 where needed; no result edits. See small-validation.json and large-validation.json. No performance or full M9 completion claim.
