# DWM039 checkpoint trial (M688)

Runner bef4071, KMD164, UMDCC82, hosted ICD3508. GPU DWM7768.
Trial122.269s including restoration; render-loop reported90.247s.
Worker/watchdog exit0; restored CPU10576; all tasks removed.
894 matched DWM DMA pairs, no lost events/buffers or unmatched pairs.
19 queue-fence samples caught up, final666/666.
Exact selected pixels: baseline26,584; final primary/GDI32,472 each.
Eight requested markers,26,462 sequenced events,8,805 successful maps,
8,797 ends,eight live buffers,no pending map calls. Aggregate audit passes.
1,477 render-phase image-write maps, including479 at160x120 and472 at320x240.
No image map crosses phase boundaries. Writers and eight live buffers need
classification; map permissions do not measure stores. G0 remains open.

Evidence scripts: hosted-runtime/analyze-dwm-checkpoints.py at bef4071; independent image/fence/ETW analyzers retained in scratch/g0-hosted/dwm039-ops. Audit log contains only unchanged BC250 audit lines; unrelated output omitted. Raw images and ETL remain private. Pixel acceptance is selected regions and frozen client, not full-desktop animation.
