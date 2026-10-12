# What the gfx1013 silicon does with the GFX10.1 dot instructions

Date: 2026-10-10. Unit A, Windows, five runs between 17:55Z and 18:26Z. Milestone M16, the HIP
route of the BC-250 Windows driver.

## What this directory answers

LLVM and Mesa describe gfx1013 as GFX10.1 plus ray tracing, and neither gives it any Dot feature.
That is a statement of a target definition, not of the hardware. A definition can also be an
omission. So the question was put to the part itself: which GFX10.1 dot-product instruction does
this silicon execute, and what does it do with the ones it does not?

Three results, all measured on the part:

1. Nine dot opcodes were issued. **None of them computes a dot product.** Eight write zero. The
   ninth computes something else entirely.
2. That ninth one, `v_dot4c_i32_i8`, shares its VOP2 encoding with a slot that SI and CI used for
   `V_MIN_LEGACY_F32`. **The slot still holds that instruction on this part**, and so does the
   neighbouring slot 0x00e, which GFX10 leaves empty and no GFX10 mnemonic reaches.
3. Nothing faulted. No TDR, no bugcheck, no timeout. A wrong answer on this part arrives silently.

## The material

- The probe: `dotprobe.hip`, one kernel, one code object, one process. The host is the same file.
- The build: `build.ps1`, five stages and three gates. `run-lab.ps1` runs it on the unit;
  `peek-load.ps1` reads the state of the unit before and after.
- Compiler and assembler: `clang version 22.1.8`, the portable AMDGPU build (`clang-version.txt`),
  with `llvm-mc`, `ld.lld` and `llvm-objdump` of the same tree.
- LLVM target definitions read for the opcode map: `llvm/lib/Target/AMDGPU/AMDGPU.td`
  (`FeatureISAVersion10_1_3`), `VOP3PInstructions.td` and `VOP2Instructions.td` of LLVM 23.1.2.
- Runtime: our own `amdhip64.dll` over `bc250hsa`, the M16 route-B runtime. No AMD user-mode
  component is in this path.
- Unit A ran the registered driver, escape ABI `0x000700D8`, `DpmMode` 1, `DpmMaxMHz` 2000,
  `TdrDelay` 10. The GPU idled at 500 MHz and 60.5 C throughout.

## How a result is made to mean something

**The instruction bytes are not ours.** gfx1013 has no Dot feature, so its assembler refuses the
dot mnemonics. The device text comes from `clang -S`, which does not assemble, and `llvm-mc`
assembles it with `-mattr=+dot1-insts,+dot2-insts,+dot5-insts,+dot6-insts,+dot7-insts,+dot10-insts`.
A build gate then re-assembles every instruction's own operand text for gfx1013, gfx1012 and
gfx1030 and refuses the build unless all three give the same four bytes. `encoding-compare.txt` is
that comparison. gfx1012 and gfx1030 have these features by their own definition, so the word the
silicon receives is the word a supported part would receive.

**Feature forcing was rejected, and why matters.** The obvious route, `-Xclang -target-feature
-Xclang +dot1-insts`, does not work and does not say so. On a HIP device pass clang 22.1.8 exits
0, prints no warning, and emits a code object with `amdhsa.kernels: []`: the kernel is gone, 880
bytes against 4491. A probe built that way would have measured nothing and reported success. The
`llvm-mc` route above exists because of that trap.

**Three positive controls.** `v_pk_fma_f16`, `v_fma_mix_f32` and `v_mul_i32_i24` with SDWA are
instructions gfx1013 has without any forced feature. They go through the same build, the same
code object, the same kernel and the same reference check as the probes. If they fail, the probe
is at fault and says nothing about the hardware.

**A false pass is not available.** Each lane gets its own operands, the host computes the expected
value per lane, and the destination register is preloaded with the sentinel `0xcafef00d` under a
tied `"+v"` constraint. So "the instruction wrote zero" is distinguished from "the instruction
wrote nothing", which a write-only destination could not tell apart.

**One kernel, one process, one flushed line per launch.** The opcode index is a kernel argument,
so the `switch` is a scalar branch and only the selected instruction is fetched. The host appends
one line to the log on disk and flushes it before the next launch, so a hang is attributed to the
instruction named on the last line.

## 1. The nine dot instructions

4096 lanes per launch in the last two runs (256 in the first three). Operands are integer edge
words, half pairs or f32 values according to the instruction.

| instruction | encoding | LLVM feature | result on the part |
|---|---|---|---|
| `v_pk_fma_f16` (control) | VOP3P 0x0e | none, gfx1013 has it | **correct, 4096 of 4096** |
| `v_fma_mix_f32` (control) | VOP3P 0x20 | none, gfx1013 has it | **correct, 4096 of 4096** |
| `v_mul_i32_i24` SDWA (control) | VOP2 0x09 + SDWA | none, gfx1013 has it | **correct, 4096 of 4096** |
| `v_dot2_f32_f16` | VOP3P 0x13 | `dot10-insts` | writes zero in every lane |
| `v_dot2_i32_i16` | VOP3P 0x14 | `dot2-insts` | writes zero in every lane |
| `v_dot2_u32_u16` | VOP3P 0x15 | `dot2-insts` | writes zero in every lane |
| `v_dot4_i32_i8` | VOP3P 0x16 | `dot1-insts` | writes zero in every lane |
| `v_dot4_u32_u8` | VOP3P 0x17 | `dot7-insts` | writes zero in every lane |
| `v_dot8_i32_i4` | VOP3P 0x18 | `dot1-insts` | writes zero in every lane |
| `v_dot8_u32_u4` | VOP3P 0x19 | `dot7-insts` | writes zero in every lane |
| `v_dot2c_f32_f16` | VOP2 0x02 | `dot5-insts` | writes zero in every lane |
| `v_dot4c_i32_i8` | VOP2 0x0d | `dot6-insts` | computes a floating-point legacy minimum |

The sentinel count is 0 and the zero count is the whole launch for the eight that write zero, so
the destination is written. The instruction retires. It simply does not do what its name says.

The three controls passed in all five runs. The eight zero rows reproduced in all five runs.

## 2. VOP2 0x00d and 0x00e: the SI and CI legacy minimum and maximum

LLVM's GFX10 VOP2 opcode map runs 0x003 to 0x00c, then jumps to 0x00f `V_MIN_F32` and 0x010
`V_MAX_F32` (`VOP2Instructions.td:2551-2562`). The two slots in between hold nothing on GFX10. On
SI and CI they held `V_MIN_LEGACY_F32` and `V_MAX_LEGACY_F32`. GFX10.1 put `v_dot4c_i32_i8` on
0x00d and `v_dot2c_f32_f16` on 0x002.

Five VOP2 words were therefore issued with the operands written into the word, because the
register numbers are part of it. The inline assembly names the physical registers (`"+{v5}"`,
`"{v8}"`, `"{v9}"`), which clang honours for AMDGPU, tied constraints included, and surrounds
with the loads, the `s_waitcnt` and the store the kernel needs.

VOP2 on GFX10: `src0` in bits [8:0] (a VGPR is 256 + n), `vsrc1` in [16:9], `vdst` in [24:17], the
opcode in [30:25], bit 31 clear. With `vdst` = v5, `src0` = v8 and `vsrc1` = v9 the constant part
of the word is `0x000a1308`.

| word | slot | what GFX10.1 says is there | measured, out of 4096 lanes |
|---|---|---|---|
| `1E0A1308` | 0x00f | `v_min_f32` | the raw route's control, see below |
| `200A1308` | 0x010 | `v_max_f32` | the raw route's control, see below |
| `1A0A1308` | 0x00d | `v_dot4c_i32_i8` | **4096 of 4096** `D = (S0 < S1) ? S0 : S1` |
| `1C0A1308` | 0x00e | nothing | **4096 of 4096** `D = (S0 >= S1) ? S0 : S1` |
| `040A1308` | 0x002 | `v_dot2c_f32_f16` | **4096 of 4096** zero |

Those two rules are the SI and CI definitions of `V_MIN_LEGACY_F32` and `V_MAX_LEGACY_F32`,
including the tie direction. The tie was measured, not assumed: against `<=` the 0x00d row gives
4094 of 4096, and against `>` the 0x00e row gives 4094 of 4096. In both cases the two missing
lanes are the two orderings of `+0` against `-0`, which is the only pair that compares equal while
holding different bits. The minimum takes `S1` on a tie and the maximum takes `S0`.

Each word is gated twice before the build finishes: the opcode field arithmetic, and `llvm-mc`'s
own encoding of the mnemonic that occupies that slot on some generation. `llvm-objdump` then
decodes `1A0A1308` back to `v_dot4c_i32_i8 v5, v8, v9` and `040A1308` back to
`v_dot2c_f32_f16 v5, v8, v9` in `dotprobe.gfx1013.dis`, which is a third confirmation. It cannot
decode `1C0A1308` at all and prints it as `.long`, which is the same statement the opcode map
makes.

### The operand set

4096 lanes. Every ordered pair of 25 f32 bit patterns fills the first 625: both zeros, both
infinities, quiet and signalling NaNs of both signs, the smallest and largest denormal of both
signs, the smallest normal, the largest finite, and ordinary values. A deterministic random word
stream fills the rest, with one operand in three of them replaced by a special value, so the
asymmetric rules are also exercised against ordinary neighbours. The destination is preloaded with
a third unrelated word, and that word came back in 0 of 4096 lanes for all five, so each of them
did write.

Fourteen other candidate rules were evaluated on the same lanes. None reached 4096. For 0x00d the
next best were the denormal-flushing legacy minimum 3795, the IEEE minimum 3749, the
NaN-propagating minimum 3733, the signed integer minimum 2930, `src1` 2416 and `src0` 1705.

### Why these are not the documented minimum and maximum

The same run measured `v_min_f32` and `v_max_f32` by mnemonic, in the same kernel and under the
same mode register, and compared the outputs lane against lane. That comparison needs no
assumption about `MODE.IEEE` or about denormal flushing, because both instructions ran under the
same mode.

```
SAME v_min_f32        raw-vop2-0x00f   4096 of 4096   <= identical
SAME v_max_f32        raw-vop2-0x010   4096 of 4096   <= identical
SAME v_min_f32        raw-vop2-0x00d   3588 of 4096
     lane   12 v_min_f32 00000000  raw-vop2-0x00d 7fc00000
     lane   14 v_min_f32 7fc00001  raw-vop2-0x00d 7f800001
SAME v_max_f32        raw-vop2-0x00e   3588 of 4096
     lane   12 v_max_f32 00000000  raw-vop2-0x00e 7fc00000
     lane   14 v_max_f32 7fc00001  raw-vop2-0x00e 7f800001
```

Lane 12 is `+0` against `+qNaN` and lane 14 is `+0` against `+sNaN`. The documented instruction
returns the number and quiets a signalling NaN (`7f800001` in, `7fc00001` out), which is the IEEE
rule. The legacy slot returns `S1`, because `+0 < NaN` is false, and passes the signalling NaN
through untouched. 508 of the 4096 lanes separate the two, and they are the NaN lanes.

The first two rows are the raw route's positive control: a hand-written word and the assembler's
own encoding of the same instruction agree in every lane, so the route, the register constraints
and the word arithmetic are sound. In this build the compiler allocated v5, v8 and v9 for the
mnemonic cases as well, so those two pairs are byte-identical instructions. The control shows that
the route works, not that two different encodings agree.

Not identified here, and not needed for any decision: the exact rule this part's own `v_min_f32`
follows. Its best candidate reached 3792 of 4096, so it is none of the four references, but it is
not the legacy form, and that is the only property the identification above rests on.

## 3. What this means for the stack

`has_accelerated_dot_product` must stay false for `CHIP_GFX1013` in
`src/amd/common/ac_gpu_info.c`. Turning it on would not make the part slower. It would make it
wrong: `v_dot4_i32_i8` returns zero and `v_dot4c_i32_i8` returns a floating-point minimum, both
without a fault, a timeout or an error anywhere. A plausible-looking number is worse than a zero.

The three instructions that do work are reached by generation tests, not by chip lists, so no
patch is needed to use them: `v_pk_fma_f16` and `v_fma_mix_f32` are GFX9-and-later packed-math
instructions, and `v_mul_i32_i24` is in every generation.

`aco_elf.cpp:159-162` already asserts `!has_accelerated_dot_product` for gfx1013. llama.cpp's
Vulkan path asks the driver for `VK_KHR_shader_integer_dot_product` rather than consulting a chip
list, so it follows the Mesa value. `ggml-cuda/vendors/hip.h:236-238` does leave gfx1013 out of
its `RDNA1` list, and for the dot product that omission is in our favour, because the inline
assembly that list selects costs 2.2 times the instructions on this part
([the barrier measurement](../gfx1013-dp4a-barrier-2026-10-09/README.md)).

## 4. The runs

| run | lanes | what it added | log |
|---|---|---|---|
| 17:55Z | 256 | the nine dot opcodes and the three controls | `results-20261010T175529Z.txt` |
| 17:57Z | 256 | a repeat of the same build | `results-20261010T175736Z.txt` |
| 18:09Z | 256 | the first candidate table for the two VOP2 forms | `results-20261010T180943Z.txt` |
| 18:24Z | 4096 | the five raw words, the special-value operand set, the agreement matrix | `results-20261010T182432Z.txt` |
| 18:26Z | 4096 | the two tie-direction candidates | `results-run-20261010T182643Z.txt` |

Each of the last two runs is 19 launches in one process in 0.8 s. The device code object is
`C7E54202` in both, so the instruction bytes were identical; only the host references changed.

Unit A before and after every run: health `flags=15`, generation 27787407 and epoch 5 unchanged
across the last two runs, 0 timeouts, 0 refused and 0 soft-recovered on both nodes, "no TDR",
0 bugchecks, 0 Display 4101 events, the fan under driver control, idle at 500 MHz and 60.5 C.
Nothing needed recovery at any point. One episode between the first and the second run showed the
GPU at 100 % busy and 84.4 C with `emergencies=1` and no GPU client process left; it belongs to
another agent's work on the unit, not to this probe, and is recorded here only because it falls
between two of these runs.

## 5. How to re-run it

```
powershell -File scratch\gfx1013-dot-probe\build.ps1
python bc250-win\tools\win\target.py push scratch\build\gfx1013-dot\dotprobe.exe --to C:\BC250\tmp\gfx1013-dot
python bc250-win\tools\win\target.py ps scratch\gfx1013-dot-probe\run-lab.ps1
```

`build.ps1` refuses to finish unless every instruction appears exactly once in the device text,
the three targets give the same bytes for each one, every hand-written word matches both the field
arithmetic and the assembler, and every word survives into the linked code object. `run-lab.ps1`
reads the driver's health, clock, DPM state and log summary before and after, refuses to start at
or above 87 C, and kills the process past its deadline. `dotprobe.exe --list` prints the opcode
table, and `--op <name>` runs one of them alone.

## Files

| file | contents |
|---|---|
| `dotprobe.hip` | the probe: the kernel, the per-lane references, the candidate table, the host |
| `build.ps1` | the five build stages and the encoding gates |
| `run-lab.ps1` | the runner: health before and after, the deadline, the result log |
| `peek-load.ps1` | the read-only state check of the unit |
| `encoding-compare.txt` | each instruction's bytes for gfx1013, gfx1012 and gfx1030 |
| `dotprobe.gfx1013.dis` | the disassembly of the linked code object |
| `results-*.txt` | the five run logs, each line flushed as it was written |
| `clang-version.txt` | the compiler banner |
| `SHA256SUMS.txt` | the hashes of everything above and of the build artifacts |
