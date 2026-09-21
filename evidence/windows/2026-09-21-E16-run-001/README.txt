E16 run 001, 2026-09-21, unit A, Windows boot 2026-09-21T18:28:12 (one boot for the whole run).
Files are copied from C:\BC250\e16\out as they were written (the *.log files are UTF-16, PowerShell redirection).
Nothing was redacted: the only matches of the redaction patterns are driver versions and two public class GUIDs.

Builds: bc250kmd 0.7.1.0 (commit b491cf0) for step 1, bc250kmd 0.7.2.0 (commit e971351) for the gate-open run,
both from a clean worktree, plain package (no UserModeDriverName).

What happened, in order (local time, file):

20:07  install-x-200754     0.7.1 installed, every gate closed. Device OK, stage 61, presents counting.      (H1 holds)
20:08  sweep-*-before       witness sweeps GC, MMHUB, MP0, NBIO, OSSSYS, exit 0 each.
20:09  ring-installed       first read of the driver's log ring on the lab: 15 lines, start-up order, and
                            "post display 1920x1200 pitch 7680 format 22 target 4294967295".
                            That number is D3DDDI_ID_UNINITIALIZED, and stage A of 0.7.1 would have put it into every
                            CRTC_VSYNC report and compared it in GetScanLine. Fixed in 0.7.2 before the gate was opened.
20:12  state-after-install  the overlay confirmed the start by itself (unconfirmed 0), presents 306.
20:16  install-x-201610     0.7.2 installed, gates closed, stage 61. unconfirmed 1 (the monitor had not confirmed yet).
20:16  ring-closed072       same start-up order, same target 4294967295 (second driver load, same value).
20:17  gate-x-201709        EnableMmio = EnableVram = EnableFullWddm = 1, device disable/enable.
                            RESULT: device status Error, CM_PROB_FAILED_POST_START. Stage history
                            10 20 30 31 32 33 34 35 39 70 79: DriverEntry and DxgkDdiStartDevice completed (39), no
                            first CommitVidPn (50) and no first present (60, 61), then DxgkDdiStopDevice (70, 79).
                            unconfirmed 2. No display event 4101, no bugcheck, no Kernel-PnP error, no live kernel
                            report, dwm.exe still the process started at 18:29:13. D3DKMT lists BasicRender and
                            BasicDisplay only: Basic Display took the desktop.
20:17  ring-open10s         bc250kmd_cli finds no adapter: the driver had been unloaded, and its log ring with it.
                            This run therefore has NO record of which DDIs dxgkrnl called between the start and the stop.
20:19  diag-201900          Windows' own record: problem code 0x2B, problem status 0x00000000, service STOPPED.
                            The two Kernel-PnP 411 events (0xC00000E5) belong to the two installs at 20:07 and 20:16,
                            not to the gate-open start; both installs ended with the device running.
20:19  budget-reset         UnconfirmedStarts 2 -> 0 by hand, through the registry (the documented way back when no
                            adapter of ours answers). Whether a third start would have been refused was not tested.
20:20  gate-x-202012        gate closed, device disable/enable: display-only driver back, stage 61, presents counting.
20:21  state-back           still healthy 20 s later, presents 81. 67.5 C. Before the run 67.8 C, during 68.0 C.   (H6 holds for a failed start)

Screenshots through bc250mon (not in the repository): scratch\e16\screens\step1.jpg, run1-open-a/b.jpg, run1-back.jpg.
The overlay answered in every one of them, including while Basic Display owned the screen.

Not reached in this run: H2 (refuted as far as "starts without a problem code" goes), H3, H4 (no TDR and no
bugcheck is true, but for a driver that never ran), H5, run 2, the sweeps `after`.
