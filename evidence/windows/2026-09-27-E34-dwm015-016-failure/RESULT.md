# M585: DWM corruption persists after offscreen fixes

UMD67E4C9F5/hosted ICD3508416F on unchanged KMD152 passes M582/M584 controls
but fails desktop visual correctness. DWM015 runs180.108s as PID7804 and
DWM016 runs62.495s as PID4316. Neither unexpectedly restarts during its trial.
ETW has3635 and1267 matched DMA start/stop pairs respectively, matching
submission/completion IDs and zero lost events/buffers. GPU work is attributable
to DWM; those witnesses do not prove image correctness.

DWM015 primary dynamic-20.bmp captures severe stretched triangles, cyan
stripes and damaged window contents. Most other sparse captures look intact.
The owner did not watch015, then requested the one-minute016 demonstration
and reported many glitches. Three supplied photographs (IMG_2413,2418,2411)
independently show large stretched window triangles and dark strips. Originals
stay private; hashes bind them. The full desktop/room photographs are not
published. DWM016 dynamic-2.bmp also fails the diagnostic moving-shape check.

Static8000-pixel red/overlap ROIs pass in baseline, final primary and final GDI
captures for both trials. Their pass cannot override dynamic corruption.
The connected-component check passes015's CPU baseline and rejects its damaged
primary capture. It is a diagnostic shape check, not a full-desktop oracle.
Primary and GDI captures are not simultaneous. Diagnostic readbacks occurred;
no new complete CPU-frame-copy exclusion or performance claim is made here.

Both runs restored baseline UMD8279AC7F/registered ICD93B1D1FD, hash checked
by cleanup. CPU DWM8544 after015,6696 after016. All run/composition/watchdog
tasks terminal and removed. No candidate promotion. BD-043 and G0 remain open.
Next isolation must distinguish changing geometry/buffer contents and ordering
from presentation/scanout; passing fixed or bounded offscreen controls alone
does not settle that distinction. Parent b28d90d; unit A,2026-09-27.
