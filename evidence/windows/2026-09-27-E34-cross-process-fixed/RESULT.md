# M565: shared-resource lifetime and linear pitch controls

Unit A, source ff99662 plus shared-pitch.patch. Final UMD23F5269C, hosted
ICD3508416F and strengthened control64B47285 are identified in manifest.json.
The control and dimensions are unchanged from the M563 CPU reference068.

## Results

- 072: UMD F8BB0D3E passes10 exchanges and20480 pixels per direction. Both
  resource closes have resource_refs=1/object_refs=1 and successful deallocation,
  with no device loss. This directly contrasts with067's failed final release
  on the same executable and pre-fix UMD B69AA635.
- 073: the same candidate passes100 exchanges and releases both resources,
  then fails creating68x36. Requested pitch320 is rejected by RADV with
  VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT. This is retained as
  a separate failure after the completed-batch lifetime fix.
- 074: final UMD23F5269C passes1000 bidirectional exchanges and10 texture
  generations,2702400 pixels in each direction, zero mismatches. All20 shared
  resource closes have sole references and S_OK deallocation. There are no
  device-loss or Vulkan-error messages. Both processes finish normally.
- 075: the same final UMD passes120 native two-buffer FLIP_SEQUENTIAL Presents.
  Final green readback is0/76800 mismatches and GetDeviceRemovedReason is S_OK.
  Sampled monitored fences agree through submitted=completed120. Both runtime
  resources close with sole references and S_OK deallocation.

The global KMD counter interval for074 spans14.953 seconds. Live objects remain
195 before and after. Device create/destroy deltas are4/4, context deltas8/8,
process deltas2/2, and allocation-open/close call deltas128/128. Node0 hardware
submitted/completed deltas are6000/6000; timeout and refusal counters stay zero.
These are global observations, not per-process attribution. Allocation-create
and allocation-destroy call counts are not one-object counts and are not
claimed to balance. Stable final live objects excludes retained global objects
at these endpoints; it does not replace an in-process memory-growth or residency
stress measurement. Derived last-summary lines and private raw-log hashes are
retained separately.

## Implementation and validation

M563's completed-batch change reclaims the screen's already-completed global
batch list before the hosted helper tests sole ownership. The fence wait and
ownership checks remain. The pre-fix failure and072 success support this cause
for the measured teardown failure.

The pitch correction changes new runtime surfaces from64-byte alignment for
nonprimaries to256-byte alignment, matching Mesa's GFX9-GFX11 linear surface
validation and GFX10 AddrLib. It does not reinterpret existing imported layouts.
Thus old CPU-owned allocations with incompatible recorded pitch still require
compatibility work; the old CPU baseline binary was not rebuilt or replaced.

All eight existing build gates pass; exact one-file pitch-patch replay passes.
M563 retains the three-file completed-batch replay. Full1000-exchange execution
and075 validate the exact final artifact, not merely the source or build.

Attempt071 failed before creating the test process because a generated CLI path
contained a backspace from Python string escaping. Its cleanup then tried to
kill a process object that had not started. Both harness errors were corrected
in073; the unsuccessful runner/output are retained. This is not a GPU failure.

## Scope and restoration

GPU completion is established with EVENT queries before each CPU-event handoff.
This proves serialized cross-process content exchange, open/close, recreation,
extent changes and staging map/unmap for these tests. It does not prove an
asynchronous cross-device GPU-fence protocol, eviction/residency stress,
unexpected producer exit, or all M13.2 requirements. No full G0/M13 closure or
permanent GPU desktop promotion is claimed.

Every completed control restores baseline UMD8279AC7F/registered ICD9C40083C;
DWM84 remains unchanged. Task075 is terminal with exit0. No OS reboot, KMD
update or DWM restart. The private screenshot from075 is retained by hash and
is not used as a full-screen pixel oracle. Raw KMD logs and the screenshot stay
private; public counter extracts are explicitly derived. Other copied logs
are preserved as captured. No credentials or account identifiers are included.
