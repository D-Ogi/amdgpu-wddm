E16 run 005 and 005c, 2026-09-21: bc250kmd 0.7.5 (commit d576e75) as the UMD stub package 0.7.5.1
================================================================================================

Package frozen before the run: package-manifest.txt (tools/packagecheck, PASS, 25 checks with --load).
bc250kmd.sys sha256 1e7bdcc24a047ab4ce1aa30ba82d86598608022c241421ac6d0e45dbb98c0e82.
One change against run 004: DXGK_QUERYSEGMENTOUT4.PagingBufferSegmentId = 0 (H9, facts M66).

Timeline (local time, unit A)
  21:54:53  install, every gate closed: display-only start, stage 61 (install-x, state-pre-gate5), confirmed.
  21:56:26  gate -Full 1 (gate-x-215626). The display-only instance stops (kept ring 195627).
            The full table starts: status OK, CM_PROB_NONE, stage 39. No teardown.
  21:56:56  ring-open5-10s: DRIVERCAPS .. QUERYSEGMENT4 ("paging buffer segment 0") .. CreateProcess,
            CreateDevice x2, CreateContext, then BuildPagingBuffer operation 11 (UpdatePageTable) and
            SetRootPageTable segment 1 offset 0, 512 entries. After that only QueryAdapterInfo type 25
            (ADAPTERPERFDATA, refused) once a second.
  21:57     the owner reports a black screen. SSH, the overlay and the desktop session stay alive
            (the overlay still reports a 1920x1200 screen; its screenshots are black).
  21:58:43  ring-open5-b, state-open5-b: unchanged. 0 TDR, 0 bugcheck, 0 live kernel reports.
  21:59     ring-summary5-*: 1028 BuildPagingBuffer calls, all operation 11, the count stable between two
            summaries 51 s apart; 0 submissions, 0 presents, no ControlInterrupt, no allocation, no
            GetRootPageTableSize. 4 objects alive (2 devices, 1 context, 1 process).
            On the target: a Default_Monitor devnode is present and OK; user32 EnumDisplayDevices from the
            SSH session lists no adapter; Win32_VideoController has no current mode.
  22:03:24  run 005c (H10): InstalledDisplayDrivers = bc250umd x3 written by hand into the software key,
            budget cleared, device reloaded with the gate open (gate-x-220348, kept ring 200350 = the first
            instance's full log including its summary and the stop).
  22:04:26  ring-open5c: the same sequence, ending after SetRootPageTable again. H10 refuted.

Temperatures 67.5 to 68.0 C. The display DDIs shared with the display-only table do not log, so this record
cannot say whether dxgkrnl called any of them; CommitVidPn was not reached (no stage 50).
