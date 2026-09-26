# M558: nonzero D3D vertex-ID offsets

Unit A, existing KMD0.7.152.1. Source: bc250-win06bc7f7 plus vertex-id.patch,
Mesa base05e6c962 with recorded E34 patches. UMD A68C2451, hosted ICD3508416F.
Full hashes in manifest030.json. All eight local gates and one-file replay pass.

The identical control EXE0EF77CFC passes all three cases on WARP058, baseline
llvmpipe059 (UMD8279AC7F), and hosted Zink060. Each reads R=0/1/2 at the three
point positions, G/B/A=2/3/4, with zero mismatches. Cases are Draw(3,0), Draw(3,5),
and DrawIndexed(3,0,7) with indices2/0/1. The shaders carry the unmodified
SV_VertexID through a flat varying, using ID modulo3 only for point positions.

This verifies the tested nonzero StartVertexLocation and BaseVertexLocation
semantics against two software references. The frontend retains VERTEXID_NOBASE;
Zink subtracts first_vertex from vertex_id. The pass now runs before shader-init
info gathering, gated on vertex stage and draw_parameters. It no longer depends
on the TGSI finalize path. No unsupported-capability fallback is implemented.

Initial WARP056 failed even the zero case because the control's PS input
signature did not match VS output registers; it interpreted position bits as ID.
That invalid control is retained separately with its old manifest and raw logs.
Matching the full VS/PS interface fixed the zero control before GPU testing.
056 is not evidence of driver or D3D semantic failure.

All run logs are unchanged. DWM1528 remains the same, and GPU060 restores CPU
UMD8279AC7F/registered ICD9C40083C. No OS reboot, DWM replacement or promotion.
No claim for instancing, negative base offsets, converted primitives or every
shader entry path follows from this bounded test. G0 and no-copy proof remain open.
