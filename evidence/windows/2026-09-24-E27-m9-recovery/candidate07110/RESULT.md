# M350 - Candidate110 first control passes; isolated RLC reset leaves busy set

Exact0.7.110.1 SYS4743B3CE3D177970BD33F30F00E01B242E00EDDCDB2C5353014E808FDA48D9D1.
All actions use Windows boot2026-09-24T05:20:01. No OS restart or AC cycle.
Installed from healthy109display-only with every execution gate and experimental
EnableRlcReloadReset closed. First full110startup keeps the experimental gate0.
Unchanged64KiB GPU probe passes3residency cycles and4full-word readbacks;
GFX4/4,paging1170/1170,zero timeouts/refusals,noTDR. Independent validator result
is control-validation.json. Native probe output and persistent logs are preserved.

After a closed-gate PnPstop, one warm full startup enables the experimental gate.
Persistent ring-20260924-034228-014.log records disabled/busy RLC before PSP:
CNTL0,STATUS2 0x01000008, both readstatus0. Reset preflight reads
ME15000000,MEC50000000,SDMA0/1 both00000001. All selected halt bits are present.
The helper returns-62 (BC250_ETIME), converted to0xC0000185. Its post-reset
snapshot still reads CNTL0/STATUS2 0x01000008 with successful read statuses.
This proves that the isolated callback did not satisfy the immediate busy-clear
postcondition in this trial. It does not prove that the reset register is locked,
that no hardware reset occurred, or that busy alone means outstanding DMA.
No independent soft-reset register readback/timing trace was captured.

Startup unwinds before PSP firmware loading: attempted0x3,completed0x1,
ready0,unwind1,quarantine0. GART firmware restore reports282writes. Device start
fails (stage91), native script exits1, but independent pinned SSH responds.
The log message entering PSP initialization precedes the helper; it must not be
read as proof that PSP loading ran. No shader/probe workload runs after refusal.

Persistent logs were collected before recovery. Closed-gate PnPrestart restores
110display-only. Independent info:stage61,47presents, same boot. Experimental
and full gates0; subsequent CLIconfirm succeeds and UnconfirmedStarts0.
No DWM restart, Windows reboot, AC cycle or manual owner recovery was required.
Control/recovery logs redact hardware identity fields; raw KMD logs unchanged.
The boot log bundle includes earlier109logs; only exact110control/warm evidence
and the supplied timestamps belong to this candidate's measurements.

The hypothesis that this isolated callback immediately clears the observed
post-retirement busy condition is not supported. Do not repeat the same trial,
bypass the busy refusal, or infer a working reset from the host model. Next
review reset-domain prerequisites and AMD's surrounding soft-reset sequence,
including its RLC stop/start and mask selection, against the measured state.
Any changed hardware sequence needs source-derived intent and its own plan.
Warm reentry, broader DMA ownership/cache/alias requirements and Windows-over-
Linux matched performance acceptance remain unresolved.
