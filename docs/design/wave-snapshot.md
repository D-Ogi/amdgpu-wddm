# GPU wave snapshots

The tool reads live wave registers and saves them before a recovery kill.
It does not stop a wave or change its state. Each record contains sequential
samples. Equal identity rechecks do not make the record atomic.

## Source and scope

PROVENANCE: Linux amdgpu, MIT, revision `7d0a66e4bb9081d75c82ec4957c50034cb0ea449`.
UMR, MIT, revision `a3d8666f936144ef497d93ea366070464b3a4d5b`.

The leaf read and register order come from `wave_read_ind` and
`gfx_v10_0_read_wave_data` in the imported `reference/gfx_v10_0.c`.
The bank selector follows the GFX10 path in
[UMR scan_waves.c](https://gitlab.freedesktop.org/tomstdenis/umr/-/blob/a3d8666f936144ef497d93ea366070464b3a4d5b/src/lib/scan_waves.c).
All register offsets and field masks come from the imported AMD headers.

This version admits GC 10.1.3 with two shader engines and two arrays per engine.
Each array has five physical WGP positions. Each WGP has four SIMD positions
and each SIMD has 20 wave slots. The count agrees with LLVM 23.1.2
`AMDGPUBaseInfo::getMaxWavesPerEU`. These limits are not the GC 10.3 limits.
The reader combines the hardware and user inactive-WGP masks for each array.
It preserves holes in those masks. It reports WGP, not an inferred CU number.

## Access and lifetime

The live escape requires an administrator, ABI 1, operation 0 and the exact size.
It accepts only `HardwareAccess | NoAdapterSynchronization`.
There is no flag fallback. The
[WDK escape flags](https://learn.microsoft.com/windows-hardware/drivers/ddi/d3dukmdt/ns-d3dukmdt-_d3dddi_escapeflags)
do not specify how this pair bypasses a scheduler wait.
The known-spin control must check delivery while a wave remains active.

The escape captures its inputs once. It returns a driver-owned, zeroed buffer.
It accepts no register offset, pointer or write value from the caller.
The read runs at PASSIVE_LEVEL. It takes `GfxPagingLock` shared, then `GartLock`.
GFX teardown takes the first lock exclusively. Other shim sequences and the
recovery kill hold the second lock. The power-phase gate rejects a transition.

The local sequence uses the generated GFX access table. Its write callback
admits only `GRBM_GFX_INDEX` and `SQ_IND_INDEX`. It cannot write `SQ_CMD`
or `SQ_IND_DATA`. A failed read cannot poison the submission sequence.
Cleanup restores broadcast first, then the saved indirect index, with readback.
It uses checked MMIO directly so a stopped sequence cannot suppress cleanup.
The original backend is restored before the locks are released.

The register loop has a 250 ms budget. Lock admission and registry flush are
outside that budget. Truncation, a read error or a timeout clears completion.
Available complete records remain in a partial result. An all-ones status or
identity read is an error. A failed or empty scan is not proof of GPU idle.

## Saved recovery record

`DxgkDdiResetEngine` writes its pending record, captures waves, then issues its
first recovery kill. The reset admission remains closed during these steps.
The snapshot does not freeze other GPU engines or external clients.

The snapshot is a `REG_BINARY` value named `Snapshot` under the service key's
`Parameters/HangRecovery/Waves` child key. The child has a protected DACL:
SYSTEM can write, administrators can read. Existing recovery counters keep
their permissions. The writer sets the DACL before it writes the value.
It flushes the key before recovery continues, after releasing the GPU locks.

The header contains capture times, driver ABI version, sequence, OS fence,
VMID, result and flags. Allocation failure writes a new failure header.
A persistence failure is logged and does not stop recovery. In that case an
older value can remain: compare the header to the current recovery record.
The record covers the soft-kill path. It is not a snapshot of every TDR path.

## Validation

Host tests inject register failures and deadline expiry across the read path.
They cover active-mask holes, every slot, full occupancy and identity changes.
Decoder tests retain raw values and reject damaged lengths, counts and selectors.
The CLI tests replace adapter, token and registry calls. They do not touch a GPU.

On the target, first run `bc250kmd_cli waves --expect-idle` with no active GPU job.
Then run a known spin shader and pass its GPU code interval to
`bc250kmd_cli waves --expect-pc 0xSTART 0xEND`. Use a bounded probe with a stop path.
The second control must see a stable wave in that interval. Zero waves fails.
Other desktop waves are allowed. Preserve raw output and the code interval.
Only after both controls pass can a stuck-wave capture support a diagnosis.

For recovery, compare `waves saved` with the current sequence, fence and time.
Check that its write precedes the first `SQ_CMD` kill. A host test cannot prove this
hardware result. Lab validation is still required before release.
