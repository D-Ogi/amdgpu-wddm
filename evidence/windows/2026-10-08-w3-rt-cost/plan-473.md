# Trial 473: Witcher 3 HIGH, W3 RT effect-cost series arm E (RT GI + AO): preset w3rt-ao

`ARM_LABEL="E (RT GI + AO)" bash scratch/w3-rt-cost/run-arm.sh 473 w3rt-ao` - method, lab state, conjecture C74, kill and
safety rules as plan-469.md (same location, drive-still input, window B 90 s GPU only, 1920x1080 exclusive fullscreen,
FXAA, no upscaler/FG/DRS, VSync off, LimitFPS 240, direct start, no experiment, overlay summary poll paused).
Expected closure: recovery-unverified (Verify "Confirmed CPU baseline required": the game's 1920x1080 mode commit moves
the KMD epoch, as 466/469); the runner re-confirms the start and checks the stack identities by hand.
