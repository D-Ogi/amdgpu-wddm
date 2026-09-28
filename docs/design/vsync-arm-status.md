# VSync arm failure propagation

ControlInterrupt(CRTC_VSYNC, TRUE) must not report success when the selected
hardware timing source cannot be armed. WddmVSyncArm now returns the exact
DcnVsyncEnable status, or DEVICE_NOT_READY for missing/stopping WDDM state.
ControlInterrupt publishes VSyncEnabled only on success and preserves its prior
state on failure. Already-armed and software-timer paths remain successful.
Disabling reporting still leaves timing armed as before; teardown owns disarming.
Visibility and low-IRQL flip callers retain their existing best-effort behavior.
This change does not alter FLIP_PENDING or EARLIEST_INUSE interpretation.

Validation: run_display_visibility.ps1 extracts the actual helper and DDI bodies.
881 checks pass, including sync timeout, MMIO read/write refusal, retry, repeated
enable without writes, disabling reporting, stop admission, software timer,
missing WDDM state and unsupported interrupt types. The first changed-code run
failed one expectation because the MMIO mock returns DEVICE_NOT_READY rather
than UNSUCCESSFUL; the assertion now checks the mock's exact status.

Full main-tree build and quality gates pass at scratch/g0-hosted/vsync-arm-build001.
This uncommitted-source build is source eligible=False and is not a deployable
exact candidate. Lab remains166. No claim that this fixes the measured TDR: no
arm failure during those incidents has been established. A clean isolated build
and runtime validation are required before promotion.