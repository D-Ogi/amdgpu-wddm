# M327 - Scheduler access checkpoints

M326 last07103 startup snapshot ends at scheduler. Local AMD MIT reference
Linux gfx_v10_0.c default gfx_v10_0_kiq_setting reads RLC_CP_SCHEDULERS,
preserves upper24bits, inserts KIQ me/pipe/queue and writes with bit0x80 set.
The shim matches this sequence. Three callbacks now bracket that original read
and write: scheduler-read, scheduler-write, scheduler-done. No extra MMIO or
changed write order/value. Outer scheduler checkpoint remains.

Traced replay checks13 ordered callbacks, exact354+35Linux writes with24address
exceptions and four discriminating negative controls. Default replay also passes;
WDK26100 build and scoped diff check pass. Source/reference/build hashes included.
Development SYS5CC6F29876181B59E4C631409C165AE60F1767F0C5254649C88E69255E9CE3FE,
revision103 development only, NOT installed over published07103 package.

This is diagnostic preparation, not a hardware fix or exact hang-instruction
proof. Each callback includes synchronous persistent log I/O; a missing next
snapshot alone cannot distinguish an MMIO stall from a persistence failure.
No new hardware trial/restart or register probe performed in this step.
