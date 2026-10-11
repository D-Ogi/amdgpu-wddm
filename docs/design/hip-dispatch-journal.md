# HIP dispatch journal

The HIP runtime records each dispatch before its batch reaches `D3DKMTSubmitCommand`.
The KMD keeps the records in its image, so dump analysis does not depend on the process page tables.
This journal provides diagnostic data. It does not fix a GPU fault or prove that a kernel executed correctly.

## Recorded data

Each record contains the full kernel symbol, entry and descriptor addresses, grid, workgroup and dynamic local-memory size.
It also contains the argument address and all packed bytes, including the optional AQL dispatch packet.
Each dispatch has a process-local identifier.

Only metadata entries of kind `GLOBAL_BUFFER` produce pointer bindings.
The runtime records their values and the containing allocation intervals from its allocation map.
A null or unresolved value has no interval. Scalars with pointer-like bits remain scalars.
These intervals describe runtime ownership, not GPU residency or access permission.

The KMD stores the caller identity, context cookie, batch identifier, timestamps, IB address and user fence.
Before submission, it attaches the OS fence. After hardware submission, it adds the GFX sequence, VMID and page-table root.
GuardLog contains a line with the batch identifier, PID, GFX sequence and OS fence.
The copied symbol and argument bytes are user-supplied diagnostics, not trusted kernel input.
The KMD never dereferences their addresses.

## Limits and lifetime

The shared ABI is `driver/shim/include/bc250_hip_journal.h`.
Command 35 uploads diagnostic data through a context escape.
The runtime uses `NoAdapterSynchronization` and leaves `HardwareAccess` clear.
The KMD admits the registered context only for its creating process and device.
The context cookie prevents reuse of a context address from granting access to an older record.
No escape returns another process's records.

| Item | Limit |
|---|---:|
| Upload, including its header | 128 KiB |
| Dispatches per upload | 32 |
| One record | 32 KiB |
| Symbol, including its terminator | 4096 bytes |
| Packed argument bytes | 16 KiB |
| Pointer bindings | 128 |
| Retained batch slots | 32 |

The runtime refuses a dispatch whose complete record exceeds a limit.
It never truncates the symbol, argument bytes or binding list.
The submission layer ends the batch when the next record would exceed an upload limit.
After recording starts, that context refuses the legacy unrecorded submission API.
Standalone layer-1 clients can still use that API on separate contexts.

A batch upload must succeed before submission. A missing journal implementation therefore refuses the work.
If a promised batch cannot be submitted, the runtime marks the device lost.
If the OS refuses submission after upload, the runtime requests cancellation of the unused record.

Pending records cannot be overwritten. Completed, cancelled or failed records can be reused.
The ring counts overwrites and refusals. If every slot remains pending, a new upload fails.
Ending a context cancels its unbound uploads. Records for work already submitted remain pinned until retirement or recovery.

A successful engine reset marks the aborted head as failed and permits replay of the remaining queued records.
Each replay records its new OS fence and GFX sequence. The attempt counter records reuse of the same dispatch data.
The fields describe the latest attempt, not a complete attempt history. A failed reset leaves pending records pinned.

The metadata lock protects publication and state changes. Payload copying occurs outside that lock while the slot remains pinned.
An odd generation marks an incomplete record. The completion DPC updates metadata only.

The hardware doorbell precedes the final sequence stamp.
A dump can therefore contain a bound record whose IB already reached hardware, but whose sequence field remains unset.
The IB and fence still identify that record. A bound state alone does not prove that execution never started.

## Offline use

Extract the exact `g_HipDispatchJournal` symbol from a retained dump with matching symbols.
Preserve the raw bytes before decoding them.
Use `compute/hip/tools/decode-dispatch-journal.py` on that extracted object.
The tool refuses an unknown layout and marks incomplete or malformed slots as invalid.
It does not access a live device or translate dump addresses.

Join valid records with the GuardLog batch identifier and the OS fence or GFX sequence.
Keep all dispatches of a faulting batch as candidates until further evidence identifies an instruction.
A batch record does not identify which dispatch raised the fault by itself.

## Validation and cost

The host gates cover record packing, submission order, refusal, cancellation, batch limits and dump decoding.
Compiled negative controls omit the argument tail or skip upload. The corresponding tests must fail.
The runtime gate also covers its existing thread, batch and disabled-build behavior.

Packing timings measure host work only. They exclude kernel entry, KMD copying, GPU backpressure and execution.
Measure those costs on the lab with the same workload, clocks and batching policy.
Keep the optional GPU-disabled build available until the original GPUVM fault is fixed.

## Contracts

The WDK declarations are from version 10.0.26100.0.
The local DDI reference revision is `7515063cea4c9e98db6a92986c5b4ddb0463fd16`.
The implementation follows the context handles and private buffer of
[DXGKARG_ESCAPE](https://learn.microsoft.com/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgkarg_escape)
and the call contract of
[DxgkDdiEscape](https://learn.microsoft.com/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkddi_escape).
Pointer classification uses the metadata value kinds described in
[LLVM AMDGPUUsage](https://github.com/llvm/llvm-project/blob/main/llvm/docs/AMDGPUUsage.rst).
The checked local LLVM source is version 23.1.2.
