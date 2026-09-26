# M347 - PnP and startup unwind retire hardware before GFX storage

Candidate0.7.109.1, SYS551B0BE13C4DA83AA8B9C3BA283F9320969607D9D541D3A6182E275CB145A019,
prepared locally, NOT deployed.

Both Bc250StopDevice and startup failure Unwind now execute:
IH stop -> GfxPrepareStop -> PspStop -> GartPrepareStop -> GfxStop -> GartStop.
GfxPrepareStop halts engines but retains its owner, ring storage and GpuMem.
GartPrepareStop invokes the existing imported bc250_gmc_gart_disable only after
GFX/IH/PSP retirement verdicts. It retains the adev/table/dummy page and keeps
Enabled set so final firmware restoration remains owed. GfxStop then destroys
its storage with the GART owner still present; GartStop restores firmware state
and frees the final owner/dummy page. Original unbind/TLB invalidations remain.
Manual diagnostic Fini still composes its original halt/storage helpers.

Per-generation preparation flags prevent a second hardware-stop attempt.
Unconfirmed consumer/translation retirement retains GFX/GART owners and backing;
GFX destruction cannot run first. Failed final restore latches GartStopQuiet
false so repeated stop cannot retry destructive restoration. Existing PnP/startup
quarantine prevents restarting the same device object after an unconfirmed stop.
Full device removal/recreation retention remains a wider acceptance gate.

Actual-source stop matrix now includes a GART-disable failure dimension:
3620checks pass; historical pre-retirement negative control fails1768/3340 as
expected.319coordinator checks verify updated unwind ordering. WDK26100 build
and25package checks pass. These tests mock MMIO and memory release and do not
prove hardware quiescence, posted-write completion or RLC recovery.

Next hardware procedure: preserve108 state, install exact109 with gates closed,
obtain one clean first-start baseline, run the unchanged64KiB GPU residency/word
oracle, stop into display-only and inspect after-stop, PSP, GART-disable and each
unbind/hub-flush snapshot. Preserve logs before one separately recorded warm
start attempt, contingent on successful stop and no quarantine. Keep independent
SSH/temperature/clock/STOP checks and local plug telemetry. If warm start fails,
recover once using authorized AC and inspect persisted phase evidence. Do not
claim that disabled VM contexts or a cleared busy bit alone solve reentry.

No hardware operation, OS switch or AC cycle occurred while implementing M347.
Last observed lab remains M344108 display-only, boot04:46:20. That state was not
re-polled in this source/build turn. Full M9 remains incomplete.
