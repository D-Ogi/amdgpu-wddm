E22 run 001, 2026-09-22 ~04:35: bc250kmd 0.7.19 (clean worktree build of 66b8055, package-umd, .sys sha256 prefix
4c8cc2f32aaeb0a1) in display-only mode on unit A, Windows boot of 04:04:55 (the return from the Linux session E21).
run-001-script.ps1 opened EnableMmio (read-only BAR5 mapping, EnableMmioWrite 0), restarted the device with pnputil,
ran `bc250kmd_cli dcn` twice one second apart and closed EnableMmio again. run-001-console.txt is the unedited
output. Owner absent; the picture on the monitor was not observed for this run (the driver's own info escape reports
stage 61, mode 1920x1200, presents continuing).
