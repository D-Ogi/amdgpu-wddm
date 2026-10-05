# M362 - Warm startup first observes RLC busy after GFXHUB invalidation request

Unit A, exact0.7.114.1 SYS82D1C6640D512A33FC4037549AD1258DB0A926A8C5A6A9C6A2711455D658414F.
First control used recovered boot06:29:13, verified full/reset gates0 and no full-start log markers. Installation did not reboot. Reset gate remained0 throughout. No routine AC baseline.
First full start passes. BeforeGART, both hub enables, fault defaults, MMHUBflush and GFXHUBrequest/read/ACK all show RLC CNTL0/STATUS2 8. Unchanged64KiB probe passes three residency cycles and four full-range readbacks; GFX4/4,paging1242/1242,no errors/TDR. See control-validation.json and gpu-probe raw artifacts.

Stop snapshot ring-20260924-044233-352.log repeats the M360 result: CP/SDMA halted, RLC CNTL1/STATUS2 8 through retirement of25ownerGTT mappings and both-hub flush. Explicit RLCstop,PSP teardown,GARTdisable,storagecleanup andfinalrestore keepCNTL0/STATUS2 8. Immediate post-PnP CLI enumeration returns1 despite PnP statusOK; this transient is not claimed as independent display-only acceptance. Subsequent warm script independently verifies healthy adapter/exact image and successful CLIconfirm before enabling full mode.

One warm full start enable06:43:11 losesSSH. Pinned subnet scan finds0lab matches. Persisted ring-20260924-044312-067.log records:
- preGART CNTL0/STATUS2 8;
- afterGFXHUBenable,MMHUBenable,faultdefaults andMMHUBflush:0/8;
- first snapshot afterGFXHUBrequest00F80001:0/01000008;
- existing dummyread00F80001,ACK1,flushrc0/sequencefault0,still0/01000008;
- all11PSPcommands rc0/status0, afterPSP busy unchanged;
- last persisted checkpoint CP1 scheduler-read at0.319s.
This bounds the first changed observation to the GFXHUBrequest interval including observation latency; it does not prove a particular MMIO instruction directly caused the bit or later hang. No startup invalidation has been skipped. Raw snapshots unchanged.

One failure-recovery AC OFF8s/ON restored Windows boot06:44:52. DHCP rediscovery verified the pinned host and updated private config. Original native SSH PID13332 retained until verified AC, then targeted transport cleanup; local exit4294967295 is not remote script completion. Persistent snapshots collected before closed-gate PnP recovery. Final independent CLIinfo succeeds:114stage61/53presents,confirm0,all execution gates0/reset0,UnconfirmedStarts0,SSH healthy. No additional OS/DWM restart or manual action. Host logs redact PCI identity and hardware/interface lines; telemetry is auxiliary with unknown sample age/calibration.

Next: review the dependency boundary for delaying the initial GFXHUB invalidation until RLC can service it, or preserving a valid RLC context through reentry. Either solution must retain translation correctness and resource lifetime, preserve the cold positive control, and pass warm content tests. PSP VRAM addresses alone do not justify reordering. General M9 DMA/cache/alias/lifetime/performance and warm acceptance remain incomplete.
