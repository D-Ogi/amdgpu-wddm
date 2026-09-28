# M664 - DWM029 accepts/retires BGP1, aborts on hosted ICD startup witness

Unit A2026-09-27. Exact164933f383/SYS9B9B99D3, runnerd1d4ce6. First DWM5608 sample at11:57:25.2858886Z/elapsed5.2167571s includes routerCC6AAFAF and UMD5C74BF98 but lacks hosted ICD3508416F. Runner fails this required witness and rolls back before measured animation: measured_seconds0, successfalse. This is not a completed180-second trial or a visual pass.

## GPU Present result

Retained live samples show8 BGP1 calls/records/submits,6 rotations,0 refusals/rejections/submission failures; node0 hardware8 submitted/8 completed,0 timeouts/0 refused. Root flush returns0, actual completion logs retained. Pipeline samples overlap0. Corrected admission and real retirement are measured; a concurrent BGP1 burst is not established by this run, and contents are not proven by counters.

bgp1-observed-lines.txt selects unchanged lines from the private sample identified and hashed in bgp1-source.json. Other generations and full samples remain private. BC2S native010 is separate M663 evidence, not a substitute for BGP1 content validation.

## DWM startup remains unresolved

Router log has two successful OpenAdapter10_2 calls, two Zink initialization0..15 sequences, Caps130 and GetSupportedVersions, then private-instance teardown. No router CalcPrivateDeviceSize/CreateDevice witness appears. Initialization/enumeration and rollback can both affect module lifetime; the five-second snapshot alone does not distinguish adapter rejection from initialization that needed longer. Do not weaken the ICD/GPU requirement based on this absence.

ETW: zero lost events/buffers,13 MarkDeviceAsError events (Reason6 once,Reason14 twelve), no Reason19. Raw ETL/CSV retained privately with hashes. Event process-name counts are diagnostic only, not GPU attribution. Independent startup comparison is requested; no new root-cause conclusion yet.

## Recovery

Rollback restores baseline UMD8279AC7F/registered ICD93B1D1FD and switches to CPU3152 at11:57:34Z. No OS reboot or bugcheck observed; original boot retained. Collector13204/start11:57:16.6704586Z completes159 samples at12:00:17.1379545Z, no reader timeout. Archive and cleanup verify original collector absent; all029 tasks removed, including watchdog. Closure12:03:47Z exact164, CPU3152,health15/guard0,gates0,1000MHz/VID116,66.625C,no test process. Durable rollback succeeds on this normal failure path; no destructive crash durability claim.

The copied DWM startup log omits only its LUID-bearing route line; other selected outputs contain no private owner identifiers. No owner visual verdict. G0 remains open: correct BGP1/DWM images, GPU ownership and no full-frame CPU copies still need complete evidence.
