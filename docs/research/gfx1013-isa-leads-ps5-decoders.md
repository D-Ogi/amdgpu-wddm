# gfx1013 ISA leads from PS5 shader decoders

Desk research, 2026-10-10. **Nothing on this page is a measurement on unit A.** Each row is a lead for the
silicon sweep. A lead becomes a fact only when the sweep runs it on unit A with a positive control, and then it
moves to [What the gfx1013 silicon really does](../gfx1013-isa.md) with its fact and its evidence.

## Why these sources matter

The BC-250 carries the PS5 APU. Its GPU was built to Sony's requirements, and the public description of it
(LLVM, Mesa, AMD's RDNA documents) is thin, and a target definition is not a measurement. Sony's
compiler emits machine code for this GPU. Code that real PS5 games carry shows what the GPU actually executes,
including instructions that no public tool names.

Three open projects run PS5 executables on a PC. Each one has a shader recompiler that decodes the RDNA machine
code in PS5 games and translates it, so each decoder has to accept every instruction those games use:

| project | read at commit | licence |
|---|---|---|
| [AnyPS5](https://github.com/boykopovar/AnyPS5) | `9206c3e62167900ea0dabcbf4764b741b114dca2` | GPL-2.0 |
| [KytyPS5](https://github.com/KytyPS5/KytyPS5) | `1cfb9522` | GPL-2.0 |
| [sharpemu](https://github.com/sharpemu/sharpemu) | `a2983692` | GPL-2.0 |

We read them as a source of facts about their decoders only. No code from them enters this repository.

Two limits apply to everything below:

- A decoder entry is not proof that gfx1013 executes the instruction. AnyPS5 decodes all nine GFX10.1 dot
  products, and on our silicon they write zero or a legacy minimum ([M865](../facts/hardware.md#M865),
  [M866](../facts/hardware.md#M866)). The decoder tables follow the RDNA documents where those exist.
- Most AnyPS5 measurements were made on RDNA2 parts (gfx1031, gfx1035, gfx1036), not on gfx1013. Its own
  documentation says so for each one.

## Method

Three checks, each against what the AMDGPU assembler and disassembler of LLVM 22.1.8 accept for
`-mcpu=gfx1013`:

1. **AnyPS5 by opcode.** The decoder has 1098 opcode entries in 16 encodings. VOP1, VOP2, VOPC, VOP3 and VOP3P
   use the answer that the gfx1013 ALU sweep records for every slot. SOP1, SOP2, SOPC, SOPK, SOPP, SMEM, DS,
   MUBUF, MTBUF, FLAT and MIMG use one instruction word per entry, in several operand variants. Each family's
   word layout was checked first: a known opcode must disassemble to its known name.
2. **All three by name.** Every instruction name in the three decoders (AnyPS5 1114, KytyPS5 740, sharpemu 747)
   goes through the assembler. A name that it rejects for every target, or accepts for other targets only, is a
   candidate. Many candidates are only spellings: sharpemu writes `image_sample_cbcl` for `image_sample_c_b_cl`.
   A candidate becomes a lead only when its opcode is a slot that LLVM refuses for gfx1013.
3. **AMD's own map.** Every lead slot was looked up in the ten files of AMD's machine-readable ISA (package of
   2026-08-06, RDNA1 to RDNA4 and CDNA1 to CDNA5).

Step 1 refused some words because of the word layout and not because of the opcode. The assembler knows those
instructions for gfx1013: `ds_gws_*` (six), `ds_ordered_count` and both `image_bvh*_intersect_ray`. They are
not leads.

A refusal by LLVM says only that the public tool does not describe the slot. It says nothing about the silicon.

## The first lead: MIMG opcode 0xE5 in Ghost of Yōtei

[AnyPS5 issue 1941](https://github.com/boykopovar/AnyPS5/issues/1941) reports that Ghost of Yōtei (PPSA26344)
emits MIMG opcode 0xE5, and that no decoder names it. The title's shader store has 38 such words: `f1949f05` at
34 sites and `f1962969` at 4. The issue quotes one site in full: `f1949f05 00060018 121b1a19 00003715`. The
project closed the issue with two changes. One names the opcode in the error message. The other defers the
shader that contains it. 0xE5 stays unidentified.

What the thread settles:

- The store is GFX10-family code. Its 0xE6 and 0xE7 words decode as the BVH instructions on gfx1030 and gfx1013
  and on no GFX11 target, which moved those two to other opcodes.
- A reading of the 0xE5 words as the GFX11 `image_gather4_c_b_cl` was withdrawn by its own author. GFX11 reads
  the opcode from other bits, and the gfx1013 encoding of that gather is 0x4E, which the decoder already
  carries.
- LLVM has no instruction at 0xE5 for any target. No file of AMD's machine-readable ISA names it, and the
  RDNA 2 PDF guide does not either.

What we add, from the assembler of LLVM 22.1.8 for gfx1013 (`llvm-mc -arch=amdgcn -mcpu=gfx1013 -show-encoding`):

- The first word, `f1949f05 00060018`, has exactly the fixed fields of the two BVH instructions: DMASK 0xf,
  UNORM, R128, dimension 0, no sampler. The assembler encodes `image_bvh_intersect_ray v[0:3], [v24, v25, v26,
  v27, v18, v21, v55, v1], s[24:27] a16` as `f1989f05 40060018 121b1a19 00013715`. The first dword differs from
  the game's in the opcode bits only (0x66 against 0x65). The second differs in the A16 bit only. The first
  address dword is identical.
- So 0xE5 takes a BVH-style list of separate address registers. With A16 clear, the game's word gives seven
  addresses (`v24`, `v25`, `v26`, `v27`, `v18`, `v21`, `v55`) if its two zero bytes are padding, or nine if they
  name `v0`. The BVH instructions take 11 addresses, or 8 with A16.
- The second word, `f1962969`, has another shape: DMASK 0x9, a 2D array, GLC and LWE, and bit 6 set, which GFX10
  does not assign. Its four sites may be data in the store and not code.

So the first word has the shape of a ray-tracing instruction beside the two that the public documents give
gfx1013 alone. That is a reading of bits, not a measurement. Unit A is the PS5 GPU. The MIMG phase of the sweep
starts with 0xE5, with the game's own instruction words.

## Lead group A: MIMG instructions that LLVM does not know at all

The assembler rejects these names for every target. An opcode above 0x7f sets bit 7 of the opcode, which GFX10
keeps in bit 0 of the first dword.

### A1: 18 load, store and gather forms that AMD documents and LLVM does not

AMD's machine-readable ISA for RDNA1 and RDNA2 names all 18 at these opcodes, with the names that AnyPS5 uses.
The public RDNA and RDNA 2 PDF guides do not list them. The RDNA3 file gives six of these slots (0x42, 0x43,
0x4a, 0x4b, 0x62, 0x63) to other instructions. So these slots are not a gap in AMD's map. They are a gap in LLVM
and in the PDF guides. AMD's text for each of them is on
[GFX10 image instructions that only AMD's ISA XML describes](../gfx10-image-ops-amd-xml.md).

| MIMG opcode | name | AMD's description (RDNA1 file) | what AnyPS5 records | gfx1013 |
|---|---|---|---|---|
| 0x42, 0x43 | `image_load_by2`, `image_load_by4` | Load 2 or 4 horizontal elements from the largest mip level, with the format conversion of the descriptor | Measured on gfx1035: reads from `x & ~(N - 1)` on row y. Zero outside the level | not measured |
| 0x4a, 0x4b | `image_load_mip_by2`, `image_load_mip_by4` | The same from a mip level that the shader gives | The same, measured on gfx1035 | not measured |
| 0x52, 0x53, 0x5a, 0x5b | `image_store[_mip]_by2`, `image_store[_mip]_by4` | Store 2 or 4 horizontal elements, with the format conversion | Decoded. The notes give no semantics | not measured |
| 0x62, 0x63 | `image_gather4h_pck`, `image_gather8h_pck` | Gather all components of 4 or 8 texels from a 4x1 or 8x1 row. DMASK selects the channels | 0x62 measured on gfx1031: the texels read raw and packed, DMASK bit i writes word i | not measured |
| 0x70, 0x71 | `image_load_pck2`, `image_load_pck4` | Load 2 or 4 horizontal elements raw. The descriptor format is ignored, 8-bit and 16-bit components are zero-extended | Measured on gfx1035: one dword with the raw bits | not measured |
| 0x73, 0x74 | `image_load_mip_pck2`, `image_load_mip_pck4` | The same from a mip level that the shader gives | The same, measured on gfx1035 | not measured |
| 0x76, 0x77, 0x79, 0x7a | `image_store[_mip]_pck2`, `image_store[_mip]_pck4` | Store 2 or 4 horizontal elements raw | Decoded. The notes give no semantics | not measured |

If these forms execute on gfx1013, each one reads or writes several texels in one instruction. That could
matter for texture-heavy compute shaders and for kernels that read packed data through images.

### A2: 16 sample forms with an "adjust" flag, which nobody documents

Each one is a plain `image_sample` form plus 0x80: `image_sample_a` at 0xa0 is `image_sample` (0x20) with the
flag. The 16 opcodes are 0xa0, 0xa1, 0xa5, 0xa6, 0xa8, 0xa9, 0xad, 0xae, 0xb0, 0xb1, 0xb5, 0xb6, 0xb8, 0xb9,
0xbd and 0xbe, which mirror `image_sample` with every combination of `_cl`, `_b`, `_c` and `_o`.

- KytyPS5 and sharpemu decode all 16. AnyPS5 decodes five of them (0xa0, 0xa1, 0xa5, 0xa8, 0xb0).
- The three decoders treat them as the plain form with an "adjust" flag. None of them records what the flag
  changes.
- No file of AMD's machine-readable ISA names any of them, and LLVM has no instruction in these slots.

## Lead group B: instructions that LLVM knows for other targets only

| instruction | encoding | where LLVM allows it | AMD's ISA files | gfx1013 |
|---|---|---|---|---|
| nine dot products (`v_dot2*`, `v_dot4*`, `v_dot8*`, `v_dot2c_f32_f16`, `v_dot4c_i32_i8`) | VOP3P 0x13-0x19, VOP2 0x02 and 0x0d | gfx1011, gfx1012, gfx1030 | | **measured**: no dot product ([M865](../facts/hardware.md#M865), [M866](../facts/hardware.md#M866)) |
| `buffer_atomic_csub` | MUBUF 0x34 | gfx1030 and later | | not measured |
| `global_atomic_csub` | FLAT/GLOBAL 0x34 | gfx1030 and later | | not measured |
| `global_load_dword_addtid` | FLAT/GLOBAL 0x16 | gfx1030 | | not measured |
| `global_store_dword_addtid` | FLAT/GLOBAL 0x17 | gfx1030 | | not measured |
| `s_add_f32`, `s_sub_f32` (sharpemu) | SOP2 0x40, 0x41 | gfx1150 and later | RDNA3.5 and RDNA4 only | not measured |

AnyPS5 has execution tests for `buffer_atomic_csub`, `global_atomic_csub` and the two `addtid` forms, so PS5
code uses them. The scalar float pair is a GFX11.5 feature. Its presence in a PS5 decoder is the oddest row of
this table, and the first scalar slots that the sweep asks.

## What no decoder names

No one of the three decoders names any VOP1 slot from 0x80 up. Neither do LLVM and the ten files of AMD's
machine-readable ISA. What the gfx1013 sweep finds in those slots therefore has no public counterpart to
compare with. Its results go to [What the gfx1013 silicon really does](../gfx1013-isa.md), as facts with
evidence.

## Other leads

- **Execution tests.** AnyPS5 has 235 execution tests of its translated shaders. Its hardware-oracle guide asks
  contributors to pin rows measured on a real GPU as the expected values. Each test whose rows come from RDNA2
  is a ready differential vector: where gfx1013 gives a different answer, that is a gfx1013 difference.
- **GFX10.1-only operations** that RDNA2 does not have, named in the AnyPS5 oracle guide: `v_mad_legacy_f32`,
  `v_mac_legacy_f32`, `v_mul_lo_i32` and `s_dcache_discard`. PS5 code uses them, so they are worth measuring
  first among the defined instructions.
- **Semantic notes** in the AnyPS5 technical-debt list: the rounding and zero rules of `v_mad_legacy_f32` and
  `v_mac_legacy_f32` (VOP3 0x140, VOP2 0x06), the handling of signalling NaN inputs by the unary float
  instructions, and the shift behaviour of the 32-bit bitfield masks `s_bfm_b32` and `v_bfm_b32`.
- **Method.** The AnyPS5 hardware oracle runs a few lines of assembly with one input row per lane and sets
  every floating-point mode explicitly. That is the method of our gfx1013 ALU sweep, found independently.

## What happens next

The gfx1013 ALU sweep sweeps every slot of every family on the silicon. This list does not filter it. It only
puts these slots first: 0xE5 and then groups A1 and A2 at the start of the MIMG phase, the scalar float pair at
the start of the scalar phase, the rest of group B in the memory phase, and the AnyPS5 execution vectors after
the per-slot rounds. Each result moves to [What the gfx1013 silicon really does](../gfx1013-isa.md) with a fact
and evidence, and this page gets a line that points to it.
