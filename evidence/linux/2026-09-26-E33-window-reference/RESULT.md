# Linux window reference (M517)

PROVENANCE: Mesa MIT05e6c9622e135ac2aeaf56ec70222642627e2162, piglit
MIT0cc014230c0701d7a61bf240009ddd0711462d4e. Canonical scratch patch M512 retained.

The separate /opt/bc250-xorg build uses RadeonSI/ACO, LLVM disabled, for Xorg.
Client libraries remain /opt/bc250 Zink/RADV, RADV FEC7C475 restored exactly
before this experiment. Xorg log witnesses glamor and RadeonSI initialization;
client wflinfo reports Zink/RADV BC-250, OpenGL4.6, exit0 without WSI error.
The unchanged gl-1.0-swapbuffers-behavior -auto window test passes its pixel
oracle and exits0. It reports true swapping, matching Windows M514.

Earlier system Mesa25 could not identify this GPU and fell back to llvmpipe.
Using Zink in Xorg allowed GLX identity but DRI3 pixmap import failed BadAlloc;
that diagnostic remains outside the repo. RadeonSI server resolves this control.
Only renderer-related Xorg lines are exported; EDID/monitor identity omitted.

Full Linux quick003 is now running under the new Linux wrapper. The first two
starts are preserved as packaging failures: missing data with build-tree root,
then missing libpiglitutil_gl with install-prefix RPATH. Neither was a GPU test
failure. Standard CMake install plus explicit package library search path fixes
those preparation errors. Full003 has actual pass/skip results, not completion.
Linux inventory45159 differs from Windows45037; exact name sets are recorded.
No platform-only case is silently dropped. Full comparison remains pending.
