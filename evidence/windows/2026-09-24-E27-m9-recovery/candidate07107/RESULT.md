# M343 - Busy transition precedes PSP unloading

107/SYS236328FC5337185DE2D9CFA69189ABE4795B3629ECB82A5438F4636FEF039035 adds read-only boundary snapshots. WDK build,319coordinator checks and package25checks pass. Closed-gate install succeeds. Windows normal shutdown accepted,30s delay and SSH unavailable before one verifiedACoff8s/on. PinnedSSH returned in boot04:30:34. First requested full start was refused by UnconfirmedStarts2/stage90 BEFORE hardware initialization; hostsession72598 terminal1. This is a preparation error, not a GPU hang. Closed-gate device recovery and explicit guard confirmation permit one actual full initialization; session90582 terminal0 with scheduler-read/write/done. No secondACcycle.

Initial RLC observations match M342: beforePSPCNTL0/after1,STATUS2 0x8 (busyclear). Unchanged64KiB GPU probe passes native0/fourfullwordreads/threecycles. Close execution gates and PnPrestart into display-only, terminal0. Retained stop snapshot ring-20260924-023412-388.log shows:

- after-stop: CNTL0,STATUS2 0x00000008.
- after-gfx-memory-release: CNTL0,STATUS2 0x01000008.
- before-psp-unload,after-tmr-destroy,after-psp-ring-stop,after-gart-stop: same0/0x01000008.

Every MMIOread status0. Thus the observed transition is BEFORE PSP retirement and GART firmware restore. The interval includes EnginesHalted, TearDown (queue/fence/CSB teardown and unbinding), quiet bookkeeping and GpuMemRelease, plus elapsed execution time. The snapshot name does not isolate GpuMemRelease itself as causal. This is not proof of a specific freed allocation, outstanding access or eventual reset fix. Next inspect/narrow that interval, especially resource teardown and TLB flush, instead of assuming PSP unload first sets busy.

PSPdestroyTMR/ringstop return0; GART restores282writes; finalstop reaches79. No fullwarm reentry attempted. Current107healthy display-only/all execution gates closed, boot04:30:34. Original clean-baselineAC is the only OS restart in this experiment. General M9 acceptance remains open.
