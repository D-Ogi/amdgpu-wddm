# M748 - Native GPU swap-chain resize and identity rotation control

Resize001 source8dad0569, client e1883623 (SHA256
4BF5B1F5BDB834EE8E43DCEB571C12A280A36325656D25304DC3E51114A706C2),
manifestF8B00C7A50E573CD20AB9E1393B6CA9F92D57FA522C5C26F8CE78D05E01DF7EF.
UMD96615291, engine253A and ICDC0CE are unchanged from M747.

CPU FL10_0 is the first positive control, then native system-D3D11 GPU FL11_1.
Each passes twelve Present calls (S_OK only), three ResizeBuffers calls and
exact color readbacks at64x64,65x33,127x79,64x64. Each size renders three distinct
colors across the flip-discard buffers. Before resizing, context bindings and
back-buffer/view references are released; the reacquired geometry is checked.
Event queries retire each iteration. Both processes exit0 and both scene
records equal independently generated expected checksums/counts/dimensions.

All24 PAM files were separately pulled to the host. Every decoded RGBA pixel
matches the independent red/green/blue/alpha255 oracle. The twelve GPU PAMs are
included; CPU/GPU file hashes and verified pixel counts are in pixel-validation.
No tolerance is used. This catches stale size/content after buffer recreation
and rotation in this bounded workload; it is not exhaustive lifetime/leak proof.

Supervisor passes43.029629s, CPU171 restored, baseline/postflight and both
interactive Jobs verified. No thermal monitor error or cancellation. Task Missing
observed2026-09-28T14:46:59.1712839Z. No KMD/DWM/OS restart. Raw records under
scratch/m14/resize001-ops. Selected JSON omits environment, module paths and
timings. Test images are synthetic colors. Logged887B0001 is pending query
completion, followed by successful completion rather than device removal.

This test intentionally synchronizes and reads back each frame. It establishes
functional API/content behavior, not copy-free presentation, screen composition
or performance. D3D12/FL12_1 and broader conformance remain open.
