# M549: import the existing E26 TTN lowering into the hosted Zink build

The TTN-only diff from consolidated commit c1f06fd6d8895ae3adaf7951961b4fb46416d2b8
applies to the active hosted source. It retains separate texture and sampler indices
and SAMPLE variants/SVIEWINFO from E26. The older standalone E26 patch has stale
TXF_LZ context; the consolidated diff uses the matching 05e6c962 upstream base.
PROVENANCE: Mesa, MIT; existing project E26 changes imported without rewriting.

The source matches the consolidated commit after LF normalization. Replaying the
retained patch over the saved pre-change file reproduces the current file byte for
byte. See artifact.json and the hosted-runtime/ttn-e26-manifest.json manifest.

The diagnostic UMD builds successfully; all eight scoped fast gates pass in
build.log. These gates do not exercise TGSI texture sampling or Zink descriptors.
No deployment or DWM test was performed for this artifact.

Static inspection finds that zink_binding still asserts MESA_SHADER_KERNEL for
VK_DESCRIPTOR_TYPE_SAMPLER. The imported lowering emits a bare sampler for D3D
SAMPLE instructions. Graphics-stage descriptor support and a native pixel control
with different texture/sampler indices are required before another DWM probe.
BD-035/036 cleanup and the complete G0 acceptance criteria remain open.
