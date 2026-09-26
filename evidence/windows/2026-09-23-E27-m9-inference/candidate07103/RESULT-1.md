# M325 - Candidate07103 install succeeds; full start unresolved

2026-09-23, unit A. SYS B59A583015BBAD8498A1B4D29844191D543EDE2E68B59F91C8128D94BB6E2742.
Host routing329153checks, WDKbuild and full package25checks pass. First fixture
compile lacked the MAXULONG test definition; fixed test model only. Bounded
first-four per-context reserved/heap capture logs and saturating operation counts
provide a runtime path witness. No witness was acquired from this full start yet.

Before deployment07102/deviceOK/fulltable, boot19:27:05,1000MHz/VID116,70.9C,
no known probes active; STOP clear and overlay notified. Installation with all
hardware gates closed completes, oem77.inf/0.7.103.1, exact installed SYS hash,
deviceOK, sameboot. Host session57405 terminal exit0. Read-only retirement
acquisition terminal0. Pulled previous-driver-stop.log SHA256
43744DC1C9F3CDED9BCDBAFC567ADE6F03A9A8E2F703CE825ABBB9EFCFA9D500.
Old driver graphics16960/16960,paging610238/610238,0timeouts/refusals/noTDR;
stop stage70->79, IH_RB_CNTL0x403101A0, CP_ME_CNTL0x15000000,
CP_MEC_CNTL0x50000000, SDMA0/1_F32_CNTL0x1/0x1, PSP destroyTMR0/ringstop0,
GART restored282writes, no leaked-page diagnostic. This records stop observations;
it is not independent proof of all hardware retirement/cache ownership.

Fresh STOP clear and clock/temperature preflight precede ONE full start.
PnPdisable21:11:31 and PnPenable21:11:35 report success. Subsequent status query
has not returned. All three configured endpoints failTCP22 with3second probes.
Host SSH stream session21191 remains LIVE at observation, process58292.
Do not restart based on observation timeout; re-poll samehandle and/or inspect
original scratch/m9/candidate07103-start.log. No Windows reboot was requested.
User monitor/mouse question pending, explicitly no reset yet. Full start is NOT
accepted; exact failure stage and loaded full-table state are unknown until
persisted logs are recovered. Do not infer it is the old CP1 failure.
