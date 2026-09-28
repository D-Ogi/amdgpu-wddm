# M744 - CPU native window passes; GPU swap-chain resource is rejected

Window003 source b3bd75b1, manifest
14F93B4CB4DE0775194853C27C17FA507B87904A0C1C47303798B098D998BA9D.
Independent SYSTEM thermal probes and direct active-console Job supervision
pass for both phases, with no monitor error/cancellation/timeout. Both Jobs close.
The previous launcher/privilege failures no longer prevent a D3D11 attempt.

CPU system D3D11 FL10_0 creates the windowed FLIP_DISCARD swap chain, exits0,
and completes the three frozen scenes, with checksums identical to runtime011.
The client treats every Present return other than S_OK as failure. Its staged
readback validates rendered content, not independent screen composition.

GPU system D3D11 FL11_1 creates a device but reports E_NOTIMPL80004001 from
create's convert_runtime_resource rejection (frozen source line167). D3D11 then
removes the device; swap-chain creation returns887a0005 and client exits4.
No GPU scene or successful GPU Present is measured. The exact rejected descriptor
was not logged, so its unsupported field is not yet established. Three normal
process minidumps are preserved in scratch, outside the repository.

Supervisor failed-restored44.3563522s; CPU verified, GPU false. Baseline/postflight
and both phase trees verified, task cleanup succeeded and Missing observed.
CPU171 restored, no KMD/DWM/OS restart. Raw scratch/m14/window003-ops.
Selected records omit environment, module paths and timings; failure-excerpt
contains only error/closure lines. No performance, D3D12 or FL12_1 acceptance.

Next diagnostic build logs at most eight rejected runtime resource descriptors
with dimensions, format, flags and usage, without pointers or memory contents.
Existing mandatory DDI host tests and build pass; that diagnostic is not yet
live-tested and does not change feature claims or silently replace shared
runtime allocations with engine-private resources.
