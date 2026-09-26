# driver/amdgpu-import/

AMD's IP-block code from Linux, **unmodified** (ADR 0002). Three earlier attempts at this driver
derived their register addresses and init sequences by hand and all three got them wrong; the
information is published by AMD under MIT inside the kernel, so we import it instead of reading it
and retyping it.

Files here are in one of two categories, and `PROVENANCE.md` says which:

- **compiled** - built against `driver/shim` and run. Most of the directory.
- **reference only** - present so that citations can be checked and data tables extracted
  mechanically, never compiled. These live in `reference/`, a subdirectory, so that
  `driver/kmd/build.ps1`'s non-recursive `*.c` glob over this directory can only ever pick up files
  that really compile. Currently `gfx_v10_0.c` and `sdma_v5_0.c`, which call about ninety
  kernel functions each that no shim can honestly provide (see `PROVENANCE.md` for the reasoning).
  A reference-only file is still byte-identical and still carries a `PROVENANCE.md` row; the only
  difference is that no build line names it.

## Rules

1. **Nothing in this directory is edited. Ever.** Not a whitespace fix, not a warning silenced, not
   a `#ifdef`. A file here is byte-identical to the kernel tree it came from, and that is the whole
   point: `git show <tag>:<path>` has to reproduce it. This holds for both categories.
2. If a file will not compile without a change, **stop and report the exact construct** instead of
   editing. The answer is almost always a missing definition in `driver/shim/include/amdgpu.h`, a
   forwarding header, or a warning turned off from the build line.
3. Warnings that come out of these files are turned off **on the compiler command line**, in the
   build script, with a comment saying which file and why (see `driver/shim/test/run.ps1`).
4. Every file is listed in `PROVENANCE.md` with its kernel path, tag, commit and the license notice
   the file itself carries - checked by reading the file, not assumed. `THIRD-PARTY.md` at the
   repository root points here.
5. Only MIT-licensed AMD files. GPL-only kernel parts cannot be imported; where the imports need a
   helper that lives in a GPL-2.0 kernel header (`lower_32_bits`, `min`, `ARRAY_SIZE`), the shim
   defines it from its documented behaviour instead of copying it.

## How to add a file

```
git -C $env:BC250_ROOT\ref\linux-src show v6.18:drivers/gpu/drm/amd/amdgpu/<file> > driver/amdgpu-import/<file>
```

Use `git show`, not a copy out of the working tree: that checkout has `core.autocrlf=true` and a
copy would carry CRLF. Then:

- read the top of the file and confirm the MIT notice yourself, add the row to `PROVENANCE.md`;
- put every register header it includes into `third_party/linux-amdgpu/` (flat) with a row in that
  directory's `PROVENANCE.md`, and add a forwarding header under `driver/shim/include/` if the
  include path is a `gc/...`-style one;
- add whatever the file needs to `driver/shim/include/amdgpu.h`, marking each definition as copied
  from amdgpu (naming the source file) or as ours;
- compile it, both user mode and with the WDK kernel flags: `pwsh driver\shim\test\run.ps1`.

## What is here now

**M4, GART (compiled).** `gfxhub_v2_0.c`, `mmhub_v2_0.c` (GART and VM setup of the two memory
hubs), their headers, `cyan_skillfish_reg_init.c` (the register base table of this exact SoC) and
`soc15_common.h` (the register access macros). Together they are the whole register-writing part of
amdgpu's GART bring-up; `driver/shim/test` replays it against unit A's own trace.

**M5 part A, PSP (compiled).** `psp_v11_0_8.c`, `psp_v11_0_8.h`, `psp_gfx_if.h`.

**M5 part B, GFX/KIQ/SDMA (compiled).** `v10_structs.h` (MQD layouts), `nvd.h` (GFX10 PM4 packets),
`clearstate_defs.h` and `clearstate_gfx10.h` (clear-state tables), `amdgpu_doorbell.h` (the NAVI10
doorbell index assignment).

**M5 part B (reference only).** `reference/gfx_v10_0.c`, `reference/sdma_v5_0.c`.
