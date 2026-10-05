# Program invalidation when replacing a graphics shader

M574 aborts while adding a reference to the previous graphics program at the
start of a new batch. The program's reference count was zero. Zink currently
clears the current-program pointer only when binding NULL. The D3D frontend
uses empty shaders for NULL D3D shader bindings, so replacing and then deleting
the old shader can leave that pointer until the next program update.

Hypothesis: invalidating the cached graphics/mesh program on every stage bind,
including non-NULL replacements, removes the stale pointer before shader
deletion. Remove its variant hash at the same point as the existing NULL path.
Normal dirty-state handling selects and references the replacement program at
the next draw. Keep deferred batch references for submitted GPU work intact.

Apply program-binding.patch to the hash-bound input in
program-binding-manifest.json. Build the UMD with the existing debugoptimized
configuration, preserving the previous DLL/PDB. Repeat the unchanged M574
graphics-texture-control.exe: all eight earlier checks and eight texture images
must pass, matching the existing WARP/CPU oracles. Any crash, mismatch, timeout
or restoration failure rejects the candidate. Then repeat batched solid-color,
sharing and Present regressions before another bounded DWM attempt.

This is a testable lifetime fix, not yet an explanation of BD-043. Do not infer
dynamic desktop correctness from the offscreen control.
