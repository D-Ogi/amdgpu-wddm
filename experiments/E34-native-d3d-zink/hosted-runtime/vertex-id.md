# Vertex-ID offset control

Hypothesis: the D3D VERTEXID_NOBASE intrinsic, lowered as vertex_id-first_vertex,
returns the same values for nonzero Draw start and DrawIndexed base as WARP and
baseline llvmpipe. Keep the frontend semantic unchanged.

Move the existing pass from shader_finalize to shader_init before info gathering,
for vertex shaders with caps.draw_parameters. No claim for devices lacking this
capability. PROVENANCE: Mesa, MIT.

Control: runtime-vertex-id-control.cpp draws three points to a3x1 float target.
The vertex shader emits SV_VertexID unchanged as a flat varying; the pixel shader
writes it to R and constants2/3/4 to G/B/A. Point position is ID modulo3. Cases:
Draw(3,0), Draw(3,5), DrawIndexed(3,0,7) with indices2/0/1. Expected R is0/1/2
at the three pixel positions for every case. The first case controls rasterization
and readback. All four channels are checked, with a distinct clear sentinel.

Run WARP and baseline CPU UMD first without replacing system libraries; then the
bounded hosted runner with the new candidate. Preserve exact outputs, hashes and
runtime identity. Any mismatch is a failure requiring investigation; matching
zero case alone does not establish offset semantics. No DWM restart or promotion.

Results: [M558](../../../evidence/windows/2026-09-27-E34-vertex-id/RESULT.md).
WARP058, llvmpipe059 and hosted Zink060 agree in all three cases. The initial
invalid-interface control056 is retained as a test defect, not a driver result.
