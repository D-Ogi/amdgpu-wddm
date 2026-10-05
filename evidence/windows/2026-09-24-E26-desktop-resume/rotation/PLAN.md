# Resource identity rotation

PROVENANCE: Mesa801c9763c6, MIT. Incremental patch follows E26 branch-labels.
Local Microsoft ref/ddi-display/dxgiddi.md2469 explicitly requires rotating
kernel identities and preserving runtime handles. The old frontend copied
pixels through a temporary texture and kept the same kernel handles. Owner
reports overlay drawing as three descending bands, each about half a second.
Hypothesis: reused displayed backing exposes incremental software rendering.

Rotate resource backing, allocation/GPUVA/mapping together; retain runtime
handles; retarget all live RTV/SRV and bound framebuffer/sampler state. Flush
before ownership change. No pixel copies in rotation, no KMD/register change.
Extracted actual function test covers two/three buffers, six rotations each,
retained and bound views; omission of handle rotation is a negative control.
Build passes; test0failures, mutation18failures. Host test does not prove timing.

Register new DLL, existing127full+blit+hardwareflip configuration, PnP reload
for cached UMD path and DWM restart. Collect scanout, raw traces, Present kernel
handles/rotation, hardware flip counters, color/shared D3D positive control and
owner observation. Roll back DLL registration if failed; no routine OS reset.
