# M484: native Zero/PRT reads and discarded writes through CP DMA

Unit A, 2026-09-25. M12.1 groundwork; sparse Vulkan features remain disabled.

## Implementation and exact input
KMD149, SYS93ECB1BE4A4F33B1F59834B6BF5A006DE88CD9C778514472C0FEF5EDCBF3C221,
is frozen147 plus seven scoped files. The source pin and replay-checked patch
are in E33. Unrelated recovery work in the dirty tree is excluded.

The encoder imports the unchanged GFX10 PRT flag body from Linux amdgpu
gmc_v10_0_get_vm_pte: PRT, SYSTEM, SNOOPED and LOG, with VALID clear.
Null mappings have no backing address or read/write/execute permission bits,
matching RADV's null-BO mapping. Terminal directory entries also set PDE_PTE.
Zero is handled for both Windows Valid values; ordinary invalid entries retain
their prior behavior. ZeroInPteSupported is advertised for the four-level table.

Real WDK26100 DXGK_PTE host controls cover both Valid values, four levels and
protection combinations; existing physical translation and kernel-flag builds
pass with warnings as errors. These prove encoding, not all-level hardware
walk behavior. The149 API-only lab probe records the actual input:
level0, Valid1, flags0x3 -> AMD entry0x0088000000000006.
The Valid0 path was not needed in this observed request.

## Failed access-path control
KMD148 already had the Valid1 PRT encoder. Four bound COPY_DATA controls pass,
but its initial-hole COPY_DATA read still hangs. The corrected fence helper
rejects UINT64_MAX. Windows bugchecks0x116 at14:04:09Z and automatically boots
at14:04:39Z; SSH identity is rediscovered after DHCP changes. No AC cycle.
The bound result JSON was not durably preserved (NUL bytes after the crash);
the exact four comparisons and RESULT PASS remain in stdout. Do not treat that
damaged JSON as a verified native exit.

Offline CDB on the lab extracts only the driver log and driver state from the
839351534-byte kernel dump. The full dump remains on the lab under candidate07148;
the older explicitly approved147 dump remains private in workspace scratch.
No full dump or raw binary log ring is included here.

## Passing DMA_DATA controls
Mesa's cp_dma_supports_sparse refers to its DMA_DATA copy path. COPY_DATA is a
different packet and cannot be used as an equivalent PRT oracle without a
separate control. The probe's --dma path follows radv_cs_emit_cp_dma for GFX10,
using L2 source/destination selection and CP_SYNC.

On149, normal physical A/B reads and alias/rebind reads all match. Initial-hole
and post-unbind reads return exactly zero. A write of A into the hole retires;
the next hole read remains zero and physical A/B remain unchanged. Three runs
produce19exact content comparisons, all matching, plus ordinary fence values
and native exit0. Runs14:15:26Z-14:15:31Z and14:17:39Z-14:17:42Z.
This is native CP DMA evidence, not shader or Vulkan sparse acceptance.
It does not prove COPY_DATA is unsupported on Linux or on this hardware.

## Ordinary Vulkan regression and final state
Candidate Mesa05e6c962/ICD255534CA on149: normal-token explicit manifest,
vulkaninfo exit0, eight CPU-reference compute hashes match,600vkcube frames exit0.
Per-process witnesses identify the intended ICD and System32 loader.
Run14:18:47Z-14:19:16Z. System registration remains ICD9C40083C.

At14:21:20Z the same14:04:39Z boot is retained; flags15,
generation4426306309/epoch5, completed1414, UnconfirmedStarts0,
1000MHz/VID116,67.000C. DWM2052 still loads UMD8279AC7F.
No selected fault events since14:12Z; the current dump predates the149 tests.
149 installation itself was a warm PnP transition, not a Windows reboot.

## Remaining gates
Test shader vector/scalar loads, image residency, larger/higher-level holes,
rebind after writes, simultaneous resources and matched Linux controls.
Implement the HIGH/LOW scalar-address workaround and runtime privileged PTE
resolution before enabling Vulkan sparse capabilities. No performance claim.

controls.zip preserves probe inputs, native output, host output, exact scoped
KMD source and ordinary Vulkan witnesses. The final reusable probe also supports
--write-hole; earlier DMA binary input hashes are recorded separately.
Only UUID/LUID/report identifiers are redacted; manifest.json records hashes.
PROVENANCE: Linux amdgpu PRT definitions and Mesa packet/mapping logic are MIT.
