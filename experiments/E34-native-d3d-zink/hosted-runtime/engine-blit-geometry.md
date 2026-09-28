# GPU Blt geometry contract

The engine-present path needs allocation-relative byte ranges before it emits
commands. driver/kmd/blit_plan.c implements that geometry step. It is not yet
called by DxgkDdiPresent and does not change the deployed KMD or advertise interop.

Bc250PlanBlt accepts explicit source and destination surfaces, equal-size source
and destination rectangles, and a destination dirty rectangle. A null dirty
rectangle means the full destination rectangle. It intersects dirty geometry
with the destination rectangle, preserves the original source translation and
returns first byte offsets, independent pitches, row byte count and row count.
It never substitutes the primary or POST framebuffer.

The initial contract is linear32-bit BGRA or RGBA with identical layouts on both
sides. Scaling and format conversion are rejected. The adapter must map actual
DDI formats/layouts to these tokens after validating the allocation metadata.
Every rectangle, including dirty geometry before intersection, must lie in its
allocation. Invalid plans and empty intersections clear the output. Extents and
last-row bounds use64-bit arithmetic, avoiding truncated offsets and addition
wraparound. The declared surface extent includes the entire last row's padding.

Before this can submit work, the caller still must:

- Resolve the destination named by dxgkrnl, with no scanout fallback.
- Validate every rectangle before emitting any part of a present, and implement
  multipass continuation without losing dirty rectangles or partially accepting
  a later invalid rectangle.
- Resolve backed memory/GPU addresses while maintaining residency and allocation
  lifetime; reject overlapping backing ranges or implement a measured safe copy.
- Emit an appropriate GFX-node copy and cache ordering, completing only the
  presenting node's submitted fence after that work retires.
- Handle CPU staging and GPU texture allocations, UMD and non-UMD contexts,
  and validate these obligations before enabling DriverSupportsCddDwmInterop.

The host test runs4290 dirty-rectangle combinations through an independent pixel
oracle with different source/destination pitch and a sentinel in every padding
byte and untouched pixel. Additional cases cover invalid geometry, undersized
allocation, conversion/scaling rejection, null inputs and offsets above32 bits.
The runner also compiles the same source with kernel flags. tools/quality/quick.ps1
now requires this gate. This proves geometry for the tested cases, not engine
packets, cache coherence, fence behavior or a working CDD/DWM presentation path.
