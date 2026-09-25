# M477: close shared resources atomically
Unit A, 2026-09-25. M11 preparation; not the 24-hour acceptance.

## Reproduction and cause
On desktop UMD CB1480EC, each of control-01, control-03 and phase-01 retained
66 B2Ww pool objects / 13,728 bytes after two mixed cycles. Phase-01 measured
nine extra 208-byte objects after each console compute/inference application,
while the cube added none. The monitor's final five samples remained elevated.

The D3D UMD supplied only an allocation handle to pfnDeallocate2Cb even for shared
resources. Local Microsoft d3dumddi reference requires shared resources to close
atomically by runtime resource handle; Deallocate2 with zero flags has the same
semantics as Deallocate. The patch supplies hRTResource when present and keeps
the allocation-list path for device-owned allocations. It does not change KMD
object teardown. Source: Mesa f9a2d34a plus existing E26 patches and M474 padding.
Provenance: Mesa, MIT.

## Change and measured controls
Desktop UMD 8279AC7F6342CD0A31CB96531194CEF4A224A0D010BCEE4B549D9606D5E405EA
was built with LLVM 23.1.2 and loaded by DWM after device/DWM refresh.
KMD147 SYS5FCB554A and Vulkan ICD9C40083C are unchanged.
Windows boot remains 2026-09-25T05:58:31.500+02:00.

- Exact shared pixels both directions at 64x32, 1428x33, 1366x35, 1x1 and 67x65:
  zero differences; completion events pass. Separate 307200-pixel readback passes.
- Phase-02: both mixed cycles pass eight compute CPU/Linux hashes, exact greedy
  text for both AI models with complete GPU offload, and 600 cube frames per cycle.
  The nine SPIR-V input hashes match the Linux E14 artifacts.
- All measured project pool tags return exactly to their initial count and bytes
  in all five final snapshots. B2Ww: 41 objects / 22,034,288 bytes initially and
  finally; 62 / 22,038,656 at every active-worker checkpoint. The 21 temporary
  objects disappear with worker exit; there is no per-application growth.
- The same instrumentation distinguished old retained growth from new full
  release. This is a bounded regression result, not proof of no future leak.
- Temperature 66.875 to 67.125 C, 1000 MHz / VID116.
  Monitor found no new TDR/bugcheck event or kernel-dump metadata change.

## Artifacts and instrument corrections
control-03 and phase-01 are the old-UMD controls; phase-02 is the fixed UMD.
The phase worker takes extra quiescent samples. Its monitor cycles field counts
checkpoints (10); worker-result records the actual two workload cycles.
Raw stdout, loader stderr, inputs, telemetry and pool rows are preserved.
Only adapter LUID text in stderr was redacted. Private dumps/screenshots omitted.
Pool rows contain only project tags; absent transient tags have zero usage.

Earlier control-01 used PoolMon's appending snapshot path; its repeated rows
must be read using the last occurrence, never summed. control-02 stopped because
PowerShell converted null to an empty File.Replace backup path. The monitor
killed its own compute child. [NullString]::Value fixes this harness error.
Both corrections passed control-03 before phase isolation; old files remain in
scratch/m11, outside this evidence subset. No driver failure is inferred from
control-02.

The 24-hour run and one deliberate stuck queue remain required for M11.
