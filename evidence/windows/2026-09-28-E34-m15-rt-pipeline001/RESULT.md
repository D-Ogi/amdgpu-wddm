# M761: first direct RT pipeline content control

CTS dEQP-VK.ray_tracing_pipeline.trace_rays_cmds.direct.8_8_1 completes Pass on exact RT-fixed ICD949669FF9194C6F5A63D4DCB175939AFE7AD0067A084417C0D2207A7CC0C8C0F. Case3.311s, supervisor20.344s,66.9C. Live candidate module observed; RADV_DEBUG=info, RADV_EXPERIMENTAL empty. Default hardware RT configuration reports has_image_bvh_intersect_ray=1; no ISA witness yet.

Same CTS executable, thin direct-ICD adapter, hash/module admission and bounded harness as M760; only case changes to the named direct TraceRays8x8x1 test. QPA is the original self-validating output. Job closes; CPU171 boot, DWM, binary/registration identities, generation, epoch and flags unchanged. No deployment/restart.

This proves this RT pipeline case only, not full RT CTS coverage, emulated parity, performance, native D3D12 DDI/FL12_1 acceptance or Witcher 3 RT. The matching no-traversal negative in M760 applies to its ray-query case, not this pipeline test. Raw logs remain scratch/m15/rt-pipeline001; selected receipt excludes process identifiers.
