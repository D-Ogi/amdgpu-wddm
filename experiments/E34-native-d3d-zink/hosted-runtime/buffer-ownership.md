# Frontend vertex-buffer reference ownership

M578 shows obj=NULL/deleted=true while reusing a bound vertex resource.
The frontend's ReferenceVertexBuffer helper calls resource_release for every
old binding reference. Gallium p_context.h:963-968 defines this callback as
transferring full ownership to the driver. Zink defers a released bound resource
until its last bind disappears; repeating that notification during ordinary
rebinding can retire a resource the frontend still intends to use.

Use ordinary reference updates for the frontend's cached vertex-buffer bindings.
Transfer the final buffer ownership through resource_release when DestroyResource
releases the logical D3D buffer. Keep image teardown and imported-surface teardown
unchanged. Device teardown already unbinds the CSO context before dropping its
cached vertex references. This separates binding changes from resource destruction.

Apply buffer-ownership.patch to its exact manifest inputs after the M575 patch.
Repeat the unchanged M578 EXE on the new UMD. Require all48 images to match
WARP/CPU in both submission patterns, successful resource teardown and baseline
restoration. Then regress textured batching, sharing and Present before another
DWM probe. A passing offscreen control does not close BD-043 by itself.
