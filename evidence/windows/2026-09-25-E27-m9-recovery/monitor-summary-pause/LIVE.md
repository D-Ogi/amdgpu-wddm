# M464 live diagnostic-polling isolation control

2026-09-25, unit A on unchanged KMD146 recovery boot03:27:10.500+02:00.
Monitor replaced03:34:34-38 only; DWM1548 and Windows boot retained.
New monitorPID6048 EXE SHA256
E75DDCC8AADFED045BC4D453054917922976E07698F0F12486FF21ED1EA2D8AE.
Installed controlDLL intentionally remains02412B52C68E4B02030BBD26E33CD948FADDE74AADD14406813B2EB46B2B0962,
not the newer bundledDLL mentioned by the source-only agent report.
Backup retained under lab mon/summary-pause-deploy/before.

Creating graphics-summary.pause suppresses only the synchronized LOG_SUMMARY
poll. A12-second observation after draining the current call shows the last
summary sequence stays8177, while cached health completed rises800->815.
Graphics panel explicitly marks the cached snapshot paused. Marker remains on.
Host14 boundary checks and existing build tests pass. No driver or DWM restart.

This validates isolation, not a cure for S4 input stutter. Normal cold desktop
was already smooth with the poller enabled; a post-S4 comparison remains needed.
User suggests UDP broadcast telemetry. UDP is a possible monitor transport but
does not eliminate KMD's synchronized acquisition; a future cached RAM snapshot
would avoid that acquisition cost. No UDP sender implemented or started here.
