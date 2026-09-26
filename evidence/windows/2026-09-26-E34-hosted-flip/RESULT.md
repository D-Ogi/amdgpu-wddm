# Hosted flip-model window control

Unit A, 2026-09-26. Driver candidate015 is unchanged from M544:
ICD9064C398 and UMD4764DAD9. The native control changes swapchain creation from
one-buffer DISCARD to two-buffer FLIP_SEQUENTIAL, otherwise retaining120 frames,
red/blue/green phases and the end-only76800-pixel staging readback. No injected
loss. Actual GetDesc reports effect3 and buffers2, create/Present all succeed.

run030 exits0; all76800 final pixels match green, no device removal. Three
independent GDI composed-screen captures each contain46800 exact red, blue and
green pixels respectively in ROI [180,200,440,380]. Capture times are in the log.
Unmodified full-screen PNGs stay in scratch/g0-hosted because unrelated desktop
content is present. Their hashes and test-ROI measurements are preserved here.

Bidirectional GPU wait/signal diagnostics are present. CPU UMD8279AC7F,
registered ICD9C40083C and prior app UMD are restored and hash checked. DWM1052
remains the CPU compositor; no OS restart or KMD change. Exact control and DLL
hashes are in flip-manifest030.json; source is runtime-flip-control.cpp.

This proves this bounded native flip-model client displays correct colors.
It does not prove which internal identity callbacks occur, direct scanout,
no CPU copying, cross-process1000-iteration sharing, GPU DWM or G0 completion.
Next is a bounded DWM hosted-route test with rollback and process/GPU evidence,
after the remaining ownership and failure-path review needed for that test.
