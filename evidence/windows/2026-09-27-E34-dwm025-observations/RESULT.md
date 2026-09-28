# DWM025 retained interop observations - M649

KMD161/40F7916F, source6c93d74; runner8f1a3b9. Hosted UMD5C74BF98/ICD3508416F,
routerCE969883. Measured182.1490182s, GPU DWM11008. ETW3270 matched DMA pairs,
zero events/buffers lost, no unmatched/duplicate entries or pending pairs;
submission/completion IDs equal and no preemption. This is execution evidence,
not independent proof of every copy or displayed pixel.

Windows received one interop1 DRIVERCAPS reply (interop0 count0). First16 Blts
are retained and identical across repeated summaries: flags1,node0,umd0,system0,
DMA4096/private2184,offset0,list256; all pair snapshots valid. Source VA8DC000,
destination12000, distinct handles; both1920x1200,pitch7680,fmt21,size9216000.
One or two dirty rectangles. These are validated typed descriptors, not proof
of submitting-device residency through retirement. Total17 calls exceeds the
16-record capacity: call17's descriptor and timestamp were not retained.
CPU Blts17/skips0 and observation calls17 stay unchanged to the final summary.
GPU Present gate0/calls0 throughout: BGP1 still not exercised.

24 producer heartbeat samples show paint counts increasing from60/60/13 to
1650/1650/331 (red/blue/moving). Frequency10MHz, precise UTC samples bracketed
by QPC reads. Trace start FILETIME134349754927525031. For each anchor, derive
observation time in trace microseconds as:
(utc_filetime - trace_start)/10 + (observation_qpc - anchor_qpc)*1e6/frequency.
Using before/after brackets and the envelope across observed anchors places
all16 Blts between3.8261892 and4.5488429s. First DWM11008 Present is5.755840s.
The envelope includes observed calibration variation, not a bound on all
unobserved clock drift. Timing does not cover unretained call17.

Image result is mixed, not acceptance. Final GPU/GDI static ROIs both match
8000 expected pixels, but baseline.bmp contains neither expected color region:
all8000 baseline comparisons fail. Thus the planned CPU positive image control
failed; the initial fixed2-second delay did not establish visual readiness.
Eight retained dynamic BMP/PNG shapes pass; final screen.png passes. Final
gpu.bmp shape fails:26676 cyan pixels,bbox696,157,920,288,fill0.9090785169,
7 small-component pixels. A non-atomic primary capture remains a hypothesis.
The producer's deadline is270s and its task was running at the last sample;
window destruction at a180s deadline is not an established explanation.
No owner visual verdict. Do not infer a clean desktop from runner success=true.

Rollback at09:41:30.4199114Z DWM11008->CPU10856. Runner terminal09:41:32.4655792Z;
cleanup verifies baseline DLL hashes, registry and latched interop0/GPU Present0,
then removes all3 tasks. Independent09:42:56.8589549Z closure: exact161,
health15/guard0,1000MHz/VID116,66.875C,no test processes,boot retained.
Private receipts.zip12081017 bytes and gpu.etl129100800 bytes retained under
scratch/g0-hosted/dwm025; images and raw process inventory stay private.

Next: retain actual observed Blt contract for GPU admission and residency work.
Before another image comparison, wait for producer paint/readiness and a valid
CPU baseline, with bounded failure and rollback. Fix the evidence gap instead
of silently replacing the failed baseline. Full G0 remains open.
