# M578: vertex stream/layout transitions crash hosted GPU

EXE97110446 runs six state-transition cases under two submission patterns.
WARP094 and CPU095 pass all48 images (196608 new pixels), plus the preceding
eight graphics checks. GPU096 and097 pass the four viewport images, then
terminate0xC0000005 in streams-layout-stride before its first readback and
before the Flush-after-every-draw pattern. Both runs use UMD50491FA3/ICD3508416F.

Dump097 identifies a read at0x36c in update_buf_bind_count, inlined into
zink_set_vertex_buffers_internal via ResolveState/Draw. Vertex slot3 references
a resource with obj=NULL, deleted=true, all_binds=0 and vbo_bind_count=1.
The stack and fields are derived from matching private symbols. The raw dump
and full diagnostics remain private, hash-bound in private-hashes.json.

Source lead: InputAssembly.cpp's ReferenceVertexBuffer calls
pipe_resource_release on each old binding reference, while p_context.h:963-968
says that callback transfers full ownership to the driver. Zink marks a bound
resource deleted on that callback. Correct ownership and a fix-and-retest
must establish the cause; BD-043 causality is still unproven.

Both GPU runners restored baseline93B1D1FD/8279AC7F, CPU DWM4400 unchanged.
The temporary per-executable WER key used for097 was confirmed absent after
collection. No desktop switch or candidate promotion. Parent revision aeb3ae1.
Prepared package040 was not run on the lab; final artifact is package041.
