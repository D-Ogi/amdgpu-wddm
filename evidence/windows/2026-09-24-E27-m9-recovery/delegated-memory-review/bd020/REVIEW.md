# BD-020 review: GOP scratch and PSP pages

2026-09-24. No driver change, firmware execution, firmware write or lab operation.
Recommendation: NOT-A-BUG for the existing alleged GOP/PSP overlap.

## The reported 128 KiB extent is not supported by its own source

firmware/bios/analysis/gop-dcn/GOP-DCN.md section2.3 says 128 KiB, but shows a
0x20 KiB length. Directly re-read its underlying decompilation:
firmware/bios/analysis/gop-dcn/ghidra-out/decomp.c:2754-2764,
FUN_00004d28 sets the usage table's firmware length to0x20 and start to
VRAM_size_in_MiB*0x400-0x20 when the table's length is zero. The named AMD
vram_usagebyfirmware_v2_1 fields are in KiB, so this describes the final32KiB.
It does not describe128KiB. This is static interpretation, not a live allocation
measurement, and depends on the supplied P3/P5 GOP image matching the boot path.

Our psp.c:40-41 reserves three4KiB pages starting at end-0x19000. Their half-open
extent is [end-0x19000,end-0x16000). The interpreted GOP extent is
[end-0x8000,end). They do not overlap. The GART table starts at end-2MiB and has
1MiB size; its scratch page follows that table, also outside the final32KiB.
No additional blanket128KiB reservation or assertion is justified by this report.

## Actual own-unit VBIOS data, read offline

The bounded parser read-usage.py derives the ROM master-data offset and table
index from original AMD atomfirmware.h declarations; ATOM_ROM_TABLE_PTR comes
from original atom.h. No MMIO addresses or image execution.

usage.json records three images and their hashes:
- Own unitA Linux debugfs VBIOS and separately extracted unitA VFCT VBIOS are
  byte-identical, SHA256 bf4f1e3c77e27700b226265161cf7d0f92773dc2269c572aa47826c9f2830f22.
- DonorP3 embedded image SHA256 89ec00c3a91d515f545ea9ba7923e0c9b62080bdc8f27732f5dfedfa489687d1.
- All three have vram_usagebyfirmware revision2.1, start0KiB, firmware0KiB,
  driver0KiB. The own-unit blobs stay under firmware; no blob copied into repo.

This independently checks existing own-unit artifacts; it is not a new live
capture and does not establish which temporary GOP copy was patched in RAM.

## Original Linux behavior

Reference: ref/linux-src commit7d0a66e4bb9081d75c82ec4957c50034cb0ea449.
- amdgpu_atomfirmware.c:105-131, allocate_fb_v2_1: firmware VRAM reservation is
  entered only for the SR-IOV reservation operation flag. Zero table fields do
  not ask for such a reservation.
- amdgpu_atomfirmware.c:173-211, allocate_fb_scratch: zero driver size falls back
  to20KiB allocated using kzalloc, i.e. kernel system memory, not the GOP's VRAM
  workspace.
- amdgpu_ttm.c:1669-1685: fw_reserve_vram_init returns without a BO when the
  requested firmware VRAM size is zero. There is no unconditional128KiB tail
  reserve through this path.

PROVENANCE: AMD files above carry MIT notices within the GPL-2.0 kernel source;
read-only reference, no copied upstream code.

## Lifetime and limits

KMD is a Windows runtime miniport; its PnP/startup PSP allocation follows OS boot,
not a pre-ExitBootServices path. Current KMD/shim sources contain no ATOM command
interpreter or GOP invocation that reuses the GOP software workspace. Existing
M34 records successful PSP ring/TMR/firmware load using these addresses, and M439
records full WDDM workload/display controls. Neither proves absence of every
possible SMM/firmware consumer, and no such broad claim is needed to dismiss the
specific static overlap allegation.

No new unitA witness is required to reject the128KiB arithmetic/premise. A future
firmware change or introduction of GOP/ATOM execution needs its own workspace
ownership review; own-unit SPI extraction remains the existing separate L35 item.
The public backlog should retain the scoped reasoning rather than claim the
original report's blanket 'nothing survives ExitBootServices' assertion was measured.

## Proposed append-only comment

- 2026-09-24 Codex: NOT-A-BUG for the reported overlap. GOP report's128KiB text
  conflicts with its underlying FUN_00004d28:0x20KiB is32KiB. PSP pages occupy
  end-0x19000..end-0x16000, outside end-0x8000..end. Offline parsing of own unitA
  debugfs/VFCT VBIOS confirms vram_usagebyfirmware2.1 start/fw/driver all0. Linux
  v6.18 allocates20KiB kernel scratch for zero driver size and no firmware VRAM BO
  for size0. Current KMD has no GOP/ATOM execution path. No hardware, firmware or
  reservation change; future firmware/ATOM workspace ownership remains separate.
  Review/parser/results: scratch/m9/bd020/REVIEW.md, read-usage.py, usage.json.
