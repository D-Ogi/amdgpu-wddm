# M557: default sampler and bounded view controls

Source base: Mesa05e6c962, E34 patches through view-bounds in19bcd86 plus
null-sampler.patch. UMD E8D4B4A9, hosted ICD3508416F; full hashes in manifest.json.
Unit A, existing KMD0.7.152.1. All four controls exit0 and restore CPU baselines
UMD8279AC7F/ICD9C40083C; DWM1528 remains unchanged throughout.

- 051: prior explicit-sampler positive, three exact4096-pixel targets.
- 052: initially unbound sampler, explicit NULL, then bound-to-NULL transition;
  clamp sampling at UV2.25 passes three exact4096-pixel targets.
- 053: same cases at UV0.5 on a two-by-two texture; linear interpolation gives
  exact expected UNORM channels255/64 and passes all three4096-pixel targets.
- 052/053 issue GS bind/clear calls after PS view binding. No cross-stage pixel
  corruption is observed. Runtime DDI coalescing is not independently traced.
- 054: eight injected creation failures, including sampler, return8007000e with
  null device/context outputs. Subsequent two-device rendering and survivor
  device readback pass three4096-pixel targets. Actual system OOM is not induced.

The sampler exists before any runtime sampler setter and is released on normal
and partial-create teardown. Its default follows Microsoft PSSetSamplers:
https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-pssetsamplers
The local DDI reference did not contain that table. View bindings respect each
screen stage cap; high non-NULL slots are rejected, not supported by this patch.
High-slot rejection has not yet been exercised on the lab.

The local build, eight fast gates and three-file LF patch replay pass. Build
logs retain the final successful build. The preliminary missing initializer_list
include failure remains private in scratch; it was fixed before deployment.
Raw retained control logs are unchanged; no raw dumps or screenshots are included.

This does not prove broad sampler correctness, vertex-ID offsets, full128-view
support, GPU desktop G0 or absence of full-frame CPU copies. No promotion.
