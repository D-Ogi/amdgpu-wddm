# Negative control: zero-extending high-half scratch addresses

PROVENANCE: Mesa MIT; LLVM Apache-2.0 WITH LLVM-exception (read-only reference).
Unit A, same Linux session and bounded2048-element shader as M510. A single
ACO change for GFX10 removes the unconditionalffff0000 upper bits while still
subtracting the descriptor swizzle bit. Linux ICD6ffdc1f7ce447db90566ff72d6fe4e5c34e87c288902b256af3d55bc6e02a002.

The emitted prolog witnesses s_addc_u32 with80000000 instead of7fff0000.
The pixel test does not complete. Linux logs ring gfx_0.0.0 timeout with
signaled seq16/emitted17. The shader process main thread is a zombie and its
Gallium worker19309 remains in D state. Timeout wrapper19303 did not terminate
the kernel wait; it was killed after collecting logs. Wrapper137 is forced
termination, never a test result. No further GPU workload was submitted.

This refutes unconditional zero extension as a cross-platform GFX10 fix: the
known Linux high-half address needs the previous upper-bit convention in this
control. It supports investigating address-dependent reconstruction for low
Windows addresses, but does not prove that Windows failure has that cause.
The inspected LLVM PAL path alone was insufficient to establish RADV behavior.

Original installed RADV and sources restored; both Linux/Windows build outputs
remain diagnostic and must not be promoted. Logs saved to host, filesystems
synced, Xorg stopped. Some chroot binds unmounted; /dev remained busy due to the
hung shader. Existing USB identity checks passed, loader renamed to Windows
fallback, ordinary reboot requested. This report does not prove reboot success
or a clean image unmount. Linux wall-clock timestamps retain the known offset.
