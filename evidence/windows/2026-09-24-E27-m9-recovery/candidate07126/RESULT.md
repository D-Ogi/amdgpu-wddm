# M402 - GRBM_STATUS does not distinguish successful and failed reload

Unit A,2026-09-24. Exact0.7.126.1 installed in boot11:30:53 without baseline reset.
SYS EA573E60DABE3B295D0F9A226A11E0AA07DFD346608F29B371EEC09F3FB30CF5.
STOP absent, clock/temperature preflights pass. First full start completes all
visibility checkpoints;64KiB3cycle eviction/restoration GPU readback passes.
Outputs preserved before stop. Stop confirms quiescence, GFXHUB retirement,
GARTdisable and retained-disabled final policy, then display-onlystage61/CLI0.

All captured GRBM_STATUS reads in first-load, stop and the latest warm snapshot
return0x00003028 with readstatus0. Original AMD GC10.1 header decodes this as
ME0PIPE0_CMDFIFO_AVAIL8,RSMU_RQ_PENDING1,DB_CLEAN1,CB_CLEAN1. None of the CP/GFX
busy bits used by gfx_v10_0_soft_reset is set in these samples. RSMU_RQ_PENDING
is also set in the working control; it alone does not diagnose a handshake
failure. Sampled quiet flags are not proof of internal hardware health.
Raw logs, offline decoder, original MIT AMD header and its hash are retained.
Repeated CLI printouts/snapshots are not independent observations.

One warm start at11:42:51 losesSSH. All11PSPcommands complete; RLCbusy again
first appears afterSDMA0command2 while GRBM_STATUS stays unchanged. Latest
ring-20260924-094254-024.log ends with after-request observer checkpoint0.330s;
no after-dummy-read checkpoint. RLC resume returned before that interval.
Exact stalled instruction remains unproved; required invalidation is retained.

Pinned subnet discovery findszero labmatches; bothOSconfiguredendpointsets
unavailable. One recordedACcycle restores boot11:44:14,126display-onlystage61,
CLI0,FullWddm0,count2,otherexecutiongatesasrecorded. OldSSH69068terminated only
afterAC, native4294967295transporttermination. All tool sessions terminal.
USBloaderOFF,plugON. Recoverytelemetry109.7W/0.739A/241.9V,rawDPSretained;
ageunknown/scalesuncalibrated/notOSproof. No post-recovery reset.
Interface and PCI instance identities redacted from copied logs.

The data does not justify broader CP/GFX reset masks. A separate source-backed
hypothesis remains: KMD RunSetup forces pp_gfxoff=true solely to reproduce the
initial Linux trace. The existing imported gfx_v10_0_rlc_start false-policy
branch suppresses RLC-SMU messages via gfx_v10_0_rlc_smu_handshake_cntl. This is
not the unsupported generic SMU GFXOFF command discussed in M354. Next review
and explicitly model that supported policy branch before a changed hardware
trial. Do not call RSMU_RQ_PENDING or the policy choice a proven cause.
Full M9 and warm recovery remain open; do not repeat126unchanged.
