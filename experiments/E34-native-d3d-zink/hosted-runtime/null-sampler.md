# Default sampler and stage-binding controls

Hypothesis: a per-device default sampler implements D3D NULL sampler semantics
without passing a null Vulkan sampler to RADV; respecting the screen view cap
preserves PS views when GS resources change.

PROVENANCE: Mesa, MIT.

The local DDI reference lacks the default-state table. Contract checked against
[Microsoft PSSetSamplers](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-pssetsamplers):
linear min/mag/mip filtering, clamp addressing, zero bias, disabled comparison,
white border and the full float LOD range. Gallium uses zero anisotropy to
represent disabled anisotropic filtering, as the existing CreateSampler does.

Apply null-sampler.patch after view-bounds.patch. The device initializes all
supported graphics-stage sampler slots, substitutes this state on NULL binds,
and unbinds/deletes it on normal or partial-creation teardown. The diagnostic
creation-failure selector gains the sampler stage.

Procedure: package028, same bounded offscreen runner and rollback as M554.
051 repeats the explicit sampler positive. 052 samples t1/s0 with a sampler
never set on the first device, explicit NULL on the second, then a real sampler
replaced by NULL on the surviving device. Each draw first binds a GS view and
then clears all GS views after PS binding. UV2.25 tests clamp. 053 uses UV0.5
on the two-by-two texture; exact UNORM output tests linear interpolation.
All controls read three targets of4096 pixels and exercise two-device teardown.
Failure, timeout or mismatch rejects the candidate; baseline hashes and DWM PID
must be unchanged. No DWM replacement, OS reboot or performance claim.

Build, eight local gates and patch replay pass. Controls051-054 pass; see
[M557 results](../../../evidence/windows/2026-09-27-E34-null-sampler/RESULT.md).
054 additionally injects failure at all eight creation stages, including sampler,
and verifies null outputs followed by successful device recreation and rendering.
