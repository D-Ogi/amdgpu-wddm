# M575: invalidate cached graphics program on shader replacement

The unchanged M574 textured control090 passes with UMD50491FA3 and hosted
ICD3508416F. All eight textured snapshots match WARP086/CPU087 exactly;
the eight original graphics checks also pass. The previous UMD23F5269C
aborted in the same control twice (M574088/089).

The patch moves existing program-pointer invalidation and variant-hash removal
out of the NULL-only branch in bind_gfx_stage. A non-NULL replacement can now
release the old shaders without leaving curr_program/mesh_program referring
to their program. Batch references still protect already-submitted work.
Exact LF-input patch replay passed; the debugoptimized UMD rebuilt successfully.

Regressions:091 passes768 batched solid-color draws with matching hashes;
092 passes1000 cross-process exchanges across10 texture generations,2702400
checked pixels in each process;093 passes120 flip-model Presents and the
end-only readback. Each runner restores ICD93B1D1FD/CPU UMD8279AC7F with hash
checks; CPU DWM5748 is unchanged. No DWM/G0 visual pass or promotion is claimed.

See program-binding.patch and its manifest in the experiment directory.
The test EXEs are unchanged from their previously recorded controls. manifest.json
binds all tested artifacts. Original full diagnostics remain private, identified
by private-hashes.json; relevant KMD counter lines are in verification.json.
Parent repository revision8415ed8. Mesh invalidation is symmetric in source;
this D3D10 UMD control does not exercise mesh shaders.
