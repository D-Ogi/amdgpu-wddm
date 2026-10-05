# M679: DWM037 stable GPU composition

Runner source6587e49, exact KMD164/SYS9B9B99D3, UMD5C74BF98,
hosted ICD3508416F, routerE4A4EC76. Manifest retains full hashes.
Temporary registered capability ICD7A9970CA restored to CF3948D6.
Unit A, same OS boot10:58:57.500Z; no OS reboot or permanent promotion.

## Content and workload

CPU baseline before transition: all26,584 selected pixels exact, including
18,584 cyan pixels over the complete client. This follows the independent
CPU positive control M678. Hosted GPU DWM11524 starts14:40:30.6661302Z.
Normal health confirmation was recorded. Animation begins only after the
hosted startup witness and runs186.7280417 seconds, ending with an acknowledged
freeze. All36,200 selected final pixels are exact in primary and GDI captures:
8,000 red/overlap pixels and28,200 full cyan client pixels at
(616,157)-(816,298). Independent host RGB decoding agrees with the lab checker;
per-region hashes match. Four live primary/GDI pairs are retained privately as
diagnostics, not treated as atomic frame comparisons. No owner visual verdict.
No whole-desktop or all-animation-frame correctness claim is made.

## GPU ownership and completion

Loss-free ETW contains3,341 matched DWM-owned DMA start/stop pairs, no pending,
unmatched or duplicate starts; submission/completion ids agree and no pair is
preempted. All57 sampled UMD Present-fence records have submitted=completed;
last Present2940, fence2951. These are complementary ownership/progress witnesses,
not an assertion of one-to-one ETW/UMD fence ids or scanout timestamps.
KMD node0 summary progresses126/126 to2992/2992 with zero timeouts/refusals.
BGP1 counters remain9 submits,0 rejected,0 failed at both sampled endpoints;
these are startup copies, not a WSI control. CPU blit/skips/translated remain0.

## CPU-copy audit and limits

All154 cumulative map snapshots reconcile: zero overflow, unclassified calls,
unclassified bytes or unclassified persistent calls. Last sample9344 has
4,176 image maps (1,312,728,528 logical requested bytes),21,167 buffer maps
(1,856,627,536 bytes),8 persistent maps. Never sum cumulative samples.
Full1920x1200 WRITE|DISCARD mapping appears once by sample64 (9,216,000 bytes),
plus one86x508 partial write, and neither write count grows afterward. The
full-desktop READ bucket grows to5 calls (46,080,000 bytes). Four live GDI
captures plus one final GDI capture were requested; count agreement is consistent
with diagnostic readbacks but is not per-call timestamp/call-stack attribution.
Persistent buckets are included in the retained summary; no persistent image
mapping is recorded. Logical maps are not a CPU-store trace. The last sample
is not a guaranteed final teardown total. These results oppose a repeated
full-frame map/copy path in the sampled interval but do not alone close every
possible persistent/bypassing CPU-copy path. G0 remains open.

## Closure and evidence

Worker done14:43:58.7072456Z, success=true. Startup collector8592 terminal
14:43:28.4001962Z with159 samples and no reader timeout. Original process
identity checked terminal before archival. All three tasks removed; baseline
DLL hashes and both registry/latched gates verified. Fresh closure14:46:21.8813419Z
has health15, guard0,1000MHz/VID116,66.625C, no test processes, same boot.
Closure JSON identifies the restored CPU DWM.

Private archive: scratch/g0-hosted/dwm037 (ETL138,412,032 bytes,
receipts27,572,781 bytes). Full desktop images, raw ETW and stderr stay private;
hashes, region checks, parsed ownership pairs and selected audit-only log lines
are retained here. Audit selection preserves matching lines, omits other stderr.
No WSI handshake probe was run; M677's admission refusal remains unresolved.
