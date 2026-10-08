# Trial 472: Witcher 3 HIGH, W3 RT effect-cost series arm D (RT GI + shadows): preset w3rt-shadow

`ARM_LABEL="D (RT GI + shadows)" bash scratch/w3-rt-cost/run-arm.sh 472 w3rt-shadow` - method, lab state, conjecture C74, kill and
safety rules as plan-469.md (same location, drive-still input, window B 90 s GPU only, 1920x1080 exclusive fullscreen,
FXAA, no upscaler/FG/DRS, VSync off, LimitFPS 240, direct start, no experiment, overlay summary poll paused).
Expected closure: recovery-unverified (Verify "Confirmed CPU baseline required": the game's 1920x1080 mode commit moves
the KMD epoch, as 466/469); the runner re-confirms the start and checks the stack identities by hand.
