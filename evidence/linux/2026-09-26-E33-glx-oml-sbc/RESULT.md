# Linux GLX OML SBC reference gap (M528)

Same M526 reference, no configuration change or reset. Full015 ends66cases:
57pass/8skip/1fail. Earlier texture-from-pixmap now passes in the full runner.
Failure: glx@glx_arb_sync_control@swapbuffersmsc-divisor-zero reports SBC208
instead of1. The test initializes output variables to0xd0 (208); this is an
unchanged sentinel, not evidence of208 actual swaps. Identical executable and
arguments (-auto -fbo) on RadeonSI pixel/test-pass exit0. Both report the
unsupported -fbo argument, so it does not distinguish this failure.

Source inspection: glXWaitForSbcOML calls the screen waitForSBC callback when
present, otherwise returns False without writing outputs. DRI3 screen supplies
that callback; the software/Kopper screen initialization does not in the
inspected paths. This is a source-based explanation, not a measured callback
pointer or completed implementation. Do not report synchronization as correct.

Post-failure unchanged swapbuffers control passes on Zink. Retain the OML
failure alongside prior FBConfig warning and invalid GLX cleanup result.
Combined full013/014/0151077cases:925pass/149skip/1warn/2fail. Exact44082
remaining launched as full016, same original binaries/test oracles/settings.
Its final outcome is not part of this checkpoint. No coverage waiver or full
M12.1-M13.1 acceptance claim.

Owner asks about black monitor: read-only check shows Xorg21335 live, DP-1
connected/enabled,1000MHz/64C, same boot. This is the minimal Linux Xorg test
session, not Windows desktop. Connector state alone is not proof of pixel
correctness; the separate swapbuffers control supplies rendering evidence.
