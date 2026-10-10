# What the gfx1013 silicon really does

LLVM and Mesa describe gfx1013 as GFX10.1 plus ray tracing. That is a target definition. It is not
a measurement of the part. A definition can also be an omission, because few people own this chip
and fewer add it to a list.

This page records what the silicon does when we ask it directly. Every row rests on a fact in
[the claim register](facts.md) and on a directory under `evidence/`. A row without both does not
belong here.

**How to add to this page.** Run the instruction or the register on unit A with a positive control
beside it. Write the result into `evidence/`, add the fact row, then add a short section here that
cites both. State what the measurement does not show.

## Dot products: the part has none, and it does not say so

Nine GFX10.1 dot-product instructions were issued on unit A. None of them computes a dot product
([M865](facts/hardware.md#M865)).

| instruction | encoding | LLVM feature | what the silicon does |
|---|---|---|---|
| `v_dot2_f32_f16` | VOP3P 0x13 | `dot10-insts` | writes zero in every lane |
| `v_dot2_i32_i16` | VOP3P 0x14 | `dot2-insts` | writes zero in every lane |
| `v_dot2_u32_u16` | VOP3P 0x15 | `dot2-insts` | writes zero in every lane |
| `v_dot4_i32_i8` | VOP3P 0x16 | `dot1-insts` | writes zero in every lane |
| `v_dot4_u32_u8` | VOP3P 0x17 | `dot7-insts` | writes zero in every lane |
| `v_dot8_i32_i4` | VOP3P 0x18 | `dot1-insts` | writes zero in every lane |
| `v_dot8_u32_u4` | VOP3P 0x19 | `dot7-insts` | writes zero in every lane |
| `v_dot2c_f32_f16` | VOP2 0x02 | `dot5-insts` | writes zero in every lane |
| `v_dot4c_i32_i8` | VOP2 0x0d | `dot6-insts` | a floating-point legacy minimum |

**This is a hazard, not a limit.** Nothing faults. There is no TDR, no bugcheck, no timeout and no
error from the runtime. The destination is written, the instruction retires, and the answer is
wrong. The probe preloads the destination with a sentinel, so we know that the eight zero rows are
a written zero and not an untouched register.

So `has_accelerated_dot_product` must stay false for `CHIP_GFX1013` in
`src/amd/common/ac_gpu_info.c` of the Mesa fork. A true value there would not make the part slow.
It would make it quietly wrong. `aco_elf.cpp` already asserts the false value for this chip.

Three instructions in the same code object are correct in all 4096 lanes: `v_pk_fma_f16`,
`v_fma_mix_f32` and `v_mul_i32_i24` with SDWA. They are the positive controls of the probe. They
also show the way forward for packed arithmetic on this part, and no patch selects them, because
the compiler reaches them by a generation test and not by a chip list.

## VOP2 0x0d and 0x0e hold the SI and CI legacy minimum and maximum

The GFX10 VOP2 opcode map runs 0x00c, then jumps to 0x00f `V_MIN_F32` and 0x010 `V_MAX_F32`
(`VOP2Instructions.td` of LLVM 23.1.2). The two slots between them are empty on GFX10. SI and CI
put `V_MIN_LEGACY_F32` and `V_MAX_LEGACY_F32` there. GFX10.1 later put `v_dot4c_i32_i8` on 0x00d.

On this part the old instructions are still alive ([M866](facts/hardware.md#M866)):

| slot | word with `v5`, `v8`, `v9` | GFX10.1 says | the silicon gives, over 4096 lane pairs |
|---|---|---|---|
| 0x00d | `1A0A1308` | `v_dot4c_i32_i8` | `D = (S0 < S1) ? S0 : S1`, 4096 of 4096 |
| 0x00e | `1C0A1308` | nothing | `D = (S0 >= S1) ? S0 : S1`, 4096 of 4096 |
| 0x002 | `040A1308` | `v_dot2c_f32_f16` | zero, 4096 of 4096 |

Those two rules are the SI and CI definitions, the tie direction included. The minimum takes `S1`
when the two compare equal, and the maximum takes `S0`. Both directions are measured. The only
pair that can tell them apart is `+0` against `-0`.

**They are not the documented minimum and maximum.** `v_min_f32` and `v_max_f32` ran in the same
kernel under the same mode register, so the comparison needs no assumption about `MODE.IEEE` or
about denormal flushing. The outputs agree in only 3588 of 4096 lanes, and the 508 other lanes are
the NaN lanes. Against a NaN the documented pair returns the number and quiets a signalling NaN.
The legacy pair returns `S1` and passes the signalling NaN through unchanged.

This explains the last row of the dot table. On this silicon the encoding of `v_dot4c_i32_i8` is a
live SI-era instruction, which GFX10.1 reassigned in the documentation alone. A byte dot product
issued there returns a plausible floating-point number. That is worse than a zero.

What the ISA page of the part would have to say for 0x00e is not known to us. We have no AMD
document for gfx1013, and `llvm-objdump` cannot decode the word at all.

## Traps of the toolchain

Two of them cost a day each, so they are here and not only in a commit message
([M867](facts/tooling.md#M867)).

**Do not force a target feature on a HIP device pass.** With `-Xclang -target-feature -Xclang
+dot1-insts`, clang 22.1.8 exits 0, prints no warning, and writes a code object with
`amdhsa.kernels: []`. The kernel is gone, 880 bytes against 4491. A test built that way measures
nothing and reports success.

The route that works for an instruction the subtarget lacks has four steps:

1. `clang -S` for the device, which does not assemble, so inline assembly passes through as text.
2. `llvm-mc -mcpu=gfx1013 -mattr=+dot1-insts,...` over that text.
3. `ld.lld -m elf64_amdgpu -shared`. Without the `-m` flag `ld.lld` refuses the object with
   `unknown file type`.
4. `clang-offload-bundler`, then a host compile with `-fcuda-include-gpubinary`.

**For an opcode with no mnemonic, write the word and name the registers.** The register numbers
are part of a VOP2 word, so the allocator cannot choose them. clang honours physical-register
constraints for AMDGPU, tied ones included:

```c
asm volatile(".long 0x1c0a1308" : "+{v5}"(d) : "{v8}"(x), "{v9}"(y));
```

VOP2 on GFX10 puts `src0` in bits [8:0], where a VGPR is 256 plus its number, `vsrc1` in [16:9],
`vdst` in [24:17] and the opcode in [30:25]. Bit 31 is clear. Gate every word twice: against that
arithmetic, and against the assembler's own encoding of a mnemonic that occupies the slot on
another generation.

## How to re-run the probe

The probe is one kernel in one code object, and the opcode index is a kernel argument. The `switch`
is therefore a scalar branch, and one launch fetches one instruction. The host appends a line to
the log on disk and flushes it before the next launch, so a hang names the instruction that caused
it.

```
powershell -File scratch\gfx1013-dot-probe\build.ps1
python bc250-win\tools\win\target.py push scratch\build\gfx1013-dot\dotprobe.exe --to C:\BC250\tmp\gfx1013-dot
python bc250-win\tools\win\target.py ps scratch\gfx1013-dot-probe\run-lab.ps1
```

The source, the scripts and the five run logs are in
[`evidence/m16/gfx1013-isa-probe-2026-10-10/`](../evidence/m16/gfx1013-isa-probe-2026-10-10/README.md).
`build.ps1` refuses to finish unless each instruction appears once in the device text, the three
targets give the same bytes for it, each hand-written word matches both the arithmetic and the
assembler, and each word survives into the linked code object.

## What has not been asked yet

- Every other undefined ALU opcode slot of VOP1, VOP2, VOPC, VOP3 and VOP3P. The two live slots
  above were found by one guess from the SI opcode map. A sweep may find more.
- Side effects of an undefined word on `VCC`, `EXEC` and the other lanes.
- The VOP3P operand modifiers and the `op_sel` forms of the dot instructions. The probe used the
  plain forms.
- The exact rule this part's own `v_min_f32` follows. Its best candidate among our references
  reached 3792 of 4096 lanes. It is not the legacy form, and that is all the identification above
  needs.
- The scalar, branch, memory and image families. An undefined word there can hang the part, and we
  have no GPU reset ([M53](facts/linux.md#m53)).
- The image instructions that PS5 code uses and no public tool names, and the other leads from the
  AnyPS5 shader decoder: [gfx1013 ISA leads from the AnyPS5 shader decoder](research/gfx1013-isa-leads-anyps5.md).
  They are leads, not facts.

Nie wszystko złoto, co się świeci: not all that glitters is gold. A GFX10.1 opcode on this chip
may be a GFX10.1 instruction, an instruction of 2012, or nothing at all.
