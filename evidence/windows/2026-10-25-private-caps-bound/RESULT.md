# Bound the private adapter-capability response

The KMD answered every UMDRIVERPRIVATE query of at least 1472 bytes with its BC25
format. AMD AGS 6.3 uses another private format. Its 608-byte query fails as too
small. Its 17248-byte retry previously received BC25 bytes. AGS reads offset 8 as
an adapter count. The BC25 value there is 1472, the structure size. AGS then clears
`count * 0x3a00 + 0x960` bytes of a stack object and faults.

The bound preserves all existing extents from 1472 through 1520 bytes. Smaller
queries still get STATUS_BUFFER_TOO_SMALL. Larger queries get
STATUS_INVALID_PARAMETER before any output write or telemetry access. This bound
is not caller authentication. A future larger format needs a versioned request.

## Host test, 2026-10-25

Base: main 30f6fc5412697abc30465bd13ba7639d7df44074, plus this change.
Command: `pwsh driver/kmd/test/run_firmware_metadata.ps1 -Root <workspace> -Out <output>`.
Result: firmware metadata/query: 3786 checks, 0 failures.

The test extracts the actual KMD query branch. It checks all partial optional
trailer extents and refusal of 1521, 1528, 3008, 17248, the AGS-derived clear size
and UINT_MAX. Refused queries leave the output unchanged and issue no SMU read.

Separate real-AGS controls on the ADL development branch reproduce the old-policy
fault and pass with the bound, in x64 and x86 with guarded callback allocations.
Those controls do not substitute for a run of the changed KMD on the lab.

## Lab diagnosis, before the fix

The original kit faults in AGS x64 at RVA 0x19aa0, with caller RVA 0x5ec3.
The host reproduction faults at RVA 0x19969 with the same caller. The AGS memset
entry at 0x19970 dispatches to REP STOSB at 0x19969 or the AVX store loop at
0x19aa0. In x86, the host faults at 0x6fec and the lab at 0x7030. Both are
paths in memset starting at 0x6fb0. No fixed-KMD lab result is claimed here.
