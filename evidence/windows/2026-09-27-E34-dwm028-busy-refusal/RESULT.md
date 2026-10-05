# M659 - DWM028 GPU Present admission succeeded, busy rejection caused recovery

Unit A, 2026-09-27. Exact KMD163 source2c3849a, SYSB602D9A0 (full identity M657), runnerd175c87/manifest160207f. Trial ended in VIDEO_TDR_FAILURE0x116, not a completed180-second run. No owner visual verdict, no promotion, G0 open.

## Observations

Offline analysis uses the exact build PDB. The dump retained658 log records, zero lost; decoded ring is included unchanged. Full/minidumps and binary ring remain private with hashes in private-artifacts.json. Debugger outputs include only driver state and disassembly; no memory dump is published.

BGP1 private-span admission now succeeds. Dump counters:23 Present calls/records,6 rotates,18 hardware submissions and18 completions,0 hardware timeouts/refusals,0 admission rejections,2 submission failures. The submit fence slot is0x41 (65), matching the last submitted sequence. Thus all18 dispatched GPU copies completed. This proves command execution/retirement, not correct copied content.

The ring records seq65 submitted while seq64 was complete, followed immediately by fence19 NOT dispatched. Exact source gfx.c GfxSubmitReadyAccess requires SubmitInFlight==0. BGP1 wddm.c4339 uses only that readiness predicate, unlike the UMD ready-or-busy bounded submission path. A pending job therefore makes BGP1 close the node as a submission failure. The first closure sets RefusalPending[0]/WatchdogFaulted[0]; both are1 in the dump, node1 flags0. The second failed submission does not repeat the once-only failure log. No watchdog timeout or non-busy hardware refusal occurred. This evidence identifies idle-only admission as the failure mechanism; changing interrupt control is not supported by the evidence.

The fence read uses the exact disassembly in kernel-objects007.txt: the CPU fence pointer is at SubmitAdev+0x5250 and slot10 is at+0x50. kernel-fence008.txt reads0x41. These are structure/array offsets, not MMIO register addresses. The GFX and WDDM pointers were obtained from the context/device PDB layouts. Driver ResetFromTimeout is the recovery reporter, not evidence that it initiated the failure.

## Recovery

Automatic reboot at10:58:57.500Z terminated the trial. Newly copied baseline backups failed hashes; older original copies passed and were used for durable repair. Standard rollback then stalled in its pre-disable log query. Registry GPU Present/CDD interop were explicitly closed/flushed before adapter recovery. Guard budget2 caused Code43; after verified baseline DLLs/gates0, a recorded recovery budget reset allowed one adapter start. This reset was not health confirmation. The monitor subsequently confirmed real presentation progress: health15/guard0, CPU DWM2016,1000MHz/VID116,67.875C at11:18:36Z. All three trial tasks were removed at11:20:33Z. No manual OS reboot, power cycle or live kernel debugger was used.

The crash-specific closure records process termination by reboot; no normal collector/run completion receipts were fabricated. Initial recovery scripts encountered a parser error before execution, then a read-only RegistryKey SetValue failure after disabling the adapter; continuation used Set-ItemProperty and enabled it once. Raw attempts remain private. DWM028 artifacts remain immutable. Future trials need durable backup flushing and gate closure before potentially blocking diagnostics.

## Next validation

Use busy-aware bounded admission with real fence completion, regress back-to-back same-root jobs and capacity/root-switch waits, build an exact new artifact, then repeat a bounded trial. Preserve finite failure semantics and no fabricated completions. Separate BGP1 source/destination content proof is still required; final DWM captures alone cannot supply it.
