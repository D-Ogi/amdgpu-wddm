# Provenance

One unmodified file from libdrm: the gfx10 compute shader binaries that its amdgpu tests dispatch.
`driver/shim/bc250_dispatch.c` writes `bufferclear_cs_shader_gfx10[]` into GART memory and points
`COMPUTE_PGM_LO` at it. The words are not retyped anywhere in this tree, for the reason ADR 0002
gives: a shader binary is nine dwords of machine code and a single wrong nibble is an illegal
instruction, a hang, or a store to an address nobody chose.

Source checkout: `<BC250_ROOT>\ref\libdrm`, tag **libdrm-2.4.114** (an annotated tag, object
`6d4be7f9ba89babe9f8e574f0fee8b1141d0ea14`), commit `b9ca37b3134861048986b75896c0915cbf2e97f9`,
dated 2022-11-03.

| File | Path in libdrm | Bytes | Blob at the tag | `git hash-object --path=` here | Match |
|---|---|---|---|---|---|
| `shader_code_gfx10.h` | `tests/amdgpu/shader_code_gfx10.h` | 9325 | `4849bbc9bcf87d73efa3ad552cddc0448a2c764a` | `4849bbc9bcf87d73efa3ad552cddc0448a2c764a` | yes |

Verified in both directions on 2026-09-21:

```
git -C $env:BC250_ROOT\ref\libdrm rev-parse libdrm-2.4.114:tests/amdgpu/shader_code_gfx10.h
git hash-object --path=tests/amdgpu/shader_code_gfx10.h third_party\libdrm\shader_code_gfx10.h
```

## Who uses what

- `bufferclear_cs_shader_gfx10[]` (9 dwords) - the memset shader. Used by
  `driver/shim/bc250_dispatch.c` and, as the reference the stub compares against, by
  `driver/shim/test/backend_mem.c`.
- Everything else in the file - `bufferUVMtest_cs_shader_gfx10`, the draw shaders, the
  `struct reg_info` tables - is unused. The file is imported whole because an import that keeps only
  the interesting part is no longer byte-identical and cannot be verified by hash.
- `struct reg_info` is also the type `backend_mem.c` needs to read the register tables; it is
  declared in this header, which is why the test includes it rather than redeclaring the struct.

## Licence

The file carries the full MIT notice, "Copyright 2022 Advanced Micro Devices, Inc." (lines 1-22),
kept verbatim. libdrm as a whole is MIT.

**Decided by D-Ogi on 2026-09-21: `tests/amdgpu/shader_test_util.c` is treated as MIT**, the licence
of the libdrm repository it ships in, although the file itself carries no header. What follows is the
reasoning that was put to him. The PM4 sequence in
`driver/shim/bc250_dispatch.c` is transcribed from `tests/amdgpu/shader_test_util.c` at the same
tag (blob `60148fb8fb578ba08e40e67c79f300f03f3271f0`), which carries **no per-file licence header at
all** - it opens straight at `#include <stdio.h>`. Nothing is copied from it byte for byte: what was
taken is which registers to write, in which order, with which values, and each one is cited in the
source by file and line. That is the interface, not the expression of it, and it is the same
information AMD's own register headers carry. Still, the project rule is that every import names its
licence, and this one names the repository's rather than the file's, which is why it was asked
rather than assumed.
