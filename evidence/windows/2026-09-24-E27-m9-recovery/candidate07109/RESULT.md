# M348 -109 first control succeeds; phase reordering does not fix warm reentry

Exact candidate0.7.109.1/SYS551B0BE13C4DA83AA8B9C3BA283F9320969607D9D541D3A6182E275CB145A019.
See RESULT-1/2 for the preserved intermediate chronology and M347 for source/build.
First full start in boot05:12:37 and unchanged64KiB probe pass:3residency cycles,
four full GPU word readbacks, graphics4/4,paging1095/1095,no timeouts/refusals/TDR.

The changed stop order executes as intended. RLC_CNTL0/GRBM_STATUS2 0x8 stays
through GFXhalt, PSP TMR/ring retirement and successful GART hardware disable.
PSPunloadrc0; GARTdisableNTSTATUS0. The first GTT unbind still leaves0/0x8;
the first GFXHUB TLB flush changes STATUS2 to0x01000008. It persists through
later cleanup and firmware restore282writes. Every recorded RLC readstatus0,
no TLBfailure/retention message; stage79. Source-based phase separation alone
therefore does not remove the post-flush busy observation.

One full warm startup in the same boot loses SSH after PnPenable. Pinned-subnet
scan finds no lab, independent strict hostname query times out. No test retry.
Original session9232/sshPID18004 remains live until verified recovery AC and
then targeted local cleanup; its transport exit4294967295 is not remote completion.

Recovered immutable ring-20260924-031711-334.log: before and afterPSP both
CNTL0/STATUS2 0x01000008, allreadstatus0. All11PSPcommands returnrc0/status0.
Last persisted checkpoint is CP1scheduler-read at0.304s; no scheduler-write.
This does not distinguish checkpoint persistence from the following MMIO read,
and does not prove a faulting instruction. Warm reentry is still unresolved.

One ACcycle for initial control and one for warm-failure recovery, each normal
8s off interval with relay readback. Recovery boot2026-09-24T05:20:01. Persistent
logs acquired before changes. Then closed all execution gates, cleared guard
counter and restarted only the PnP device into display-only. Final independent
info:109,stage61,118presents,allMMIO/GART/PSP/GFX/IHgatesclosed,UnconfirmedStarts0,
EnableFullWddm0. No extra OS restart or DWM reset. Pinned SSH works; plugON.

Local power samples include110.3W during first boot (tool observation),91.2W
in recorded boot sample,79.2W during stalled warm attempt and116.1W during
recovery boot. Raw DPS/queryUTC are retained where captured. Scale follows
TinyTuya convention; exact-model calibration/sample age unverified. No power
value is used as a liveness, GPU-completion or reset-necessity proof.

Next: separately prepare a source-derived isolated RLC recovery trial with
halted-engine preconditions and clean first-start control. AMD's reset callback
and soft-reset branch are reference code, not a proven remedy on this unit.
Do not repeat109unchanged or drop required TLB invalidations. General DMA
resource/cache/alias/lifetime and matched performance acceptance remain open.

Control logs redact device identities; raw KMD/probe files are unchanged.
