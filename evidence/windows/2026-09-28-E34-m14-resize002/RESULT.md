# Native ABI1.4 resize002 passes

UMD75686175 (source91199e0e), engineDC65, ICDC388 and configC75C admitted by
caps005/M751. Exact hashes are in stage-manifest.json. Native system D3D11/DXGI,
unchanged e1883623 client and independent resize001 reference. CPU negative routing
control precedes GPU; router selects only the exact test process with an expiring flag.

Both clients pass three ResizeBuffers operations,12 Present calls and12 pixel checks.
CPU uses FL10_0; GPU uses FL11_1. Sizes64x64,65x33,127x79,64x64, three rotating
RGBA colors per size. Host independently parsed all24 PAMs and checked every pixel
against the color/geometry oracle, not merely client hashes. GPU images retained here;
CPU images remain in local raw records. This is synchronized functional validation,
not an independent composed-screen capture, latency measurement or no-copy proof.

Supervisor passed47.5016559s; baseline restoration/postflight and child-tree closure
confirmed. Subsequent inspect15:36:27Z reports task Missing. Same CPU171 system
artifacts, boot, DWM and device generation. No OS/DWM restart or driver update.
Raw records: scratch/m14/resize002-ops. No active test remains.

Host controls passed exact-process routing, phased supervisor, file rollback and
13 invalid resize results. The staging copy had an obsolete generic scene test;
it was replaced with a resize-specific test before the manifest was frozen.
The repository's prior resize001 test was already correct; this was a packaging error.

Native fault injection/error propagation, Trim residency under native DDI, broader
D3D11 compatibility, D3D12 and FL12_1 remain open. Engine error-state host tests
and the standalone engine OOM test do not establish those native-runtime properties.
