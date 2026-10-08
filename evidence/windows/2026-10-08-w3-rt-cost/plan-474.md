# Trial 474: Witcher 3 HIGH, W3 RT effect-cost series: in-game sweep of RT settings in one session

Owner, 2026-10-08 (through the coordinator): the restart-per-arm series takes too long; change the settings in the game
and measure about a minute per setting. Arms A-E ran as 469-473 (one launch each); this session measures the rest.

`ARM_DRIVE=none ARM_BOUND=1200 ARM_SAMPLES=900 PLUG_SAMPLES=75 ARM_ETW="-Seconds 40 -LatestB 500 -FpsSeconds 600"
ARM_LABEL="in-game sweep" bash scratch/w3-rt-cost/run-arm.sh 474 w3rt-all` - lab state, start, closure and safety rules as
plan-469.md (direct start, no experiment, 1920x1080 exclusive fullscreen, FXAA, no upscaler/FG/DRS, VSync off, LimitFPS 240,
overlay summary poll paused, 40 CU). No drive-still: the operator drives the session with gco.sh (no Jev).

Windows (one window B of 600 s GPU only from the first look; each measurement window 50 s, still camera, marked by
`note:measure X start|end` in UTC and cut by etw-present-windows.py):
1. Same spot as 469-473: menu Continue, 36 s, look:40:0:10:50 (starts window B), settle, measure A-ctl (all four RT
   effects: GI Performance, reflections, shadows Performance, AO) = in-session control against 469 (12.8/s, 8.79/s per GHz).
2. Menu (Esc, SETTINGS, VIDEO, GRAPHICS, each level checked by OCR): RT AO Off, back, settle 8 s, measure noAO
   (= the high-rt preset, compare 462).
3. Menu: Ray Tracing Off, back, measure rt-off (the cost of RT GI itself against B 470).
4. Menu: Ray Tracing On, the four effects as A, RT GI Quality, back, measure gi-q.
5. Menu: RT GI Performance (A again), back, measure A-ctl2 (drift and thermal bracket), then quit.
At the end all four RT effects are ON (owner: RT stays on); the series later restores the lab settings from the
pre-series backup.

- Question: the cost of RT GI itself, of RT GI Quality against Performance, and how an in-game change compares with a
  fresh launch of the same settings (A-ctl against 469).
- Conjecture C75: A-ctl is within 5 % of 469 per GHz; rt-off is faster than B (470); every window holds with no GPU
  fault, TDR, removed device or runner thermal stop, and every menu change takes effect without a restart of the game.
- Kill: any of those faults, a settings change that the OCR does not confirm, or no world within the bound.
- Safety: Tctl >= 87 C held 10 s or >= 89 C (runner stop), plug watts sampled, bound 1200 s (owner, interactive game
  session).
Expected closure: recovery-unverified (Verify "Confirmed CPU baseline required" after the game's 1920x1080 mode commit,
as 466/469); the runner re-confirms the start and checks the stack identities by hand.

## Result
2026-10-08 00:50:38Z Start Running, world 00:51:48Z, window B 00:51:59-01:02:00Z lab clock (600.4 s GPU only; it opens
at the world arrival, not at the first look), quit 01:03:49Z (development PC clock). Four windows of 50 s, marks
"mark measure X start|end" in the game log:
A-ctl 13.2/s at 1500 MHz (8.80 per GHz; 469: 8.79), noAO 12.5/s at 1254 MHz (9.97), rt-off 44.0/s at 1104 MHz
(39.86), gi-q 10.0/s at 1218 MHz (8.21). A-ctl2 not measured: window B ended before a fifth window fitted. Every menu
change took effect in the game without a restart; RT left all four ON with GI Performance (dx12user.settings.after).
Operator events: the owner pressed Space three times at the intro trailers (before the menu); one Esc did not open the
pause menu and the next opened LOAD GAME, left with Esc (no load); one Down was lost on the way to VIDEO (GAMEPLAY page
opened, no change). Closure as expected: recovery-unverified (Postflight), start re-confirmed (flags 15, epoch 149),
identities unchanged, marker absent. C75 holds: A-ctl within 0.1 % of 469 per GHz, rt-off far above B, no fault line.
