# UMD-created shared surfaces use LB7A, not necessarily BC2A

Source inspection,2026-09-27; no new runtime capture. Mesa source a0ad8af5ea46b90400d947d82d056f3bd58278c5,
clean worktree used for the current hosted UMD5C74BF98. File identities in source.json.

In src/gallium/frontends/d3d10umd:
- Resource.cpp:352-355 calls Bc250EnsureSurface for primary, PRESENT-bind and
  SHARED resources after initial Gallium resource creation.
- DxgiFns.cpp:91-129 constructs the32-byte LB7A SurfacePrivate, including width,
  height, pitch, D3DDDIFORMAT and allocation bytes, and passes it to pfnAllocateCb.
- DxgiFns.cpp:133-155 maps that runtime allocation, requests MakeResident on the
  UMD device and waits for its paging fence.
- DxgiFns.cpp:157-173 imports the same allocation into the hosted screen using
  its handle, stride, size, linear modifier, VA and device identity, replaces
  the initial Gallium resource and returns before the CPU Lock2 branch.
- Resource.cpp:463-496 accepts an LB7A prefix on OpenResource and imports the
  existing runtime allocation through the same Bc250EnsureSurface path.

Consequently, the phrase "UMD-created DWM texture" does not imply a BC2A blob.
For this frontend's shared/presentable surfaces the runtime allocation is typed
LB7A. Internal RADV BOs may use BC2A, but that does not change the private data
of the imported runtime allocation. Current KMD snapshot admission can describe
LB7A surfaces; it need not accept arbitrary BC2A BOs to describe these resources.

This corrects the assumption that implementing a BC2A texture extension is
necessarily a prerequisite for the first CDD/DWM interop trial. It does not prove
which destination the CDD will actually present to with the cap enabled.
Capture the actual context and backing identities and LB7A descriptor on that
path. Keep untyped BC2A rejection. The UMD MakeResident call only establishes
intent on the UMD device; it is not proof of CDD system-device residency or
cross-context producer completion. Those remain required independently.
The CPU UMD's Lock2 limitation for GPU-only standard GDI types also remains.
