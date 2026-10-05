# M553: textured native draw prototype, sampler acceptance fails

This candidate is not accepted as correct separate-sampler support.
It adds graphics-stage bare sampler bindings, recombines a combined descriptor's
image with the requested bare sampler in SPIR-V, and deduplicates TTN sampler
variables. Binding storage grows to cover textures, samplers and compact images.
All eight scoped build gates and six-file LF byte replay pass.

The native control samples t1 with s0 at UV(2.25,2.25). Texture t0 is white;
t1 is the expected red/blue/green. Sampler s0 clamps; s1 has a white border.
Three4096-pixel readbacks on two devices pass in run039 (UMD AB2ECD0A).
Texture-index mutation t1->t0 is rejected: run040 exits6,4096 mismatches.
However sampler-index mutation s0->s1 incorrectly passes all three readbacks
(run041). Repeating with shader cache disabled and ZINK_DEBUG=nir also passes
(run042). NIR retains the separate sampler dereference and out-of-range UV.
The sampler-state/binding path is unresolved; the positive alone is insufficient.

Earlier failures are retained: run036 hits unsupported load_vertex_id_zero_base
and times out124. Offline private dump and stderr identify emit_intrinsic.
A NIR lowering to load_vertex_id minus load_first_vertex removes this assertion;
this is the inverse of the existing nir_lower_system_values identity. Nonzero
base/indexed draws have not been tested. Runs037/038 then fail with access violation
0xc0000005. Offline crash038 points to zink_bind_vertex_addresses, whose element
state is null. UMD now marks vertex elements dirty initially and binds an empty
CSO layout for null D3D input layouts. Run039 uses this fix successfully.

The exception filter added to the control writes only its own lab process dump.
Raw dumps and address-bearing debugger output remain private under scratch.
No live kernel debugger was used. All runs restore CPU UMD8279AC7F and registered
ICD9C40083C; CPU DWM9648 is unchanged. No GPU-desktop or no-copy proof. Do not
promote this candidate to DWM until the sampler negative control rejects correctly.
