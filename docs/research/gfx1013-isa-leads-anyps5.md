# gfx1013 ISA leads from the AnyPS5 shader decoder

Desk research, 2026-10-10. **Nothing on this page is a measurement on unit A.** Each row is a lead for the
silicon sweep. A lead becomes a fact only when the sweep runs it on unit A with a positive control, and then it
moves to [What the gfx1013 silicon really does](../gfx1013-isa.md) with its fact and its evidence.

## Why this source matters

The BC-250 carries the PS5 APU. Its GPU was built to Sony's requirements, and the public description of it
(LLVM, Mesa, AMD's RDNA documents) is thin, and a target definition is not a measurement. Sony's
compiler emits machine code for this GPU. Code that real PS5 games carry shows what the GPU actually executes,
including instructions that no public tool names.

[AnyPS5](https://github.com/boykopovar/AnyPS5) (GPL-2.0, read at commit
`9206c3e62167900ea0dabcbf4764b741b114dca2`) ports PS5 executables to Linux and Windows. Its shader
recompiler decodes the RDNA machine code in PS5 games and translates it to SPIR-V, so its decoder has to accept
every instruction those games use. We read it as a source of facts about the decoder only. No code from it
enters this repository.

Two limits apply to everything below:

- A decoder entry is not proof that gfx1013 executes the instruction. AnyPS5 decodes all nine GFX10.1 dot
  products, and on our silicon they write zero or a legacy minimum ([M865](../facts/hardware.md#M865),
  [M866](../facts/hardware.md#M866)). The decoder tables follow the RDNA documents where those exist.
- Most AnyPS5 measurements were made on RDNA2 parts (gfx1031, gfx1035, gfx1036), not on gfx1013. Its own
  documentation says so for each one.

## Method

The decoder has 1098 opcode entries in 16 encodings. Each entry was compared with what the AMDGPU disassembler
of LLVM 22.1.8 accepts for `-mcpu=gfx1013`:

1. VOP1, VOP2, VOPC, VOP3 and VOP3P: the answer recorded for every slot by the gfx1013 ALU sweep.
2. SOP1, SOP2, SOPC, SOPK, SOPP, SMEM, DS, MUBUF, MTBUF, FLAT and MIMG: one instruction word per entry, in
   several operand variants. Each family's word layout was checked first: a known opcode must disassemble to
   its known name.
3. Every refused word was checked again by name with the assembler. Nine refusals were artifacts of the word
   layout, because the assembler knows those instructions for gfx1013: `ds_gws_*` (six), `ds_ordered_count`
   and both `image_bvh*_intersect_ray`. They are not leads.

The result is 36 slots that the AnyPS5 decoder knows and LLVM refuses for gfx1013. A refusal by LLVM says only
that the public tool does not describe the slot. It says nothing about the silicon.

## Lead group A: 23 MIMG instructions that LLVM does not know at all

The assembler rejects these names for every target. The slots are gaps in the public GFX10 MIMG opcode map.
An opcode above 0x7f sets bit 7 of the opcode, which GFX10 keeps in bit 0 of the first dword.

| MIMG opcode | name in AnyPS5 | what AnyPS5 records | gfx1013 |
|---|---|---|---|
| 0x42, 0x43 | `image_load_by2`, `image_load_by4` | Measured on gfx1035: read 2 or 4 texels from `x & ~(N - 1)` on row y, each converted like `image_load`. Zero outside the level | not measured |
| 0x4a, 0x4b | `image_load_mip_by2`, `image_load_mip_by4` | The same with an explicit mip level, measured on gfx1035 | not measured |
| 0x52, 0x53, 0x5a, 0x5b | `image_store[_mip]_by2`, `image_store[_mip]_by4` | Decoded. The notes give no semantics | not measured |
| 0x62 | `image_gather4h_pck` | Measured on gfx1031: four horizontal window texels read raw and packed into a bitstream, DMASK bit i writes word i | not measured |
| 0x63 | `image_gather8h_pck` | Decoded. The notes give no semantics | not measured |
| 0x70, 0x71 | `image_load_pck2`, `image_load_pck4` | Measured on gfx1035: one dword with the raw bits of 2 or 4 texels, zero-extended. Zero outside the level | not measured |
| 0x73, 0x74 | `image_load_mip_pck2`, `image_load_mip_pck4` | The same with an explicit mip level, measured on gfx1035 | not measured |
| 0x76, 0x77, 0x79, 0x7a | `image_store[_mip]_pck2`, `image_store[_mip]_pck4` | Decoded. The notes give no semantics | not measured |
| 0xa0, 0xa1, 0xa5, 0xa8, 0xb0 | `image_sample_a`, `_cl_a`, `_b_a`, `_c_a`, `_o_a` | Decoded as `image_sample` variants with an "adjust" flag on the sampler. The notes do not describe the change | not measured |

If these load and gather forms execute on gfx1013, each one reads several texels in one instruction. That could
matter for texture-heavy compute shaders and for kernels that read packed data through images.

## Lead group B: 13 instructions that LLVM knows for other targets only

| instruction | encoding | where LLVM allows it | gfx1013 |
|---|---|---|---|
| nine dot products (`v_dot2*`, `v_dot4*`, `v_dot8*`, `v_dot2c_f32_f16`, `v_dot4c_i32_i8`) | VOP3P 0x13-0x19, VOP2 0x02 and 0x0d | gfx1011, gfx1012, gfx1030 | **measured**: no dot product ([M865](../facts/hardware.md#M865), [M866](../facts/hardware.md#M866)) |
| `buffer_atomic_csub` | MUBUF 0x34 | gfx1030 and later | not measured |
| `global_atomic_csub` | FLAT/GLOBAL 0x34 | gfx1030 and later | not measured |
| `global_load_dword_addtid` | FLAT/GLOBAL 0x16 | gfx1030 | not measured |
| `global_store_dword_addtid` | FLAT/GLOBAL 0x17 | gfx1030 | not measured |

AnyPS5 has execution tests for the last four (`BufferAtomicCsub`, `GlobalAtomicCsub`, `GlobalLoadAddtid`,
`GlobalStoreAddtid`), so PS5 code uses them.

## Other leads

- **Execution tests.** AnyPS5 has 235 execution tests of its translated shaders. Its hardware-oracle guide asks
  contributors to pin rows measured on a real GPU as the expected values. Each test whose rows come from RDNA2
  is a ready differential vector: where gfx1013 gives a different answer, that is a gfx1013 difference.
- **GFX10.1-only operations** that RDNA2 does not have, named in the oracle guide: `v_mad_legacy_f32`,
  `v_mac_legacy_f32`, `v_mul_lo_i32` and `s_dcache_discard`. PS5 code uses them, so they are worth measuring
  first among the defined instructions.
- **Semantic notes** in the project's technical-debt list: the rounding and zero rules of `v_mad_legacy_f32` and
  `v_mac_legacy_f32` (VOP3 0x140, VOP2 0x06), the handling of signalling NaN inputs by the unary float
  instructions, and the shift behaviour of the 32-bit bitfield masks `s_bfm_b32` and `v_bfm_b32`.
- **Method.** The AnyPS5 hardware oracle runs a few lines of assembly with one input row per lane and sets
  every floating-point mode explicitly. That is the method of our gfx1013 ALU sweep, found independently.

## What happens next

The gfx1013 ALU sweep sweeps every slot of every family on the silicon. This list does not filter it. It only
puts these slots first: group A at the start of the MIMG phase, group B in the memory phase, and the AnyPS5
execution vectors after the per-slot rounds. Each result moves to
[What the gfx1013 silicon really does](../gfx1013-isa.md) with a fact and evidence, and this page gets a
line that points to it.
