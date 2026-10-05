# DWM046 - runtime Present identity and independent count

Prepared, not run. Exact KMD166, UMDd7948d8e/92697AE5 validated by M704,
hosted ICD C0CE. Same default-UCRT logger and composition workload as045.
Render105s, checkpoint deadline130s, watchdog rollback140s, acceptance180s.
No permanent promotion. Existing rollback, STOP and thermal gates remain.

Require eight checkpoints, exact candidate modules, both selected-pixel controls,
loss-free DWM GPU DMA pairs and fence progress. Reconcile all runtime imports,
Present source/destination allocations, device ownership and wait/signal order.
Require successful independent Present-count snapshots at every checkpoint and
complete coverage of the presenting device at marker8; no missing-tail tolerance.
Analyze map/store counters and KMD software-copy counters over the measured interval.

Independent ETW validation must use the hosted context/device/adapter, not DWM
PID alone. Compare source/destination allocations and signal sequence with the
UMD records through the final checkpoint. Adapter identity needs evidence;
another DWM context must not be silently merged. Unknown coverage remains open.

Fresh preflight/STOP/thermal/overlay and exact manifest before one launch.
Restore8279/CF39/gates0 and remove terminal tasks; preserve failure evidence.
No retry under046. This trial alone does not waive sharing/lifecycle gates or
establish full-stack no-copy without examining all relevant writers.
