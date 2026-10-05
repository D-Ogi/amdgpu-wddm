# M343 - Isolate RLC busy transition during teardown

M342 proves busy clear after GFX halt but set after whole teardown.107adds existing read-only RLC observations after GFX memory release, beforePSPunload, afterTMRdestroy, afterPSPringstop and afterGARTstop. These are diagnostic reads/logs; no MMIOwrites, recovery predicates, delays or reset sequence changes.

Build and packagecheck exact107. Install with gates closed. Current106 has already run GPU in this boot and shows busy set; do not use that state as an initial-start control. Preserve its evidence. For the hardware experiment prepare a clean baseline by graceful Windows shutdown, one authorized AC off/on after shutdown, then verified pinnedSSH/boot. USBloaderoff keeps Windows. No disk/firmware changes. Start107once, run unchanged64KiB GPU residency control, then close gates and restart device into display-only to capture teardown. No full warm retry is part of this experiment. Inspect persisted phases to select the next hypothesis.

Observe STOP, exactSYS/loadedrevision,1000MHz/820mV and temperature<85C. Original stream/process handles retained on interruption; no duplicate run after timeout. Existing plug recovery only if needed. All outcomes remain scoped; a busy transition does not by itself prove a firmware defect or reset solution.
