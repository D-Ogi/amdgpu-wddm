# INT8 dot products on gfx1013: software lowering in RADV and ACO

## In short

- gfx1013 has no working packed dot-product instruction. All nine GFX10.1 dot instructions give a wrong result
  without a fault ([M865](../facts/hardware.md#m865)).
- Our Mesa fork (`D-Ogi/mesa-amdgpu-wddm`) therefore lowers every packed 4x8 dot product to 24-bit multiplies and
  adds. The Vulkan driver reports no accelerated dot product, and ACO selects no `v_dot*` instruction.
- Nine commits change how the lowering is shaped. They do not change the result: every lowered sequence gives
  the same bits as the SPIR-V definition, wrap and saturation included.
- On unit A, the FSR 4.1.2 RDNA 2 model upscales 1706x960 to 2560x1440 in 11.93 ms of GPU time instead of
  16.22 ms (lowest run median, 0.736x, [M930](../facts/d3d.md#m930)). The output is bit-identical before and
  after.
- One small shader dispatch gets slower. The section "Known trade-off" gives the numbers.

## The rules

The lowering must keep these rules. A change that breaks one of them does not land.

1. `has_accelerated_dot_product` and every `has_*dot_4x8*` compiler option stay false for gfx1013.
2. Every `*Accelerated` property of `VK_KHR_shader_integer_dot_product` stays false.
3. ACO never selects `v_dot4_i32_i8`, `v_dot4c_i32_i8` or another packed dot instruction.
4. Every lowered sequence is bit-exact against the SPIR-V definition. This covers the sign extension of each
   byte, the 32-bit wrap of the accumulator and the clamp of the `_sat` forms.

`VK_KHR_shader_integer_dot_product` itself stays available. A shader that uses the extension gets correct results
through the software sequences.

## The lowering

`nir_opt_algebraic` expands `sdot_4x8_iadd`, `udot_4x8_uadd` and `sudot_4x8_iadd` when the backend has no dot
instruction. The nine commits change this expansion in five ways.

| Change | What it does | Why |
|--------|--------------|-----|
| Three-input grouping | The five terms are grouped as `(p0 + (p1 + p2)) + (p3 + c)` | ACO adds them with two `v_add3_u32`. The balanced tree needed three adds. A dot product with run-time operands goes from 7 to 6 VALU instructions |
| MAD24 chain | A signed dot product with a constant operand becomes a chain of `imad24_ir3` that ends in the accumulator | Each step is one `v_mad_i32_i24` |
| Shared operand only | The chain is used only when three or more such dot products use the same run-time operand | The chain needs the four bytes as separate extracts. One dot product alone costs 8 instructions in the chain and 6 without it |
| Accumulator fusion | `iadd(iadd(0, sdot_4x8_iadd(a, b, 0)), c)` becomes `sdot_4x8_iadd(a, b, c)` when `a` or `b` is a constant | dxil-spirv writes this form. The fusion gives the chain its real accumulator |
| Deferred lowering | For compute shaders, the first `radv_optimize_nir` round keeps each `sdot_4x8_iadd` as one instruction. The next round lowers it | The fusion and the loop clean-up then act before the expansion. The large FSR 4 network passes go from 256 to 168 VGPRs and lose every spill |

Three further changes keep the result exact on every backend:

- ACO selects `imul24` and `imad24_ir3`. The scalar form sign-extends each source with `s_bfe_i32` first, because
  `s_mul_i32` multiplies all 32 bits.
- The MAD24 chain needs the compiler option `has_imad24_ir3`. `ac_nir_set_options` sets it for ACO only. The LLVM
  backend and other drivers keep the shapes without MAD24.
- `_sat` is removed only when the accumulator is a constant that the dot product cannot push out of int32. The
  signed range is [-0xfe00, 0x10000] and the mixed range is [-0x1fe00, 0x1fa04].

The deferral changes only the order of the NIR rounds. It uses a copy of the compiler options that only the NIR
optimizer reads. No hardware dot product is selected and no reported capability changes.

## How exactness is checked

Three independent checks, from the instruction model to the real shaders on the GPU:

1. A host model of each sequence uses the instruction semantics (SDWA byte select, `v_bfe_i32`,
   `v_mul_i32_i24`, `v_mad_i32_i24`, wrapping adds, the `s_bfe_i32` field). It compares the result with the
   definition. The inputs are every byte pair in every lane, edge vectors with 12 edge accumulators, 10^7 random
   vectors, the `_sat` forms near the int32 limits and 10^7 full 32-bit scalar sources. The model makes
   240155688 checks and finds 0 failures. Its negative control, a zero-extended byte, fails 173757 of 200000
   checks.
2. A Vulkan client runs four shaders on the GPU and compares every output word with a CPU reference. The four
   shaders cover run-time operands, a constant-weight layer, uniform data on the scalar path and the
   dxil-spirv form. It checks 1633280 records. A run with the wrong sign extension must fail.
3. A second client runs 14 of the FSR 4.1.2 INT8 shaders, translated by dxil-spirv, on seeded inputs. It hashes
   every written buffer and image. The hashes must be equal before and after the change.

An offline harness, `aco_dot4_count`, compiles each case and each corpus shader for gfx1013 without a GPU. It
stops with exit code 9 when a dot opcode reaches ACO or the ACO program.

## Lab result (unit A, 2026-10-29)

Driver 40.7.216.106 (tester.34, lab flavour). "Before" is the installed Mesa, "after" the same Mesa with the nine
commits, both built with the release recipe. The evidence is
[evidence/windows/2026-10-29-int8-dot-lowering](../../evidence/windows/2026-10-29-int8-dot-lowering/RESULT.md).
The facts are [M928](../facts/icd.md#m928) (exactness), [M929](../facts/icd.md#m929) (corpus) and
[M930](../facts/d3d.md#m930) (FSR 4 through D3D12).

GPU exactness:

- All 1633280 records match the reference before and after. The output hashes are equal.
- The wrong-sign run fails with 9625073 mismatches.
- No accelerated property is reported and no `v_dot*` instruction is in the program.
- The hardware listing of the layer shader has 37 `v_mad_i32_i24` and the dxil-form shader has 31. Before the
  change both had none. The offline harness gives the same counts.

FSR 4.1.2 corpus, GPU time a dispatch, median of 30:

| Shader | VGPRs before > after | Spilled VGPRs | Median ms before > after | Ratio |
|--------|----------------------|---------------|--------------------------|-------|
| pass1 | 256 > 168 | 0 > 0 | 2.380 > 0.777 | 0.33 |
| pass2 | 256 > 168 | 40 > 0 | 9.749 > 6.129 | 0.63 |
| pass3 | 56 > 56 | 0 > 0 | 0.739 > 0.689 | 0.93 |
| pass4 | 256 > 168 | 0 > 0 | 9.923 > 5.574 | 0.56 |
| pass5 | 256 > 168 | 0 > 0 | 1.077 > 0.457 | 0.42 |
| pass6 | 84 > 84 | 0 > 0 | 2.131 > 1.172 | 0.55 |
| pass7 | 64 > 64 | 0 > 0 | 2.495 > 2.430 | 0.97 |
| pass8 | 64 > 64 | 0 > 0 | 0.588 > 0.679 | 1.16 |
| pass9 | 84 > 72 | 0 > 0 | 2.839 > 3.294 | 1.16 |
| pass10 | 256 > 168 | 27 > 0 | 1.039 > 0.532 | 0.51 |
| pass11 | 56 > 56 | 0 > 0 | 10.410 > 9.691 | 0.93 |
| pass12 | 256 > 168 | 0 > 0 | 4.027 > 2.900 | 0.72 |
| post-pass, variant 1 | 128 > 100 | 0 > 0 | 6.639 > 5.579 | 0.84 |
| post-pass, variant 2 | 128 > 84 | 0 > 0 | 6.632 > 5.565 | 0.84 |
| sum | | | 60.667 > 45.468 | 0.75 |

Every shader is deterministic, and every written resource is equal before and after.

The pass9 median is not a loss. Its dispatch times spread by up to 1.68x, and its fastest dispatch after the
change is 0.954x of the fastest before. pass8 is the one real loss. The section "Known trade-off" covers it.

FSR 4.1.2 RDNA 2 model through D3D12 (the hosted ICD), median GPU time of dispatches 150 to 299:

| Geometry | Before | After | Output |
|----------|--------|-------|--------|
| 1706x960 to 2560x1440 | 16.22 ms | 11.93 ms | bit-identical, fnv1a `d7ddba1725c13731` in all runs |
| 1280x800 to 1920x1200 | 10.58 ms | 7.79 ms | not compared: this geometry differs between runs before the change too |

The table gives the lowest run median of three runs on each side.

## Known trade-off: small dispatches

pass8 of the FSR 4.1.2 network gets slower with the change. Its GPU time goes from 0.5875 ms to 0.6792 ms a
dispatch (1.156x, +0.092 ms). Both sides have a narrow spread: 0.5872 to 0.5886 ms before and 0.6790 to
0.6803 ms after. So the loss is not noise.

pass7 and pass8 compile to the same instruction changes: 336 fewer `v_add_u32` and 168 more `v_add3_u32`, from
the three-input grouping. pass7 gains (median 0.974x, fastest dispatch 0.912x). The difference between the two is
the grid. pass7 runs 2160 workgroups and pass8 runs 540. That the small grid causes the loss is an inference. The
lab run does not measure it.

The compiler does not know the dispatch size. A compile-time condition that keeps the old shape for pass8 also
keeps it for pass7, pass3, pass6 and pass11, which gain 0.827x to 0.933x by their fastest dispatch. We accept the
pass8 cost. On the full model through D3D12 the change saves 4.29 ms for each 1440p upscale
([M930](../facts/d3d.md#m930)).

## The commits

| Subject | Author |
|---------|--------|
| nir/algebraic: group the software 4x8 dot product for a three-input add | D-Ogi |
| aco: select imul24 and imad24_ir3 | dmorazasanchez |
| aco: sign-extend the scalar sources of imul24 and imad24_ir3 | D-Ogi |
| nir/algebraic: MAD24 lowering of sdot_4x8_iadd with a constant operand | dmorazasanchez |
| nir: gate the MAD24 lowering of sdot_4x8_iadd on has_imad24_ir3 | D-Ogi |
| nir/algebraic: use the MAD24 chain only for a shared run-time operand | D-Ogi |
| aco: add an offline instruction-count harness for packed 4x8 dot products | D-Ogi |
| aco: add a SPIR-V corpus mode to the packed dot count harness | D-Ogi |
| radv: lower signed packed dot products after one optimization round | dmorazasanchez |

The ported commits keep their upstream author and name the source commit. Two parts of the upstream patch are
not ported. The `RADV_GFX103` override reports gfx1013 as GFX10.3 and opens the hardware dot path. The dense
signed-dot prepass selects shaders by a fingerprint.

PROVENANCE: dmorazasanchez/bc250-fsr4 (MIT), branch v3, commit `2f0a283815dc863f8b9c365b984877b64d2b5963`,
file `bc250-fsr4-v3.patch`.
