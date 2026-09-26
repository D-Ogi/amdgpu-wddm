# Full007: line smoothing/stipple mismatch

PROVENANCE: Mesa MIT, commit05e6c9622e135ac2aeaf56ec70222642627e2162;
piglit MIT, commit0cc014230c0701d7a61bf240009ddd0711462d4e.

Unit A, KMD151, RADVB365C281 and Zink1DAF0B60. Full quick007 stops at
1418 terminal cases:1193pass/224skip/1fail. The failure is
`spec@!opengl 1.1@line-smooth-stipple`, process exit1, not a timeout.
Pixel(9,5) expects RGB31/95/31 and observes0/128/0. The unchanged
upstream test enables smooth, blended, width3 stippled lines and probes
fractional edge coverage for complementary patterns.

All five repaired cases pass in this run: front/back distraction, raytrace
teardown, large-array scratch, BeginEnd validation and swapbuffers behavior.
GPU health before/after remains generation589506402/epoch5, flags15; the
worker restores the baseline9C40083C registration before OS transition.

The exact local upstream Zink/RADV CI failure list also names this test.
That is a lead, not proof of Linux parity on unit A and not a waiver or pass.
Next action: same-unit Linux control with the matching canonical scratch
patch and unchanged test, then full-profile comparison. No test was removed
or changed, and no timeout was raised. Full M12 acceptance remains open.
