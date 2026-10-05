# M695 - DWM042 completes with attributed image uploads

Unit A, 2026-09-27. Runner c0bca08, exact166/1798984, UMD49A44067 and hosted
ICDC0CE5DCD. Trial125.9287s, render observation97.8975s, eight audit boundaries.
Automatic CPU restoration completed21:18:53Z; done21:18:55Z reports success.
No OS restart: boot20:27:47.5Z retained. All tasks and original collector terminal
before removal. Final21:20:10Z exact166,health15/guard0,gates0,1000MHz/VID116,67.4C.
Baseline UMD8279/registeredICDCF39 restored. No persistent GPU promotion.

Both final image checks pass40984 selected pixels, zero mismatches. This is the
specified region control, not a comparison of every desktop pixel. ETW has675
matched DWM-owned DMA start/stop pairs, no pending/unmatched/duplicate starts and
zero lost events/buffers. Sixteen sampled queue-fence observations are caught up;
last submitted/completed489. This is not a per-flip scanout completion proof.

Strict DDI and checkpoint analyzers pass:7416 scopes/maps attributed,10 internal
maps unmatched,1562 completed frontend copies in the whole log. Render phase has
1374 image-write maps:1253 UpdateSubresource and121 initial_data, all with completed
copy witnesses. Eight persistent buffers remain live. Their stores are unaccounted.

One render-phase UpdateSubresource (scope323/map840/resource838) copies1920x1200.
The map record has bind8, target2, format105, runtime0. Exact Mesa13e623af defines
bind8 as PIPE_BIND_SAMPLER_VIEW, without render-target/display-target/scanout bits.
Thus this is a measured whole-image input upload; it is NOT proof of a CPU copy
of the final composed desktop. Which DWM input it represents remains unknown.
Do not infer presentation-path copying merely from desktop-sized geometry.

107 latest OTG snapshots are preserved in otg-series.json. The run did not reproduce
DWM041's crash. Read intervals are actual begin-time deltas, not nominal cadence;
adapter transitions can invalidate cross-transition comparisons. No timeout-cause
conclusion is drawn from a healthy run. G0 remains open: persistent-pointer writes,
input-resource provenance and the final no-frame-copy requirement need evidence.

Private raw receipts, ETL, images and complete analyses remain under
scratch/g0-hosted/dwm042-ops. Public evidence selects technical fields, omitting
process/device identifiers and unrelated metadata. summary.json records archive
and ETL hashes. Full logs are not rewritten.