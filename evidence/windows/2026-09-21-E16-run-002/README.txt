E16 run 002, 2026-09-21, unit A, Windows boot 2026-09-21T18:28:12 (the same boot as run 001).
Files are copied from C:\BC250\e16\out and C:\BC250\kmdlog as they were written. budget-reset.txt is the one line
of the target's file that belongs to this run; eventlog-dxgkrnl-pnp.txt is a read of the event channels taken after
the run, filtered to the DxgKrnl channels and to lines naming our device. Nothing was redacted: no MAC, serial,
UUID or SSID occurs in these files.

Build: bc250kmd 0.7.3.0 (commit 82d39fb, clean worktree, bc250kmd.sys sha256 b6b942122e21152c...), plain package
(no UserModeDriverName). The table and every answer are those of 0.7.2; what is new is the trail (GuardLogKeep).

What happened, in order (local time = UTC + 2, file):

20:47  install-x-204726       0.7.3 installed over the running 0.7.2, every gate closed. Device OK, stage 61.
20:48  state-pre-gate         stage 61, presents counting: the display-only start is healthy.
20:48  confirm-x-204824       UnconfirmedStarts -> 0 BEFORE the gate is touched (run 001 spent the budget by not waiting).
20:48  gate-x-204856          EnableMmio = EnableVram = EnableFullWddm = KeepLog = 1, device disable/enable.
                              The disable stops the display-only instance, which keeps its ring:
       ring-...-184857-976    17 lines, display-only, 89 s of life, stages 10..61 then 70 79.
                              "post display ... target 2424833" = 0x250001 = our child UID (not 0xFFFFFFFF as in run 001).
                              RESULT of the enable: status Error, CM_PROB_FAILED_POST_START, stages
                              10 20 30 31 32 33 34 35 39 70 79, exactly as in run 001. This time with a record:
       ring-...-184902-205    31 lines. After stage 39 (DxgkDdiStartDevice returned success) dxgkrnl makes ONE call:
                                16  0.120 wddm: DRIVERCAPS 576 of 576 bytes: wddm 8192 sched 0x45 mm 0x60 paging node 0 flip 0x2 slots 0
                                17  0.120 wddm: QueryAdapterInfo type 1 in 0 out 576 -> 0x00000000
                                18  0.122 stage 70
                              and the summary written inside StopDevice confirms it: QueryAdapterInfo 1 call, type 1 only;
                              no other DDI of the full table was ever entered (no segment query, no GPUMMUCAPS, no
                              GetNodeMetadata, no CreateDevice/Context, no VidPN call, no ControlInterrupt), no object
                              was created, no TDR. The structure dxgkrnl offered is 576 bytes, the size this driver was
                              compiled with.
                              KeepStatus 0x00000000, 2 files: the keeper worked at both stops.
                              No display event 4101, no bugcheck, no Kernel-PnP error, no live kernel report; dwm.exe
                              is still the process of 18:29:13; D3DKMT lists BasicRender and BasicDisplay only.
20:49  budget-reset           UnconfirmedStarts 1 -> 0 by hand (the failed start counted, as designed).
20:50  gate-x-205003          gates closed, disable/enable: display-only driver back, stage 61, presents counting.
20:50  state-back             healthy 50 s later. 67.6 C.
later  eventlog-dxgkrnl-pnp   Microsoft-Windows-DxgKrnl-Admin and -Operational: enabled, 0 records. Kernel-PnP
                              Device Management 1011 "surprise removed as it was reported to be failing" at 20:17:17
                              (run 001) and 20:49:02 (this run). The 411 events (0xC00000E5) belong to the installs.
                              Windows gives no reason in any event channel.

Reading: dxgkrnl refuses the adapter on the CONTENT of the DRIVERCAPS answer (or on the table checked against it),
not on any later answer. Ruled out by this record: a refused QueryAdapterInfo type, the segment table, CreateContext,
the root page table - none of them was reached.
