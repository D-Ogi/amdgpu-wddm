# Batched dynamic graphics isolation

BD-043 photographs show diagonal triangular corruption and stretched/repeated
window content. Test dynamic draw state before attributing this to Present.
Build graphics-state-control.cpp and invoke `warp batch`, `baseline batch`,
then `hosted batch` on the lab through the same bounded runners as M567.
Use current registered ICD93B1D1FD as the restoration baseline. DWM remains CPU.

After the eight original graphics checks, run three groups of256 draws without
per-draw query waits: scissored fullscreen draws with UpdateSubresource constant
updates; six-vertex quads with DISCARD vertex/constant buffers; and indexed quads
with DISCARD buffers, StartIndexLocation3 and BaseVertexLocation2. The direct
variant uses a16-byte vertex-buffer offset instead. Two sentinel vertices
precede the valid data. The geometry variants use no scissor, so stray geometry
is not masked by the test rectangle.

Each group overwrites64 distinct8x8 tiles four times in permuted order. The last
pass has a defined per-tile RGBA color; one final query wait and staging readback
check all4096 pixels exactly. Total new coverage:768 draws and12288 pixels.
Any mismatch, timeout, removal or missing renderer witness rejects the control.
A pass only excludes the tested combinations; it does not prove texture-state,
asynchronous shared-surface, Present or desktop correctness.
