# M554: correct the SPIR-V binding for graphics bare samplers

emit_sampler decorated its variable with driver_location, the Gallium slot,
instead of data.binding, the translated Vulkan binding. These coincide for the
existing kernel-stage path but differ in the M553 graphics prototype. The one-line
correction makes SPIR-V match the descriptor layout. Optional BC250_HOST_TRACE_SAMPLER
logs sampler creation, separate SPIR-V sampling and descriptor-buffer writes.

Before correction, run043 observes a white-border sampler created and written to
the requested descriptor while the negative test still incorrectly passes.
2x2 clamp run044 fails the survivor-green readback; wrap-negative045 reaches the
same mismatch then crashes in bind_gfx_stage during DestroyDevice. Its private
minidump was analyzed offline; raw dump/addresses remain outside the repository.
The incorrect descriptor binding means these executions cannot establish sampler
or lifetime correctness. No independent claim about the crash root cause is made.

With corrected UMD5A48852B and hosted ICD3508416F:
- run046:1x1 t1/s0, three4096-pixel exact red/blue/survivor-green readbacks,exit0.
- run047:s0->s1 white border,4096 mismatches,expected rejection exit6.
- run048:t1->t0 white texture,4096 mismatches,expected rejection exit6.
- run049:2x2 clamp,three4096-pixel exact readbacks including survivor-green,exit0.
- run050:2x2 sampler changed to wrap,4096 mismatches,expected rejection exit6.
All five use the same UMD/ICD. Shader cache is disabled. Artifacts and unchanged
raw logs are retained; checks.json asserts both positive and negative outcomes.
Three-file LF replay and eight scoped build gates pass.

All runs restore CPU UMD8279AC7F and registered ICD9C40083C; DWM9648 is unchanged.
This validates the tested SAMPLE texture/sampler slot combination and two-device
lifetime, not every SAMPLE variant, arrays, comparison sampling, arbitrary slot
counts, nonzero-base draws or broad GL/CL regression. GPU DWM and no-copy G0 remain
unproven. A bounded DWM probe is now the next acceptance step.
