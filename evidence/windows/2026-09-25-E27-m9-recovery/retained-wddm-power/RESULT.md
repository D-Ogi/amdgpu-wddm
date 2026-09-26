# M460 - Retained WDDM software suspend/resume boundary

2026-09-25. Source/host work, not wired to SetPowerState and not deployed.
The accepted lab baseline remains M458 KMD145 with owner-confirmed output.

WddmSuspendRetained joins previously queued notifications, then under the owner
lock checks execution/paging queues, active submissions, completion/preemption
reports and fault/refusal state. It refuses outstanding work without changing
accepted fences. When idle, the existing Stopping admission flag closes private
work, DCN vsync is disabled with checked status, timers are canceled and queued
private DPCs removed/joined. RetainedPowerPause distinguishes this reversible
closure from ordinary stop. Device->Wddm stays published; no object, aperture,
capture owner, allocation or fence history is destroyed.

WddmResumeRetained requires a retained pause, empty work, both engine readiness
checks, source-hidden and cached hardware-blank state. Requested vsync notifications
are restored with checked hardware status (or the software timer). Admission opens
without resetting OS fence history or making the source visible.

This is the software half only. A future PASSIVE/Level Three coordinator must
close diagnostic admission, stop/join IH and hardware consumers, retain private
backing/firmware, restore translation/engines/display, refresh cached blank and
scanout state, handle OS table restoration and reopen health with a fresh epoch.
The readiness/cache checks do not prove those hardware steps occurred. Do not
wire these functions to SetPowerState until that positive path exists. Existing
SetPowerState remains incomplete; no resume support is claimed by this change.

Actual-source host controls:191 checks,0 failures. The extracted production
functions run against ordered kernel mocks and independent retained identity/fence
sentinels. Controls cover both engines' pending states, both nodes' report/fault
states, joined DPC/timer ordering, repeated pause, failed hardware notification
change, black-source requirements, missing readiness, software/hardware notification
restoration and unchanged retained identities/history. No real power loss or
Windows scheduler execution is simulated by these mocks.

Mutation omitting admission closure:188 checks,19 failures. Mutation resetting
LastCompletedFence during suspend:191 checks,3 failures. Raw outputs retained.
Full WDK build/sign passes, undeployed SYS SHA256:
A01197479DA0A586F4674AAF8AFF315EA02F1352E73B4F1EE338E941785BA937.
This development build still carries145 version; installed SYS remains ED7B2E07.
Build includes M459 metadata prerequisite; use hashes, not version alone.

Local Microsoft references: conceptual staging110f60ea, display/threading-and-
synchronization-third-level.md (GPU idle/evicted; QueryAdapterInfo exception),
and plug-and-play--pnp--start-and-stop-cases.md (black, invisible source at D0).
DDI snapshot7515063: dispmprt/DxgkDdiSetPowerState, PASSIVE and restore obligations.
Full source review: ../ac-cold145/power-resume-source-review.md.

Reproduce: driver/kmd/test/run_retained_power.ps1; use -Source for each negative
source. KMD build uses driver/kmd/build.ps1 with local NuGet kits, output
scratch/build/m460-retained-power. No lab calls or power transitions in this task.
