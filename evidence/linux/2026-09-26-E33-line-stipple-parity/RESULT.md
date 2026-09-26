# Same-unit line-smooth-stipple comparison (M516)

PROVENANCE: Mesa MIT05e6c9622e135ac2aeaf56ec70222642627e2162 plus M512
canonical48-bit scratch patch; piglit MIT0cc014230c0701d7a61bf240009ddd0711462d4e.

Unit A booted Linux6.18.52-0-lts. GPU1000MHz, requested820mV/read818mV,
loaded module hash5984c673... matches M510. Mesa and full piglit build exit0.
Installed RADV FEC7C475 and Gallium1FA58014 are bound by build.sha256.
The matched canonical scratch patch applies cleanly; before/after source
hashes are saved. Unchanged large-array shader exits0 and reports pass.

The unchanged line-smooth-stipple executable, -auto -fbo, exits1 on Linux:
pixel(9,5), expected31/95/31, observed0/128/0. This exactly matches Windows
M515, including first failing pixel and RGB values. Linux reports a failing
pixel oracle, not timeout or device loss. This is a shared observed failure,
not a pass, waiver, complete profile comparison, or conformance claim.

The Linux control uses surfaceless EGL; Windows uses WGL with -fbo. This
comparison covers the offscreen content only. GLX reports missing DRI3;
X11/EGL aborts in driQueryOptionb. Windowed Linux reference remains open.

The preceding unclean ext4 build image was copied before journal recovery.
Read-only recheck passed before mounting. No partition or firmware changes.
Logs retain the Linux clock, which differs from host UTC; use boot chronology
and process results, not cross-OS wall-clock subtraction.
