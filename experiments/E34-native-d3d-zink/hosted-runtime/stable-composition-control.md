# Stable CPU composition positive control

M672 retained a failed CPU moving-shape control while the static regions passed.
The old control moved during the banded primary readback; that is a possible
cause, not an established explanation of the failed image. Windows rounded
corners also invalidate an exact rectangular oracle (M674).

`stable-composition-control.cpp` provides a fresh control for subsequent trials.
It resolves its receipt directory from its executable path, refuses existing
heartbeat/animate/freeze markers, requests square corners, paints all three
windows, and acknowledges a successful DwmFlush before recording
`baseline_ready`. It remains stationary until an `animate` file appears.
Its existing 270-second lifetime and final `freeze` acknowledgement remain.
A failed corner request exits instead of silently masking pixels.

For a new trial, build with /O2 /MT /W4 /WX and link user32, gdi32 and dwmapi.
Stage the executable with `wait-stable-composition.ps1` and
`check-stable-composition.ps1` in a fresh trial directory. Call the waiter with
that directory and the composition task name before any GPU transition.
It retains every attempted capture and verifies unchanged geometry/zero frames
across readback. The checker requires the flush/paint witness and checks every
pixel of the actual cyan client, in addition to the existing 8,000 static pixels.
No corner mask, colour tolerance or area-ratio allowance is used.

Only after the hosted DWM startup witness succeeds should the trial create
`animate`. Do not use an old runner unchanged: it never creates this marker.
Retain the later heartbeat proving frames and geometry change, run the bounded
animation, then use the existing freeze handshake before final capture. Keep
independent primary/GDI captures, DMA/fence evidence and CPU-copy accounting.
This baseline check alone does not validate the dynamic desktop or exclude CPU
copies. Previous numbered runners and their measurements remain unchanged.

Host validation: `test-stable-composition.ps1 -OutputDirectory <fresh-directory>`
accepts a synthetic exact image (26,584 pixels), rejects one bad corner pixel
and a torn strip, and rejects moving, unflushed, empty and out-of-image witnesses.
The executable builds with warnings as errors. Neither a host GUI run nor a lab
runtime pass is claimed by these checks.
