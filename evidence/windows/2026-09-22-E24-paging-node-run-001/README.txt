E24 run 001, 2026-09-22 09:41:54 - 09:48: bc250kmd 0.7.24 (f14c11c) on unit A, owner at the monitor.
run-001-script.sh is the script as run. run-001-console.txt is the console up to the point where it stopped
producing output; the run was aborted from this side after the target stopped answering.

Sequence and result: gate -Full 1 -GpuVa 1 -Engines 1 -GpuSubmit 1 -Blit 1 -PagingNode 1 (full table, all gates
of stage C plus the new paging node; EnableDcnWrite and EnableVidPnFlip closed) -> started, desktop up;
gart enable -> 285 writes, STATUS_SUCCESS; psp load -> 11 commands, rc 0; ih init -> ring ENABLED, 21 writes,
MSI vector 0x70, ISR not yet called; `gfx run 8` at 09:42:48 -> NEVER RETURNED. The machine wedged completely:
the ssh session died inside that call, ping stopped answering on both the wired and the Wi-Fi address, the
monitor went black. The phase's own output file on the target (C:\BC250\e16\out\gfx-x-094248.txt) is empty:
the escape hung before the driver wrote the first line of the phase.

Recovery: the owner power-cycled the unit (09:53:43 boot). The one-shot gate meant the driver came back
display-only at stage 61; the start after the gate-close restart was refused by the UnconfirmedStarts guard
(CM_PROB_FAILED_POST_START) until the counter was cleared, then the device started normally, 70 C.

No bugcheck (BugCheck 1001 = 0 after the reboot), no TDR event (Display 4101 = 0), no live kernel report: a hard
hang, not a crash, so there is no dump and the driver's log ring of that start died with it.
