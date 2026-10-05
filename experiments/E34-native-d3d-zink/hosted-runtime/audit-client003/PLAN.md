# Audit client003: UpdateSubresource positive control

Prepared, not run. Same watchdog, baseline checks and restoration as client002;
new exact run/task paths003. UMD49A44067 and hostedICDC0CE5DCD remain pinned.
The new client uses --update-subresource:12 updates of a default shader-resource
texture followed by GPU CopyResource and Present. Each update supplies307200 bytes.
The two readbacks, GPU-clear interval and eight checkpoints remain unchanged.

Acceptance: both76800-pixel controls;12 frame-write maps between markers5/6;
all12 correlate to update_subresource scopes with copy_complete, no such scope
in the GPU-clear interval. App byte count is supplied payload, not measured bus
traffic. Initial-data copies and persistent stores remain separate coverage gaps.

Router/control /W4 /WX builds and PS5 parsing precede local packaging. Require
fresh lab preflight, explicit free slot, overlay announcement, launch once and
observe to terminal. Never restart on observer timeout. Restore baseline8279/CF39,
keep CPU DWM identity/boot, remove only terminal tasks and archive receipts.
No desktop GPU/no-copy conclusion follows from this small client alone.
