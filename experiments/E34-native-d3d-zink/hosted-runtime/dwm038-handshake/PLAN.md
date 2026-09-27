# DWM038: matched CPU/GPU redirection handshake

Hypothesis: confirmed hosted GPU DWM offers the 641x479 window a GPU GDI
redirection surface where the matched CPU control M681 returned 0x00263008.
A refusal or the same response is a valid observation, not a Present success.

Use exact KMD164, UMD5C74BF98, hosted ICD3508416F and probe0DC75F8C from
source c0edf93. Keep both GPU binaries unchanged; rebuild the path-bound router and the unchanged
composition-control source. Preserve the M679 stationary
CPU baseline and frozen GPU pixel checks, ETW, fences, map audits and rollback.
After normal health confirmation, start a separate interactive task with the
same no-paint/GDI-paint cases and arguments as M681. Each probe is strictly
handshake-only/no-open: no source, device, context, resource open or Present.
Require the original GPU DWM PID/start/module identity before and after each.

The observation task has a70-second monitored deadline and75-second task cap.
Its two children retain45-second individual deadlines; exceeding the pair limit
aborts instead of extending the desktop trial. Stop the task and path-identified
children before rollback, including from the independent300-second watchdog.
The GPU animation interval is bounded to160 seconds, leaving approximately20 seconds of the
three-minute window for final captures and restoration. STOP and85 C limits remain active. No baseline promotion.

Both cases must produce clean terminal receipts. Interpret exact HRESULT,
handle kind, format, update id and ETW allocation events; do not preselect a
successful HRESULT. On0x263005, retain the handle classification for later
Present review. All other results retain their actual meaning. Even0x263005
proves neither GPU rendering by the probe nor successful windowed Present.
An exact selected-pixel image is not a complete no-CPU-frame-copy proof.

Results: not run. Compare to M681 and record the result with immutable evidence.

Preparation validation: router and composition control compile with /W4 /WX;
PS5.1 parses all scripts; the handshake transcript controls pass. Remote stage
hashes, PS5 parsing, rollback163 and the no-marker restore no-op pass at
2026-09-27T15:30:42.8057249Z. Probe hash remains0DC75F8C.
