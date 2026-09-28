# Audit client005 - descriptor writers during texture draws

Prepared, not run. Same exact166, UMD3484e1f4/B514FF61 and hosted ICD C0CE as004.
Only workload and isolated run/router paths change. CPU DWM retained. Existing
45-second process deadline and independent watchdog/restoration remain.

Hypothesis:18 texture draws produce completed get_descriptor witnesses within
live buffer maps, with bounds and checkpoint counts reconciled. Alternating
1x1 resources exercises descriptor changes; direct black-clear/draw/readbacks
must give magenta and green across all76800 pixels. Deliberate UpdateSubresource
uploads and final yellow readback remain as the known CPU-copy control.

Require all three pixel checks,18 draws,8 markers, exact loaded modules,
nonzero descriptor spans, strict lifetime/store/DDI parsers, no pending or
invalid stores, and measured record rate. Startup writes stay outside interval
sums. Failure cannot pass as absence of CPU stores. This is not G0 or a timing
benchmark. Restore baseline8279/CF39, retain CPU DWM/boot and remove terminal
tasks. Fresh preflight and STOP/thermal check precede one launch only.
