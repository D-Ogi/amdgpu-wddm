# M701 - DWM045 completes with default stderr and a reconciled store census

Exact166 (1798984), UMD3484e1f4/B514FF61, hosted ICD C0CE; runner656bdea.
Compared with044, the experimental logger change removes explicit _IONBF.
The same4s marker deadline is retained. All8 markers passed in25.3-118.8ms;
first58.6ms. This run supports the logging-cost explanation for the previous
late producer. It does not measure the production driver's performance.

The trial completed127.947574s with101.836132s sampled runtime. Both GPU BMP and
OS PNG controls passed34600 selected pixels with zero mismatches. These are
selected composition regions, not a pixel-perfect assertion about every desktop
pixel or every frame. ETW has2186 matching DWM6764 DMA start/stop pairs with
no loss, unmatched starts/stops or duplicate starts. All41 sampled queue-fence
points were caught up; last submitted/completed1988, Present1980. Queue completion
is not a hardware scanout timestamp.

Through marker8, strict sequences/counters reconcile18016 successful maps,
18008 ends,8 live persistent maps and34353 begun/ended store spans, no pending
stores and no outer-boundary-crossing stores. In the render-start/end window,
32166 descriptor writes expose1138496 destination-span bytes. This count includes
diagnostic capture subintervals and describes permitted GetDescriptorEXT spans,
not measured changed-byte counts. Seven persistent24000-byte maps have descriptor
writers. The1MiB staging resource16 receives exactly two instrumented startup
subdata spans:4 bytes at0 and16 bytes at4096; join by mapped_resource_id, not
by the caller's transfer-map ID. Neither is in the measured render window.

Outside capture subintervals,2230 image writes match UpdateSubresource copy
witnesses.2228 have bind8; two have bind0xa. No full-desktop-sized box appears.
The accessible buffer maps include16000-byte index and160000/240012-byte vertex
maps plus small constant buffers. Their footprints are not bounds on cumulative
application writes. Resource input/output roles, additional writer coverage and
the final presentation path still need to be audited before a no-CPU-frame-copy
claim. This result alone does not close G0.

CPU restored; all three tasks removed and original collector terminal. Final
23:03:16Z September27: exact166, baseline8279/CF39,health15,1000MHz/VID116,
67.25C,same OS boot, no trial processes. CPU DWM9552. No permanent promotion.
Full logs, ETL, screenshots and analysis remain in scratch/g0-hosted/dwm045-ops.
Export is reduced numeric results, marker lines and pixel-check receipts, with
SHA256 links to raw artifacts. No private configuration or raw screenshot export.

Analysis: analyze-ddi-origins.py and analyze-dwm-checkpoints.py on the exact log
and boundary receipts; xperf dumper and the preserved analyze-etw.py, with PID
read from done.json. The ETL was pulled from dwm-hosted045, not an earlier run.
Store-summary lifetime totals include startup; render-specific fields explicitly
use the selected boundaries. Avoid comparing total-prefix and interval counts.
