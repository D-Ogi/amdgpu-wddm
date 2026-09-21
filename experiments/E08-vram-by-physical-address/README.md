# E08: the VRAM carve-out is reachable by system physical address under Windows

State: **run 001 done, H1-H6 hold, one finding about BAR0** (2026-09-21). Evidence: `evidence/windows/2026-09-21-E08-run-001/`.

## Why

M4 needs a GART page table that the GPU can walk. amdgpu keeps it in VRAM (on unit A at MC `0xF5FFE00000`, the
top 2 MB, E03 dmesg) and, this being an APU, reaches VRAM through its system physical address:
`GCMC_VM_FB_OFFSET << 24`, which the E03 trace and the e820 map of the same boot both put at `0x270000000`
(8 GB, `reserved`). BAR0 shows only the first 256 MB. Before the miniport writes a page table through that
physical address, this experiment proves under Windows that the address is right, with no GPU register written.

A host-side comparison already shows the starting point is the same as under Linux: all 147 `GCMC_VM_*`,
`GCVM_*`, `MMMC_VM_*`, `MMVM_*` location, aperture, context 0 and L2 registers read the same in the Windows
sweeps (E02, E07) as in the firmware state amdgpu found (E03 sweep before the driver).

Driver: `bc250kmd` 0.4.2, new file `vram.c`. Gates `EnableVram` and `EnableVramWrite`, both default 0 and closed by
every install. The carve-out is computed from `GCMC_VM_FB_OFFSET`, `GCMC_VM_FB_LOCATION_BASE/TOP` (read through
the table-checked `MmioRead`), refused if it overlaps any range of `MmGetPhysicalMemoryRanges` (memory Windows
owns), BAR0 identified from PCI config space plus the translated resource list. Every access maps one page and
unmaps it. Reads are accepted inside BAR0's length and in the top 2 MB; writes only in one page at VRAM offset
`0x0F000000` (inside BAR0's window, 240 MB above the firmware framebuffer), and only if the driver can place
the firmware framebuffer and it does not overlap that page.

## Hypotheses

- H1. Closed gates: `memory` reports the framebuffer address but no VRAM; `vread` is refused with
  `STATUS_DEVICE_NOT_READY`. Display unaffected (stage 61).
- H2. `EnableMmio = 1`, `EnableVram = 1`: the driver reports VRAM at `0x270000000`, length `0x200000000`, MC base
  `0xF400000000`, BAR0 at the address Windows assigned with length `0x10000000`. The firmware framebuffer lies
  in one of the two views at VRAM offset 0 or close to it.
- H3. Same memory, without writing: words of the visible framebuffer read through `phys` equal the words read
  through `bar0` at the same VRAM offsets (pixels are non-trivial data, so equality is not an accident of
  zeros). Compared at three offsets inside the framebuffer.
- H4. The top 2 MB of VRAM (`0x1FFE00000` and up) are readable through `phys` without fault. Content unknown
  (a cold boot leaves whatever DRAM held); reads outside the accepted windows are refused with
  `STATUS_ACCESS_DENIED`.
- H5. `EnableVramWrite = 1`: `0x0BC25008` and its complement written through `phys` at two words of the test page
  read back through `bar0`, and values written through `bar0` read back through `phys`. A write outside the
  test page is refused with `STATUS_ACCESS_DENIED`. The original contents are restored at the end.
  Known uncertainty: BAR0 accesses go through the GPU's HDP block, which has a read cache that amdgpu flushes
  explicitly (`amdgpu_device_flush_hdp`), and we do not write the flush register here. A stale word through
  `bar0` right after a `phys` write would therefore be a finding about HDP, not a refutation of the address;
  the `phys` -> `phys` read-back and H3 carry the address claim.
- H6. A GC register sweep after the memory accesses differs from the sweep before them only by the noise set:
  touching the carve-out from the CPU changes no GPU register. Display alive throughout.

What would refute: `phys` and `bar0` disagreeing on framebuffer pixels (the carve-out is not where
`FB_OFFSET` says, or BAR0 does not start at VRAM offset 0), a machine check or hang on the first `phys` read
(the carve-out is not CPU-accessible under Windows the way it is under Linux), the carve-out overlapping OS
memory.

## Safety

No GPU register is written. CPU reads of DRAM have no side effects. The only writes go to one page of VRAM
that neither the firmware's scan-out nor Windows uses, and the driver refuses them if it cannot prove that.
If the first physical read hangs the machine, the start budget and the closed-by-default gates give back the
plain M3 display driver at the next boot.

## Procedure

`e08_target.ps1` on the target, one phase per call, logs under `C:\BC250\e08\out`: `install` -> `probe` (H1) ->
`gate -Mmio 1 -Vram 1` -> `probe` (H2-H4) -> `gate ... -VramWrite 1` -> sweep x2 -> `write` (H5) -> sweep (H6) ->
`gate` all 0.

## Result (run 001)

| | Outcome |
|---|---|
| H1 closed gates | **holds**: framebuffer reported at `0xC0000000 + 0x8CA000` (the start of BAR0), no VRAM, every memory command refused with `STATUS_DEVICE_NOT_READY` |
| H2 location | **holds**: VRAM at `0x270000000`, length `0x200000000`, MC base `0xF400000000`; the range does not overlap any memory Windows owns |
| H3 same memory | **holds**: 3 x 64 words of the visible framebuffer (wallpaper pixels, all nonzero, one block with 64 different values) and 16 words of the test page are identical through `phys` and `bar0` |
| H4 top of VRAM | **holds**: the top 2 MB read through `phys` without fault (content: leftovers); reads outside the windows refused |
| H5 cross-path writes | **holds**: `phys` write -> `bar0` read and `bar0` write -> `phys` read agree for both words; writes outside the test page refused; original values restored |
| H6 no register moved | **holds**: the GC sweep after the accesses differs from the control only inside the 6-register noise set; display alive throughout (stage 61, presents counting) |

Finding (the uncertainty named under H5, now measured): after the restore through `phys`, reads through
`bar0` returned the values last written through `bar0` for at least 45 s, while `phys` showed the restored ones;
a few reads later `bar0` agreed again. Both mappings are `PAGE_NOCACHE`, so this is not the CPU cache: the
BAR0 path keeps its own copy of words it has recently handled and does not notice a change made behind it.
amdgpu flushes HDP explicitly for this reason and does not use the BAR on APUs at all. Consequence for the
driver: VRAM is accessed by physical address only; BAR0 stays what the firmware framebuffer hand-over needs
and nothing more (facts M31, M32).

One script bug on the way, without hardware access: the first `probe` ran with a helper named `Cli`, which
PowerShell resolves to its built-in alias of `Clear-Item`; renamed to `Run-Cli`, log kept.
