# M718 - DWM048 stops on instrumentation import failure

Runner179df1c, package manifest14B5EF0DA3DB8A64892FDE020077FAFADE1DE8C8AA8137FB7403192547F65F48. Exact169/AF715A56; hosted UMD92697AE5 and ICDC0CE. [Selected receipts](observations.json) are decoded without registry/provider graphs or private device identifiers. Raw receipts/ETL remain outside the repository in scratch/g0-hosted/dwm048-ops.

DWM9876 loaded the candidate and reached the first interval boundary. QueryInterruptTime imported from kernel32.dll raised EntryPointNotFoundException before the starting VSync snapshot. Runner exited1 with success=false after43.265s; reported rendering sample7.851s. No native-client or full-duration acceptance follows from this run. It is an instrumentation failure, not a successful GPU stability test.

Restoration completed, watchdog exit0/aborted=false and collector36 samples/no timeout. Tasks removed. Independent04:16:11Z preflight verifies CPU169/UMD8279/ICDCF39, health15 generation53569799498/epoch5,67.1C. Boot identity unchanged; queried Display4101/WER117/141 interval contains no suspect events. This does not substitute for the missing GPU VSync/TDR summaries.

Clock correction uses api-ms-win-core-realtime-l1-1-2.dll and performs an admission read before changing the desktop. A read-only lab control returned advancing interrupt-time values, delta114.1028ms around a100ms sleep. No new GPU test was run with that correction. The public [API metadata](https://github.com/MicrosoftDocs/sdk-api/blob/docs/sdk-api-src/content/realtimeapiset/nf-realtimeapiset-queryinterrupttime.md) lists that API set; the rendered requirements table names kernel32, which did not export the entry point in this measured process.

Next: a new immutable trial with clock admission, full VSync interval/ETW retirement checks, native client and image controls. G0 and M13 remain open.
