# Same-unit Linux graphics scratch reference

PROVENANCE: Mesa MIT; libdrm MIT; piglit MIT. Unit A, Linux6.18.52-0-lts,
Mesa05e6c9622e135ac2aeaf56ec70222642627e2162, ACO with LLVM disabled,
libdrm2.4.133 and piglit0cc014230c0701d7a61bf240009ddd0711462d4e.
Built in Debian13 glibc userland on the diagnostic USB kernel. packages.txt and
built-files.sha256 record dependencies and outputs. Installed original RADV
SHA2564a8fa25972093600e92c130848a60ccd332bafe5804fe8d09d6cfec785a73668.

At1000MHz, requested820mV/readback818mV:
- Compiler-eliminated array control: exit0, upstream pixel oracle pass.
- Exact M508 bounded2048-element shader, configured4x4 FBO: exit0, pixel pass.
- Unchanged upstream glsl-predication-on-large-array at default dimensions:
  exit0, pixel pass.
- Bounded shader with scratch logging only: exit0, pixel pass.

Waffle uses surfaceless EGL here versus WGL on Windows. This is offscreen
scratch/reference evidence, not presentation parity or full piglit acceptance.
GL4.6 renderer identifies Zink/RADV GFX1013. Dynamic-loader log witnesses
/opt/bc250 Mesa gallium/EGL/RADV; system GLVND dispatch libraries are also listed.
No GL version override. Cache disabled; exact commands are retained.

Both bounded final NIRs retain two scratch loads and two scratch stores, but are
not byte-identical: Linux has the32-bit descriptor-address high wordffff8000,
Windows1, plus instruction ordering/SSA-number differences (diff retained).
Do not claim identical executable shaders or a compiler-independent root cause.

Logging-only Linux scratch trace:403439616bytes,525312bytes/wave,768waves,
reported24CUs, SPI_TMPRING_SIZE00201300. These equal Windows M509 values.
Linux BO VAffff800100e00000, descriptor words00e00000/80008001. Windows M509 BO
VA2013c0000, descriptor013c0000/80000002. Both match their own allocation base.
The trace is CPU construction, not a read of hardware registers. The reported
CU count does not independently measure the enabled CU mask.

This establishes that real scratch access and the original failing test can
complete on this unit with the same upstream Mesa revision under Linux. It
narrows M508 investigation to differing runtime/setup/compiler inputs; it does
not yet identify the Windows defect. ACO hw_init_scratch addsffff0000 minus the
swizzle bit to descriptor high bits; assess its assumptions for low Windows VAs
against ISA and actual hardware state before changing it.

Tracing patch and binary identity retained; original installed RADV and source
restored afterward. The build-tree RADV output remains traced until rebuilt.
No test remains live. Xorg002 remains active, GPU64C at final test readback.
Linux wall clock is about2hours ahead of host UTC (linux-time-build.log was
observed beside host2026-09-26T10:30:54Z). Preserve raw logs, use uptime/ordering;
do not treat their wall-clock timestamps as synchronized. No clock was changed.
Full L37 comparison and M12.1-M13.1 acceptance remain open.
