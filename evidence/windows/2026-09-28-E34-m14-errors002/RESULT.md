# Errors002 passes native error propagation controls

Client0e6d887b/1FE2C408, shell d18fe04f, unchanged engineDC65/ICDC388, pinned by
stage-manifest.json. Relative to M753, the new client pre-creates a staging buffer
and tests Map READ after the injected WRITE_DISCARD allocation failure. The oracle
change follows the API contract and exact-runtime static analysis in
../2026-09-28-E34-m14-map-removal-offline/RESULT.md. M753 remains a failed historical trial.

CPU: all three independent devices pass; Map READ returns S_OK/non-null.
GPU: CreateBuffer returns E_OUTOFMEMORY/null and subsequent creation succeeds on a
healthy device. Map WRITE_DISCARD returns S_OK/non-null runtime fallback, while
GetDeviceRemovedReason returns0x887A0005. Subsequent staging Map READ returns
0x887A0005/null; removal stays sticky after Flush. UpdateSubresource also produces
sticky0x887A0005. Selected debug lines confirm injected allocation failures and
legal DDI error codes. The client does not dereference the write-only fallback pointer.

Unused per-case JSON read_hr/recovery fields retain the E_FAIL initialization value;
only fields exercised by that case are evaluated. They are not additional API failures.

Supervisor passes51.1825089s, CPU171 baseline restored, postflight and child-tree closure
verified. Cleanup followed by Inspect confirms task Missing. No DWM/OS restart,
no driver update. Raw records: scratch/m14/errors002-ops. Lab free.

This establishes these three injected native-runtime error paths, not all OOM paths,
physical-memory exhaustion, kernel reset, residency, concurrency, D3D12 or FL12_1.
