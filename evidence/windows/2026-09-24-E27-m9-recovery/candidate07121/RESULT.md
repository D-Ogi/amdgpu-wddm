# M391 - Candidate121 first load passes; warm RLC failure remains

Unit A, 2026-09-24. Exact0.7.121.1 installed, SYS
14E4A60D7FB36BD59CF51F1ED5CBF6DD13018AB377265AD5F7618B11C5E23321.
Packagecheck and source evidence: ../rlc-cg-policy/RESULT.md.
Interface comment lines and PCI identities are redacted from copied logs.

## First load and stop

Linux-to-Windows graceful reboot did not return SSH; one recorded AC recovery
returned Windows boot10:21:01 local. USB loader remains bootx64.off. See
TRANSITION.md; Linux and development-host wall clocks were not synchronized.
STOP absent. Installed121 with closed gates and exact hash/version readback.
First full start and64KiB residency control passed with three cycles and GPU
readback pattern checks. Same boot throughout; no TDR in final summary.
New capture pressure witness: peak1 unfinished plan and1648 reserved bytes per
context, not a universal demand bound. Probe native0; artifacts preserved.

One stop/PnP restart with closed gates returned. Immediate CLI enumeration was
not ready (stage20, BasicDisplay only); subsequent read-only inspection observed
stage61/display-only and CLI success without another PnP or DWM action.
Persisted stop log shows both SDMA reset bits assert/release, pre/post-reset
quiescence0, reset result0/backend fault0 and retirement GFXHUB flush0. These
alone do not prove successful future firmware reload.

## One warm start

The same full-start script was invoked once. PnP enable returned, then SSH was
lost. Independent configured-address checks and subnet scan found no pinned
Windows lab. Original stream stayed locally open; after one recorded AC recovery
its local SSH process53728 was terminated (native4294967295), not mistaken for
a completed remote test. No second identical warm start was attempted.

Latest persisted warm snapshot has82lines, no wrap loss or high-IRQL drops.
Before PSP, mmRLC_CNTL=0 and mmGRBM_STATUS2=0x00000008. After command2 loading
SDMA0 firmware, STATUS2=0x01000008 (RLC_BUSY), still CNTL0. All11PSP commands
return rc0/status0. Last preserved line is entering GFX stage5 (RLC); no completed
stage5 or CP1 checkpoint. This reproduces the earlier118 location despite
corrected clock-gating policy. Exact instruction of the hang is not proved.

## Recovery and remaining work

One AC recovery after the warm failure returns boot10:30:11 local, installed121,
healthy display-only stage61 and working SSH. FullWddm gate0; diagnostic/execution
gates remained enabled from the attempted full start. Read-only snapshots later
show UnconfirmedStarts0 (initial recovery inspection saw2); no new full start.
Smart plug ON, USB loader OFF. Two AC cycles total in this turn: one for the
failed Linux-to-Windows transition and one for the warm Windows failure.

M390's policy correction is retained as source alignment, not a warm-recovery
fix. Next investigate firmware/RLC lifetime across retirement and reload using
preserved E29 live-queue reset as the positive reference. Do not repeat121 warm
unchanged. General paging-resource, cache/lifetime and performance acceptance
remain open; M9 is not complete.
