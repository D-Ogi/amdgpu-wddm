# M359 - Candidate113 integrates GTT retirement before RLC stop

Candidate0.7.113.1 SYS17AED863C702EDA8367616663F6953455B9B67E3479E3453A3F2EB01EF47C1D7.
Built, not deployed. Latest hardware remains M356candidate112display-only.

GfxPrepareStop (shared by PnPstop and startup unwind) uses
HaltForMappingRetirement after completed CPstage. It drains/halts SDMA, unmaps
queues/dequeuesKIQ/haltsCP while retainingRLC, closes selfring and verifies
existing engine halt/undo/sequence conditions. Only then it invokes M358's
owner-scoped GTTunbind and both-hub flush, with backing and VRAMCSB retained.
It explicitly stops RLC afterwards, including after an unbind/flush refusal;
a faulted backend still suppresses writes. The final quiet verdict includes
mapping retirement result and sequence status. PSPunload/GARThardware-disable/
GFXstorage-destruction/GARTrestore follow unchanged. Later mem_free skips
already retired translations while historical Bound still protects release.

DiagnosticFini and pre-CP partial startup keep their existing HaltEngines path.
This does not certify those historical paths for every partial firmware state.
No allocation owner is detached in the new phase, no TLBflush is skipped before
mapping retirement commits, and no busy-bit shortcut/reset is introduced.
The KMD relies on its explicit VRAMCSB placement, not a universal AMDallocation
invariant. Undocumented RLC accesses remain a hardware acceptance boundary.

Validation:
-3632 actual-source stop checks pass, including12new phase/ownership checks.
 Existing historical negative control:3340checks,1768expected failures.
-327startup-coordinator checks and38actual-source GTTretirement checks pass.
-WDKbuild and25package checks pass,0errors/warnings.
-M357 already validated shared shim keep-RLC/explicit-stop behavior in replay;
 no further shim algorithm changed in this integration.
The first harness build needed conditional inclusion of new tests when compiling
historical source; no production sequence was changed to satisfy that control.

Next hardware acceptance: exact113closed-gate install, one clean first-start
control because existing112boot already contains stopped/busy retirement,
unchanged64KiBcontent probe, stop with pre-unbind/flush/RLCstop witnesses, then
one same-boot full restart with experimental reset gate0. If full startup works,
require a new output-directory content probe; startup alone is insufficient.
If rejected/hung, preserve persistent logs before recovery. No unchanged retry.
A successful trial would still require repeated/workload acceptance and would
not close general DMA/cache/alias/resource readiness or performance targets.
