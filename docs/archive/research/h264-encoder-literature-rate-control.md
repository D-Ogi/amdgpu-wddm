# Literature sweep: rate control and perceptual quality for a real-time GPU H.264 encoder

> This sweep has an independent re-check, [`h264-encoder-literature-rate-control-check.md`](h264-encoder-literature-rate-control-check.md). The check opened every source again
> on its own and recorded a verdict for each. Some verdicts are VERIFIED WITH CORRECTIONS, and this
> sweep's own text was not rewritten from them. Read the check before you act on a quote here.

Target: `driver/umd/mft-h264` (Media Foundation H.264 encoder MFT, D3D11 compute on BC-250 /
Cyan Skillfish / gfx1013). Written 2026-10-06. Research only: no code changed, nothing built, no lab access.

Reason for the sweep: train b19 on unit A met T1 (throughput) and T2 (GPU busy) and missed T3 (quality).
Measured on unit A, `<BC250_ROOT>\scratch\m15\video-encode\LAB-B19-RESULT.md`: 1080p serial 11.51 ms per picture
(GPU busy 6.53 ms, readback wait about 1 ms, CAVLC 3.42 ms on the CPU), pipelined 4.55 ms (219.8/s);
the Windows inbox software H.264 encoder on the same CPU is 1.7 to 1.8x faster than our pipelined path.
Quality against the inbox encoder at the same nominal rate: Y -1.3 to -2.3 dB, Cb and Cr -3.8 to -6.0 dB.

---

## 0. What this document is and is not

Every source below was read at the source: the paper PDF (extracted and quoted from the file), the
publisher page, or the code file itself. No source is summarised from a secondary description. Where
only an abstract was reachable, the entry says so and no number from the body is claimed.

Three sources I could not open are listed in section 12 with the reason.

A warning that applies to the whole document. Our T3 target is written in PSNR. Most of the perceptual
literature below (adaptive quantization, psy-RD, SSIM-RDO, macroblock-tree) trades PSNR away on purpose.
Three of the sources state this in their own words. So the sweep splits into two different jobs that
must not be mixed up:

* **Closing T3 as written (PSNR against the inbox encoder).** The levers are prediction and
  rate-distortion accuracy: partitions, intra in P pictures, a chroma term in the decision, a correct
  motion cost function, trellis or soft quantization. These raise PSNR at equal rate.
* **Perceptual quality beyond T3.** The levers are AQ, mb-tree, psy-RD, deblocking strength. Several of
  these will make the T3 number *worse* while making the picture look better. They are worth having, but
  not for closing T3, and not before T3 is closed, or the measurement will not be interpretable.

---

## 1. Our encoder as the object of the research

Facts established by reading the code, needed to make the mapping in each source entry concrete.
All paths relative to `driver/umd/mft-h264/`.

| Stage | Where | What it does today |
|---|---|---|
| Motion estimation | `shaders/cs_me.hlsl` | one 32-thread group per macroblock; 16x16 only; three integer stages (5x5 step 8, 5x5 step 2, 3x3 step 1, so reach +-21 full samples from (0,0)); then half and quarter sample refinement out of groupshared windows (9.8 KB of groupshared, occupancy limited); cost `SAD + gLambda * (abs(mvx) + abs(mvy))`, absolute, not relative to a predictor; optional snap back to (0,0) within `gSkipBias` |
| Mode decision | `shaders/cs_mb.hlsl` | I pictures: `Intra_16x16` with four luma modes plus four chroma modes, ranked by **SAD of the prediction residual**, luma and chroma ranked separately (`gRedCost`, threads 0..3 luma, 4..7 chroma). P pictures: every macroblock is `P_L0_16x16`; `gMbCost` is just the ME SAD read back from `MbInfo`. The file states the restriction: "Intra macroblocks inside a P picture are deliberately not produced: they would force the P picture onto the wavefront as well." |
| Quantisation | `shaders/h264_common.hlsli` `Quant4x4` | uniform deadzone, `f = (1 << qbits) / 3` for intra and `(1 << qbits) / 6` for inter, that is a rounding offset of 0.333 and 0.167 of a quantiser step. No trellis, no soft decision, no per-coefficient decision |
| Deblocking | `shaders/cs_deblock.hlsl` | full clause 8.7, conformant, one dispatch on the wave `t = mbx + 2*mby`. `gAlphaOffsetDiv2` and `gBetaOffsetDiv2` are already in the constant buffer and already used (`idxA = clamp(gQpY + gAlphaOffsetDiv2 * 2, 0, 51)`), and `h264_syntax.cpp` already writes both to the slice header. They are **not** exposed in `EncoderConfig` or `ICodecAPI`. Default is `deblocking = false`, which emits `disable_deblocking_filter_idc 1` |
| Entropy coding | `src/h264_cavlc.cpp` | CAVLC on the CPU, single slice per picture. `mb_qp_delta` is hardcoded to zero: "Rate control is per picture, so the quantiser never changes inside a slice" |
| Rate control | `src/encoder.cpp` `UpdateRateControl` | frame level. Leaky bucket of `kRcBufferFrames = 16` frames; a complexity estimate `bits * 2^(qp/6)`; an open loop term `want = 6 * log2(complexity / targetBits)`; a closed loop term of one quantiser step per frame of bits ahead of budget, gain `rcGainScale`, step clamped to `kRcMaxStep = 4`; `qpMin = 14`, `qpMax = 46`; IDR gets `qp - 3` |
| Motion cost weight | `src/encoder.cpp` | `gp.lambda = (1 + qp/8) * lambdaScale / 100`, `lambdaScale = 300` by default (measured: 3x beat 1x by 1.06 to 2.26 dB of luma at matched bytes). `gp.skipBias = (32 + qp*12) * skipBiasScale / 100`, `skipBiasScale = 0` by default |
| Chroma | `src/encoder.cpp` `ChromaQpFromLuma` | Table 8-15 of `qpY + chromaQpIndexOffset`; `chromaQpIndexOffset = 0` by default, settled before the parameter sets are built and never changed afterwards |
| Readback | `src/gpu_pipeline.cpp` | levels buffer `MbCount() * kLevelsWordsPerMb * 4` with `kLevelsWordsPerMb = 204` (two levels packed per 32-bit word), plus `MbCount() * kMbInfoWords * 4` with `kMbInfoWords = 16`. At 1080p (8160 macroblocks) that is about 6.7 MB of levels plus 0.5 MB of macroblock info per picture, read back in full whatever the coded block pattern dropped |
| Pipelining | `src/gpu_pipeline.cpp`, `src/encoder.cpp` | two slots (`SubmitFrame` / `RetireFrame`), one unless the client asked for `CODECAPI_AVLowLatencyMode`, overridable with `BC250_MFT_DEPTH` |

Three consequences of that table that the sources below speak to directly.

1. **Bit exactness is defined against the inbox decoder, not against our own previous output.** The
   `--encode` gate checks that our reconstruction is sample for sample what the inbox H.264 decoder
   produces. Changing the mode decision, the motion cost, the rate control or the quantiser rounding
   changes the bitstream but cannot break that gate, because the gate follows the levels we actually
   coded. What *can* break it is any change to the normative path: per macroblock QP (clause 8.7.2 takes
   `qPav` of the two macroblocks across an edge, and `cs_deblock.hlsl` currently says "Both quantisers
   are constant over the picture in this encoder, so qPav is just the picture QP"), chroma QP derivation,
   or the deblocking offsets. So the risk column in every entry below distinguishes "changes the bytes"
   from "risks conformance".
2. **Per macroblock QP is the gate on the whole AQ family.** It needs `mb_qp_delta` in
   `h264_cavlc.cpp`, a per macroblock `qpY` and `qpC` in `cs_mb.hlsl`, and a per edge `qPav` in
   `cs_deblock.hlsl`. That is one change with three conformance surfaces, not a tuning knob.
3. **Our motion search never leaves a +-21 sample box around (0,0) and has no predictor seeding.** For
   game content at 1080p60 that is the most likely single cause of the luma gap, and no amount of rate
   control will recover it.

---

## 2. Macroblock-tree: the published per-block quantizer algorithm, with its own measurements

**Citation.** J. Garrett-Glaser (Fiona Glaser), "A novel macroblock-tree algorithm for high-performance
optimization of dependent video coding in H.264/AVC", Department of Computer Science, Harvey Mudd
College. PDF read in full at `https://huyunf.github.io/blogs/2017/12/06/x264_slice_type_decision/MBtree%20paper.pdf`
(10 sections plus 13 references; the author's own paper describing the algorithm committed to x264).

**Claim.** A lookahead-driven per macroblock quantizer offset derived from how much future residual
depends on each macroblock, at negligible cost.

**Quotes.**

Abstract: "This novel macroblock-tree approach provides PSNR improvements of up to 1.2db and SSIM
improvements of up to 2.3db over existing fast ratecontrol algorithms at very low computational cost."

Section 5, the propagate step: "propagate_fraction. This is approximated by the formula
1 - intra_cost / inter_cost."; "The total amount of information that depends on this macroblock is equal
to (intra_cost + propagate_in)."; and the finish step:

> "Macroblock QP Delta = -strength * log2((intra_cost + propagate_cost) / intra_cost)"
> "where strength is an arbitrary factor derived from experimentation. Testing suggests that 2 is a
> near-optimal value for most videos."

Section 5, on the relationship with variance AQ: "It is important to note that x264 already has a
variance-based adaptive quantization (VAQ) algorithm implemented. Macroblock-tree adds quantizer deltas
on top of the effects of the existing adaptive quantization algorithm. While it might be tempting to
consider adaptive quantization a part of macroblock-tree to inflate its SSIM gain, they are two separate
algorithms and VAQ will not be covered or benchmarked in this paper."

Section 9, cost: "The cost of macroblock-tree in our implementation is approximately 28 clock cycles per
macroblock per lookahead frame. Even with a lookahead of size 50 frames, this is less than half the cost
of a single rate-distortion mode analysis in x264".

Section 10, the low-latency variant: "it is possible to implement macroblock-tree without a lookahead, by
propagating the propagate_cost from past frames to the current frame (i.e. in reverse). This is not as
effective as a future-based macroblock-tree, and has the problem of 'resetting' its history on every
scenecut, but may be applicable in some applications."

**Measured numbers and conditions.** Section 8: x264 r1924 (git `08d04a4d30b452faed3b763528611737d994b30b`),
`--preset slow`, one-pass constant quality, CRF swept 1 to 51, 50-frame lookahead, Viterbi adaptive
B-frame placement, `--tune psnr --psnr` and `--tune ssim --ssim` runs; mb-tree strength 1, 2, 3 encoded as
`--qcomp 0.8 / 0.6 / 0.4`; baselines `--no-mbtree` (qcompress) and `--no-mbtree --qcomp 1` (nominal constant
QP); all inputs forced to 25 fps to remove x264's framerate-based psychovisual tuning. Section 9 speed, on
foreman CIF, one thread, 1.866 GHz Core i7, gcc 4.6, rate matched to within 0.04 percent: with B frames
29.770 to 30.293 fps (+1.80 percent, mb-tree is *faster*), without B frames 29.363 to 28.705 fps
(-2.20 percent).

**Application to our encoder.** Direct in form, blocked in practice today:

* Which stage: a new pre-pass, plus the per macroblock QP plumbing of section 1 note 2.
* Expected gain: the paper's "up to 1.2 dB PSNR, up to 2.3 dB SSIM" is against x264's own
  `--qcomp`-based frame level control, not against a flat QP, and on long sequences with scene-to-scene
  complexity variation. On the short synthetic clips of our sweep the paper itself predicts little:
  "standard test sequences typically gain less from macroblock-tree than real-world content."
* Cost: 28 cycles per macroblock per lookahead frame. At 1080p that is 8160 x 28 = 0.23 Mcycle per
  lookahead frame; a 10-frame lookahead on one BC-250 Zen 2 core at ~3 GHz is under 1 ms per picture. Not
  free against our 4.55 ms pipelined budget.
* Blocker: a lookahead costs frames of latency, which is the opposite of what a Media Foundation capture
  client wants, and `CODECAPI_AVLowLatencyMode` already drives our pipeline depth to one. The
  **reverse (past-propagating) variant named in section 10 is the only version compatible with our
  latency contract**, and the author says it is weaker and resets at every scenecut.
* Risk: does not threaten the decoder oracle; does need per macroblock QP, so it carries all three
  conformance surfaces at once.
* Verdict: **not the first lever.** Park it behind per macroblock QP and behind T3.

---

## 3. x264's variance adaptive quantization, read in the source

**Citation.** x264, `encoder/ratecontrol.c`, function `x264_adaptive_quant_frame` and helpers
`ac_energy_plane` / `ac_energy_mb`; defaults in `common/base.c` `x264_param_default`. Read from
`https://raw.githubusercontent.com/mirror/x264/master/...` (official mirror of the videolan repository).
GPL-2.0-or-later. Read only; nothing is to be copied into our driver.

**Claim.** A macroblock QP offset proportional to the logarithm of the macroblock's AC energy
(variance), with the constants chosen so that the overall bitrate is unchanged.

**Quotes (verbatim from the file).**

The comment on the constants, which is the honest part worth repeating:

> `/* constants chosen to result in approximately the same overall bitrate as without AQ.`
> ` * FIXME: while they're written in 5 significant digits, they're only tuned to 2. */`

Mode 1 (`X264_AQ_VARIANCE`, the default):

```
strength = h->param.rc.f_aq_strength * 1.0397f;
...
uint32_t energy = ac_energy_mb( h, mb_x, mb_y, frame );
qp_adj = strength * (x264_log2( X264_MAX(energy, 1) ) - (14.427f + 2*(BIT_DEPTH-8)));
```

Modes 2 and 3 (`X264_AQ_AUTOVARIANCE`, `..._BIASED`) normalise against the frame:

```
uint32_t energy = ac_energy_mb( h, mb_x, mb_y, frame );
float qp_adj = powf( energy * bit_depth_correction + 1, 0.125f );
...
strength = h->param.rc.f_aq_strength * avg_adj;
avg_adj = avg_adj - 0.5f * (avg_adj_pow2 - 14.f) / avg_adj;
...
qp_adj = strength * (qp_adj - avg_adj);
```

`ac_energy_mb` sums the energy of **all planes present**, not luma only: for 4:2:0 it is
`ac_energy_plane(..., 0, 0, ...)` for luma plus `ac_energy_plane(..., 1, 1, ...)` for the interleaved
chroma plane. `ac_energy_var` computes `ssd - (sum * sum >> shift)`, the plain population variance.

**Defaults, from `common/base.c`:** `rc.i_aq_mode = X264_AQ_VARIANCE`, `rc.f_aq_strength = 1.0`,
`rc.b_mb_tree = 1`, `rc.i_lookahead = 40`, `rc.f_qcompress = 0.6`, `analyse.f_psy_rd = 1.0`,
`i_deblocking_filter_alphac0 = 0`, `i_deblocking_filter_beta = 0`,
`analyse.i_luma_deadzone[0] = 21` (inter), `[1] = 11` (intra).

**Numbers.** None in the source. The source's own comment says the constants are tuned to two significant
digits. This entry is a mechanism source, not a measurement source.

**Application to our encoder.**

* Which stage: a new per macroblock variance pass (cheap on the GPU, it is one reduction per macroblock
  over source samples already resident in `cs_me.hlsl`'s `gSrcMb` and `cs_mb.hlsl`'s `gSrcY`/`gSrcC`),
  feeding the per macroblock QP plumbing of section 1 note 2.
* **Note the formula's shape against ours.** x264's offset is `strength * (log2(energy) - 14.427)`. The
  14.427 is the pivot: a macroblock whose AC energy is 2^14.427 = 22000 gets no offset. Our encoder has
  no equivalent pivot because it has no per macroblock QP at all.
* Expected effect on T3: **negative.** AQ moves bits from flat areas to busy ones. MSE is minimised by
  the opposite allocation. Section 6 below quotes a paper that states exactly this for an SSIM-driven
  scheme, and section 2's own author reports mb-tree improving SSIM roughly twice as much as PSNR. Do not
  expect AQ to close a 1.3 to 2.3 dB PSNR gap; expect it to widen it.
* Cost: one variance reduction per macroblock on the GPU, negligible against 6.53 ms, plus the per
  macroblock QP conformance work.
* Risk: as section 1 note 2.

---

## 4. x264's lambda, motion vector cost, deadzone and psy-RD, read in the source

**Citation.** x264: `common/tables.c` (`x264_lambda_tab`, `x264_lambda2_tab`,
`x264_chroma_lambda2_offset_tab`), `encoder/analyse.c` (`x264_analyse_init_costs`, `init_costs`,
`mb_analyse_init_qp`), `common/set.c` (`x264_cqm_init`, the deadzone to bias conversion),
`common/quant.c` (`QUANT_ONE`), `encoder/rdo.c` (`ssd_plane`, `ssd_mb`). Same mirror and licence as
section 3.

**Claims and the exact code that carries them.**

*(a) Lambda is exponential in QP.* `x264_lambda_tab[qp]` is `round(2^((qp-12)/6))`: the table reads 1 at
qp 12, 2 at 18, 4 at 24, 5 at 26, 8 at 30, 16 at 36, 32 at 42. `x264_lambda2_tab[qp]` is
`round(0.9 * 2^((qp-12)/3) * 256)`: 230 at qp 12, 5851 at qp 26. So the SAD-domain weight is
`lambda = 2^((QP-12)/6)` and the SSD-domain weight is `lambda2 = 0.9 * 2^((QP-12)/3)`, the familiar
H.264 Lagrangian with 0.9 rather than the 0.85 of the standards literature.

*(b) The motion vector cost is lambda times the bits of the coded difference, logarithmic in its
magnitude.* `x264_analyse_init_costs`:

```
logs[0] = 0.718f;
for( int i = 1; i <= 2*4*mv_range; i++ )
    logs[i] = log2f( i+1 ) * 2.0f + 1.718f;
```

`init_costs`: `h->cost_mv[qp][i] = X264_MIN( (int)(lambda * logs[i] + .5f), UINT16_MAX )`. And in
`encoder/me.c`:

```
#define BITS_MVD( mx, my )\
    (p_cost_mvx[(mx)*4] + p_cost_mvy[(my)*4])
```

with `p_cost_mvx` and `p_cost_mvy` based at the **motion vector predictor**, so the index is the
difference actually coded as `mvd_l0`, not the absolute vector. The comment on the allocation says so:
"factor of 4 from qpel, 2 from sign, and 2 because mv can be opposite from mvp".

*(c) The quantiser rounding offset.* `common/quant.c`:

```
#define QUANT_ONE( coef, mf, f ) \
    (coef) = ((f) + (uint32_t)(coef)) * (mf) >> 16;
```

and `common/set.c`:

```
int deadzone[4] = { 32 - h->param.analyse.i_luma_deadzone[1],
                    32 - h->param.analyse.i_luma_deadzone[0],
                    32 - 11, 32 - 21 };
...
h->quant4_bias[i_list][q][i] = X264_MIN( DIV(deadzone[i_list]<<10, j), (1<<15)/j );
h->quant4_bias0[i_list][q][i] = (1<<15)/j;
```

`quant4_bias0 = (1<<15)/mf` is round-to-nearest, so it is the 0.5 reference; therefore
`quant4_bias = (deadzone << 10)/mf` is an offset of `deadzone/64` of a quantiser step. With the defaults
(`i_luma_deadzone[1] = 11` intra, `[0] = 21` inter) the four lists get `deadzone` 21, 11, 21, 11, that is
**0.328 of a step for intra and 0.172 for inter, luma and chroma alike.**

*(d) psy-RD adds a complexity-difference term to SSD.* `encoder/rdo.c`:

> `/* Psy RD distortion metric: SSD plus "Absolute Difference of Complexities" */`
> `/* SATD and SA8D are used to measure block complexity. */`

```
satd = abs(h->pixf.satd[size]( fdec, ... ) - dc - cached_satd( h, size, x, y ));
int64_t tmp = ((int64_t)satd * h->mb.i_psy_rd * h->mb.i_psy_rd_lambda + 128) >> 8;
...
return h->pixf.ssd[size](fenc, FENC_STRIDE, fdec, FDEC_STRIDE) + satd;
```

*(e) Chroma distortion is reweighted when psy is on, and the source says what it costs.*
`encoder/analyse.c` `mb_analyse_init_qp`:

> `/* Adjusting chroma lambda based on QP offset hurts PSNR but improves visual quality. */`

```
int chroma_offset_idx = X264_MIN( qp-effective_chroma_qp+12, MAX_CHROMA_LAMBDA_OFFSET );
h->mb.i_chroma_lambda2_offset = h->param.analyse.b_psy ? x264_chroma_lambda2_offset_tab[chroma_offset_idx] : 256;
```

`x264_chroma_lambda2_offset_tab` reads 256 at index 12 and doubles every six entries, that is
`256 * 2^((qp - qpc)/6)`: chroma distortion is weighted **up** by exactly the quantiser step ratio
whenever Table 8-15 puts the chroma QP below the luma QP. `ssd_mb` applies it:
`i_ssd += ((uint64_t)chroma_ssd * h->mb.i_chroma_lambda2_offset + 128) >> 8`.

**Application to our encoder.** Four separate findings, in order of how much they are worth.

**(4.1) Our motion cost function is the wrong shape and the wrong reference. This is the strongest
finding in the sweep.** Ours is `SAD + gLambda * (abs(mvx) + abs(mvy))` with the vector measured from
(0,0). x264's is `SAD + lambda * bits(mvd)` with the difference measured from the clause 8.4.1.3
predictor and the bit count logarithmic. Worked example at QP 26 with our defaults
(`gLambda = (1 + 26/8) * 3 = 12` per full sample per component) against x264 (`lambda = 5`,
`bits(d) = 2*log2(d+1) + 1.718` in quarter-sample units):

| case | our penalty | x264 penalty | ratio |
|---|---|---|---|
| vector (0,0), predictor (0,0) | 0 | 2 x 5 x 0.718 = 7 | 0.0x |
| vector (1,1) full sample, predictor (0,0) | 12 + 12 = 24 | 2 x 5 x (2 log2 5 + 1.718) = 55 | 0.44x |
| vector (16,16) full sample, predictor (0,0) | 12 x 32 = 384 | 2 x 5 x (2 log2 65 + 1.718) = 138 | 2.8x |
| vector (16,16), predictor (16,16) (a pan, every macroblock after the first) | 384 | 7 | **55x** |

So on panning content we charge up to fifty-five times the real bit cost of the correct vector, every
macroblock, and the search is pushed back toward (0,0) exactly where it should not be. That is consistent
with the measured sweep recorded in `src/encoder.h`, where raising `lambdaScale` from 1.0 to 3.0 *gained*
1.06 to 2.26 dB: the right fix is not a bigger constant on the wrong shape.
Also note our lambda grows linearly in QP (`1 + qp/8`, a factor of 2 over QP 16 to 40) where the
literature's grows as `2^(QP/6)` (a factor of 16 over the same range), so one tuned constant cannot be
right at both ends of the rate range.
Where: `shaders/cs_me.hlsl` `Reduce`/`gCost`, and `src/encoder.cpp` for the lambda.
Cost: a predictor needs the neighbours' vectors, which the single-dispatch P-picture ME deliberately does
not have. Two ways out, both cheap: (i) seed and reference the **co-located vector of the previous
picture**, which is already in a buffer and needs no intra-picture dependency, the standard trick in GPU
ME; (ii) move the ME to the `mbx + mby` anti-diagonal the intra pass already uses, which costs
parallelism. Option (i) first.
Expected gain: unquantified by this source, but it is the term that currently fights the search on every
panning or scrolling scene, which is most game content. Risk: changes the bytes, cannot break the decoder
oracle.

**(4.2) Our mode decision uses SAD where the literature uses SATD plus a bit term.** See also section 5,
which documents x264's `SATD0 = SATD + lambda * bits`. Our `cs_mb.hlsl` ranks the four `Intra_16x16`
modes and the four chroma modes by SAD of the prediction residual, and does not add the mode's own bit
cost at all. SAD over a residual that is about to be transformed systematically misranks modes whose
residual has the same absolute sum but different spectral concentration. A 4x4 Hadamard over the residual
is a handful of instructions per 4x4 block and the data is already in groupshared memory.
Where: `shaders/cs_mb.hlsl`, the `gRedCost` block. Cost: small on the GPU. Risk: bytes only.

**(4.3) Our quantiser rounding offsets already match x264's defaults. This is a dead lever, and that is
worth knowing.** Ours are 1/3 = 0.333 (intra) and 1/6 = 0.167 (inter). x264's defaults work out to 0.328
and 0.172. Within three percent on both. Do not spend time on the deadzone; the chroma gap is not there.

**(4.4) Chroma is missing from our rate-distortion decision in two places, and x264 says what each is
worth.** First, the mode decision: `ssd_mb` adds chroma SSD to luma SSD for every candidate, weighted;
our P-picture decision has no chroma term at all and our I-picture decision ranks luma and chroma
independently rather than jointly. Second, the weight: x264 multiplies chroma distortion by
`2^((qp - qpc)/6)` when psy is on, and its own comment warns that doing so "hurts PSNR but improves
visual quality". For a target written in chroma PSNR, the useful half of this is the *presence* of a
chroma term in the decision, not the perceptual weight.
Where: `shaders/cs_mb.hlsl` (a joint cost), and `src/encoder.h` (`chromaQpIndexOffset`, currently 0,
range -12..12, already plumbed into the PPS and into `ChromaQpFromLuma`).

---

## 5. x264 against the H.264 reference encoder: the numbers for mode decision and trellis quantization

**Citation.** L. Merritt and R. Vanam, "x264: A High Performance H.264/AVC Encoder", marked
"In Preparation", Dept. of Electrical Engineering, University of Washington. PDF read in full at
`http://akuvian.org/src/x264/overview_x264_v8_5.pdf` (this is reference [5] of the macroblock-tree paper
of section 2).

**Claim.** A SATD-plus-bits mode decision with early termination, plus trellis quantization, reaches
JM-class compression at roughly fifty times the speed.

**Quotes.**

Section 1: "we compare the performance of the JM encoder (ver. 10.2) with x264 (ver 0.47.534) and show
that x264 is about 50 times faster and provides bitrates within 5% of JM for the same PSNR."

Section 2.2 step 10, the cost function:

> "Run quarter-pixel precision with SATD0, defined as
>  SATD0 = SATD + lambda * bits (3)
> where SATD is the sum of absolute Hadamard-transformed difference"

Section 2.3: "In x264, the mode decision to choose macroblock partition is a hybrid of SATD0 and
rate-distortion optimization."; "the intra modes, with I16x16 first. The selection of intra directions is
decided by SATD0."; and the early termination: "the cost of P16x8 and P8x16 are estimated as being equal
to (SATD of P8x8) + 0.5*(bit cost of P8x8). If this estimate is worse than the known SATD0 cost of
P16x16, then skip P16x8".

Section 2.4.1 on the deadzone, which is the quantiser we use: "The conventional quantization approach
used in most codecs is the uniform deadzone. This works by simple division, biased towards zero. The bias
accounts for the fact that smaller coefficients take fewer bits. This is RD-optimal assuming the
coefficients are independent and follow a Laplacian distribution"; and why that assumption is wrong:
"the vast majority of coefficients are zeros or ones, so the dependence between coefficients matters more
than the distribution of one coefficient in isolation".

Section 3, the trellis measurement: "on an average Trellis-2 performs 0.7 % better than Trellis-1 and
5 % better than Trellis-0 in terms of bitrate. This improved performance of Trellis-2 comes with an
increased cost in encoding time. On an average, Trellis-2 takes 27 % more time than Trellis-1 and 31%
more time than Trellis-0."

Section 2.2, the search: x264's integer search is seeded from a predictor set and uses "Adaptive radius:
Select the search range for step (7), based on the best SAD so far"; "The default search range is 16.
This may decrease as low as 12 if SAD is small and the predictors are similar, and may increase as high as
24 if SAD is large and the predictors differ much."

**Measured numbers and conditions.** 19 CIF sequences; five reference frames in both encoders; equal QP;
QP swept 18 to 36 in steps of 3; x264 using Trellis-2. Result: about 50x faster than JM 10.2 on average;
bitrate within 5 percent of JM at equal PSNR, better than JM above 38 dB. Trellis-2 against the uniform
deadzone: 5 percent bitrate at 31 percent more encode time. Figures 2(a)-(d) carry the curves; the axes
are average encoding time per frame and relative bitrate against average PSNR.

**Application to our encoder.**

* **Trellis / soft decision quantization bounds a real lever at 5 percent of rate**, measured against
  exactly the quantiser we use, for 31 percent more encoder time. Caveat that matters for us: x264's
  trellis is written against CABAC rate estimates, and `encoder/rdo.c` exposes
  `trellis_cabac_4x4` / `..._8x8` entry points. A CAVLC trellis has to re-derive the rate model from the
  clause 9.2 tables we already have in `src/h264_tables.cpp`, which is work but not research. Where: a
  new pass between `cs_mb.hlsl`'s quantisation and the CPU CAVLC, or on the CPU inside
  `src/h264_cavlc.cpp` before the levels are written. Risk: bytes only. Cost: 31 percent of *x264's*
  encode time is not 31 percent of ours, because our mode decision is far cheaper; the per-coefficient
  work is the same order as the quantisation itself.
* **The search range finding directly indicts ours.** x264's *default* range is 16 but it searches from a
  predictor set and adapts the radius up to 24 around that seed. Our search is a fixed +-21 box around
  (0,0) with no seed. The reachable displacement is therefore not comparable: x264 can follow a 200 pixel
  per frame pan, we cannot follow 22. Where: `shaders/cs_me.hlsl` stage 0. Cheapest fix: seed `cx, cy`
  from the co-located previous-picture vector and from the left and above vectors of the previous
  picture's field (no intra-picture dependency), then keep the existing three-stage refinement. Cost:
  one extra `MbInfo` read per macroblock, nothing measurable. Expected gain: unquantified here, but this
  is the difference between finding and not finding the motion at all.
* **Partition modes**: the paper's SATD0 estimate for P16x8 and P8x16 from the P8x8 result is the
  cheap way to get partitions without a full search per shape, and would fit our one-group-per-macroblock
  layout: the 32 lanes already compute per-8x8 partial sums in `cs_me.hlsl`'s `gPart`.

---

## 6. SSIM-motivated rate-distortion optimisation: measured gain, and the PSNR it costs

**Citation.** S. Wang, A. Rehman, Z. Wang, S. Ma, W. Gao, "SSIM-Motivated Rate-Distortion Optimization
for Video Coding", IEEE Transactions on Circuits and Systems for Video Technology, vol. 22, no. 4,
pp. 516-529, April 2012. DOI 10.1109/TCSVT.2012.2187915. PDF read in full at
`https://ece.uwaterloo.ca/~z70wang/publications/TCSVT_SSIM_RDO.pdf`.

**Claim.** Replacing MSE with SSIM in the H.264/AVC Lagrangian, with an adaptive frame-level multiplier
from a reduced-reference SSIM estimate and a macroblock-level adjustment from motion information,
substantially reduces rate at equal SSIM.

**Quotes.**

Section VII-B: "For IPP GoP structure, on average 15% rate reduction for fixed SSIM and 16% rate
reduction while fixing weighted SSIM are achieved for both QCIF and CIF sequences. When the GoP structure
is IBP, the rate reductions are 9% on average for fixed SSIM and 10% on average for fixed weighted SSIM."

The caveat our T3 target has to respect, same section: "We have also compared the performance in terms of
PSNR of the luminance component, which is shown in Tables II and III. ... for some sequences, such as
Akiyo and Container, PSNR increases. However, **on average PSNR decreases because our optimization
objective is SSIM rather than PSNR**."

And again at Fig. 10: "since our proposed RDO scheme is based on SSIM index optimization, higher SSIM and
lower PSNR are achieved."

Cost, Table VI: "On average the computation overhead is 6.3%".

**Measured numbers and conditions.** H.264/AVC reference software; "all available inter and intra modes
are enabled, five reference frames, one I frame followed by 99 inter frames, high complexity RDO, and the
fixed QPs are set from 28 to 40"; both CABAC and CAVLC variants reported (Tables II and III); SSIM window
8x8; curve differences computed with the Bjontegaard method. Best case: "Rate reduction peaks for
sequences with slow motion such as Bridge, in which case 35% of the bits can be saved for the same SSIM
value". Against two prior SSIM-based RDO schemes the paper reports 12.39 percent versus 9.79 percent at
QP1 and 16.28 percent versus 11.58 percent at QP2.

**Application to our encoder.**

* This is the clearest single piece of evidence that **the perceptual family of levers is the wrong
  family for T3 as written**. A scheme that saves 15 percent of rate at equal SSIM loses PSNR on average.
  If we adopt any of it, T3 has to be restated in SSIM or VMAF first, with the inbox encoder measured on
  the same metric.
* The mechanism that is *metric-neutral* and worth taking: a per picture Lagrange multiplier derived from
  an estimate of the picture's distortion, rather than a fixed function of QP. Our
  `gp.lambda = (1 + qp/8) * 3` is a fixed function of QP; section 4.1 already says its shape is wrong.
* Cost reference point: 6.3 percent of encoder time for a full SSIM-domain RDO in a software reference
  encoder. Our equivalent per-candidate cost would land in `cs_mb.hlsl`, where it is cheaper in relative
  terms because our candidate set is tiny.

---

## 7. psy-RD measured subjectively: an honest negative result

**Citation.** Z. Duanmu, K. Zeng, Z. Wang, M. Eisapour, "Perceptual Evaluation of Psychovisual
Rate-Distortion Enhancement in Video Coding", IS&T Electronic Imaging - Human Vision and Electronic
Imaging, Burlingame CA, Jan-Feb 2017. PDF read in full at
`https://ece.uwaterloo.ca/~z70wang/publications/HVEI17_PsyRD.pdf`.

**Claim.** x264's psy-RD, on by default, does not on average improve subjective quality; it makes it
worse and raises the bitrate.

**Quotes.**

Abstract: "Unfortunately, the impact of Psy-RD optimization on video quality does not appear to be
encouraging. Somewhat surprisingly, the perceptual quality gain of Psy-RD ON versus Psy-RD OFF cases is
negative on average. Our results suggest that Psy-RD optimization should be used with caution."

Analysis section: "the stronger the negative impact. 2) Psy-RD tends to increase the actual bitrate of
videos as shown in Table 2. The larger of the Psy-RD strength, the larger the bitrate of the encoded
video. 3) The impact of Psy-RD is content dependent. We observe that Psy-RD often improves the quality of
complex-scene videos, and the gain peaks at Psy-RD strength 0.6. On the other hand, the quality of most
of videos is hurt by Psy-RD, especially for the videos with low spatial and temporal complexity."

And: "Consistent MOS loss is observed from the table, which means turning Psy-RD on would on
[average lower quality]".

**Measured numbers and conditions.** 15 source clips, 1280x720, 10 s, 25 fps, diverse content
(animation, people, nature, architecture, screen content, game footage). x264 at four bitrates
(250, 500, 950, 1300 kbps) and four psy-RD strengths (0, 0.6, 1.0, 2.0), 240 test sequences. 20 naive
observers, one removed as an outlier, 19 valid; 0-100 continuous scale; ITU-T BT.500 viewing conditions,
2560x1600 calibrated LCD; randomised order. Table 1 reports MOS gain against psy-RD OFF for the twelve
(bitrate, strength) cells, and **all twelve printed values are negative**, ranging from -0.2200 to
-4.6433 MOS points. Table 2 shows the delivered bitrate rising with strength at every target: at the
250 kbps target 253.44 kbit/s at strength 0 up to 264.84 at strength 2; at 500, 493.00 up to 510.02; at
950, 953.85 up to 969.68. Among nine full-reference and one no-reference objective metrics, SSIM,
MS-SSIM, SSIMplus, VIF and VQM "performs reasonably and almost equally well", ranked by cost from
SSIMplus (lowest) through SSIM, MS-SSIM, VIF to VQM; the no-reference model (BRISQUE) "does not provide
adequate predictions".

**Application to our encoder.** Short and useful: **do not implement psy-RD.** It is the one item on the
perceptual list with a published, controlled subjective study against it, it costs bits, and it would make
T3 worse on both counts. If a perceptual term is wanted later, section 6's SSIM-domain RDO has measured
gains and this one does not. Saved effort: the whole `ssd_plane` Hadamard-complexity machinery of
section 4(d), which would have had to be built in `cs_mb.hlsl`.

---

## 8. Chroma in adaptive quantization: two measured studies

### 8.1 Chroma variance in the QP decision

**Citation.** L. Prangnell, "Spatiotemporal Adaptive Quantization for Video Compression Applications",
arXiv:2005.07925 (Department of Computer Science, University of Warwick). PDF read in full.

**Claim.** HEVC's AdaptiveQP sets the coding unit QP from luma variance only; adding chroma variance and
a temporal masking term gives large BD-rate reductions on all three components.

**Quotes.** Abstract: "It is designed to perceptually adjust the QP in Y, Cb and Cr Coding Blocks (CBs)
based only on the variance of samples in a luma CB."; "Our method achieves a maximum BD-Rate reduction of
23.1% (Y), 26.7% (Cr) and 25.2% (Cb). Furthermore, a maximum encoding time reduction of 4.4% is achieved."
Section 1: "This technique takes into account only the variance of luma samples; in addition, it does not
account for motion information in a CU, thus leaving room for improvement."

**Measured numbers and conditions.** HEVC HM 16.7, Random Access, QPs 22/27/32/37 per the JCT-VC common
test conditions; FourPeople and KristenAndSara (720p), ParkScene (1080p), Traffic (1600p), each in 4:4:4,
4:2:2, 4:2:0 and 4:0:0. Anchor is the AdaptiveQP tool, not a no-AQ encoder. Table 1, the 4:2:0 column,
which is our format:

| sequence (4:2:0, 8-bit) | BD-rate Y | Cb | Cr | encode time | decode time |
|---|---|---|---|---|---|
| FourPeople | -13.2 % | -15.6 % | -16.9 % | -2.0 % | +0.2 % |
| KristenAndSara | -22.4 % | -27.9 % | -24.6 % | -0.8 % | -0.4 % |
| ParkScene | -6.5 % | -15.2 % | -16.0 % | -0.9 % | +3.0 % |
| Traffic | -4.8 % | -13.4 % | -17.9 % | -0.8 % | +0.2 % |

The 4:0:0 column isolates the non-chroma part of the method (motion plus the lambda-QP refinement) and
gives only -6.9 to -3.0 percent Y, so the chroma-variance contribution is the larger half of the 4:2:0
numbers. The paper does not ablate the three components individually, so no single number can be
attributed to chroma variance alone.

### 8.2 Cross-colour-channel AQ, and the chroma QP ceiling

**Citation.** L. Prangnell, M. Hernandez-Cabronero, V. Sanchez, "Cross-Color Channel Perceptually
Adaptive Quantization for HEVC", arXiv:1612.07893 (University of Warwick). PDF read in full.

**Quotes.** Abstract: "Our technique achieves considerable coding efficiency improvements, with maximum
BD-Rate reductions of 15.9% (Y), 13.1% (Cr) and 16.1% (Cb) in addition to a maximum decoding time
reduction of 11.0%." Section on the model: "the HVS is typically more sensitive to gradations to the data
in the luma channel. Moreover, the data in the chroma channels is susceptible to severe artifacts caused
by very high levels of quantization. Therefore, in the HEVC standard the maximum QP permitted for chroma
data is QP = 39 (chroma QP offset) for YCbCr 4:2:0 chroma subsampled input video data."

**Measured numbers and conditions.** HM 16, All Intra and Random Access, 4:4:4 / 4:2:2 / 4:2:0 JCT-VC
sequences, anchor AdaptiveQP, plus a subjective paired comparison. For 4:2:0 All Intra, KristenAndSara
8-bit, Main profile: -14.3 percent Y, -12.3 percent Cb, -12.5 percent Cr. For 4:2:0 Random Access, same
sequence: -15.5 percent Y, -12.8 percent Cb, -11.8 percent Cr.

**Application to our encoder (both 8.1 and 8.2).**

* Our encoder's decision is luma-only in exactly the way these two papers identify as the defect. The
  defect is the same; the fix they measure is per-block QP from chroma variance, which needs our per
  macroblock QP plumbing.
* **But the sign is wrong for T3.** These are BD-rate numbers against an AQ anchor in HEVC, measured with
  PSNR as the distortion axis, so they are a genuine rate-quality gain and not only a perceptual one.
  That makes 8.1 and 8.2 the *only* AQ sources in this sweep whose metric is compatible with T3. Treat
  them as the evidence that per macroblock QP driven by **all three planes** is worth its conformance
  cost, and the evidence against a luma-only variance AQ in the x264 style (section 3), which would
  reproduce the very anchor these papers beat.
* Chroma QP ceiling note: both papers lean on the HEVC chroma QP mapping saturating around 39. H.264's
  Table 8-15 does the same thing (our `kChromaQpFromQpi30` already implements it), so at our `qpMax = 46`
  the chroma quantiser is already well below the luma one. **Our chroma deficit at QP near 26 is
  therefore not a chroma QP mapping problem**, and `chromaQpIndexOffset` is a tuning knob, not the cause.

---

## 9. Peer work on the same silicon: two BC-250 compute encoders with measured numbers

These two are the most directly comparable sources in the sweep: the same APU, the same absent VCN, the
same "compute shaders plus CPU entropy coding" architecture, and in one case the same 16x16-only mode set.
**Both are GPL-3.0. They are read here as measurement and as prior art only; no code or shader from either
may be copied into our driver.**

### 9.1 `Shalasere/bc250-vulkan-encode-stopgap`

**Citation.** Shalasere, `bc250-vulkan-encode-stopgap`, README.md and docs/DEVLOG.md sections 19 and 20,
read at `https://raw.githubusercontent.com/Shalasere/bc250-vulkan-encode-stopgap/main/...`. A Vulkan
compute H.264 and HEVC VA-API driver for the AMD BC-250.

**Claims and quotes.**

Scope, which matches ours almost exactly: "**CABAC** (`feature/h264-cabac`, ITU-T 9.3, adapted from x264,
GPL-2.0-or-later): auto-selected for Main/High profile or via `BC250_USE_CABAC=1`. **10-13% smaller output
than CAVLC at matched QP, ~28% more CPU**, still well above real-time. Scope: I_16x16 intra / P_L0_16x16
inter only."

Where the time goes: "GPU shaders are <1.5% of frame time; the remaining bottlenecks are CPU/memory-side."

DEVLOG section 19.1, measured with the shipped driver, 300 frames, mean over P frames, 1440p:

| stage | testsrc (QP 12) | testsrc2 (QP 25) |
|---|---|---|
| CAVLC (CPU) | 5.44 ms (40 %) | 11.19 ms (58 %) |
| `shadow_copy` (CPU) | 3.03 ms (22 %) | 2.92 ms (15 %) |
| GPU total | 4.08 ms (30 %) | 4.93 ms (25 %) |
| wall | 13.69 ms, 71.9 fps | 19.26 ms, 51.3 fps |

The diagnosis, section 19: "CAVLC is not compute-bound on entropy coding, it is bandwidth-bound on
scanning a buffer that is almost entirely zeroes." And the headroom they measured before writing any code:
"**89.8-95.8% of all blocks are entirely zero** (95.6-99.4% have zero AC)."

Section 19.5, result of exporting a per-4x4-block nonzero bitmask from the quantisation shader instead of
re-deriving it on the CPU: CAVLC -17.5 to -20.7 percent, wall -6.7 to -11.2 percent, at 1080p
8.82 to 8.15 ms (testsrc) and 12.84 to 11.53 ms (testsrc2), with quality unchanged (PSNR ranges fully
overlapping).

Section 20, removing a 16x overcopy of the pre-quantisation coefficient buffer: host-visible staging
44.2 MB to 2.8 MB, GPU-to-host copy per frame 22.1 MB to 1.4 MB, throughput +30 percent at 1440p
(19.532 ms to 14.998 ms moving, 14.100 ms to 10.835 ms static), verified byte-identical against the
previous commit across five configurations. The mechanism is the part worth reading twice:

> "But only ~1.2 ms of it is the copy that was actually removed. **The larger share is CAVLC running 37% /
> 30% faster despite not being touched by this diff at all.** ... The inferred mechanism for the CAVLC
> gain is cache residency: a 22 MB/frame `memcpy` was streaming through and evicting the `quant_levels` +
> nonzero-mask working set that CAVLC reads immediately afterwards."

And on mapped memory types, section 19.2: "A `HOST_CACHED` Vulkan mapping still does not behave like
ordinary cacheable RAM for scattered CPU reads on this hardware." Their 2x2 measurement: with cached
staging 13.76 ms, with uncached staging 329.92 ms.

Two further measured facts worth carrying: "`qp_min=12` is deliberate, and lowering it is a measured net
loss ... taking the floor to 8 spent 14% more bits for **-22% encode throughput and no visible quality
change**"; and on GPU contention, "a heavy compute-bound title can cost this encoder up to ~45x
(measured 66.2 -> 1.48 fps at 1440p under a synthetic worst-case load generator)".

Finally, two honest limitations that we do not share and should not acquire: "Output is not
bit-reproducible on moving content" and "in-loop deblocking is **luma-only** while the bitstream signals
`disable_deblocking_filter_idc=0`; and I-slice intra prediction reads *source* rather than reconstructed
neighbours" (together about 3.7 dB of per-GOP drift).

**Application to our encoder.** This is the richest performance entry in the sweep.

* **CABAC is worth 10 to 13 percent of rate at matched QP for about 28 percent more CPU, measured on this
  exact silicon with our exact mode set.** Our MFT refuses CABAC today (`--mft` checks that it is
  "refused honestly"). 10 to 13 percent of rate at matched QP converts to roughly 0.5 to 0.8 dB at a
  typical rate-distortion slope, which is a third to a half of our luma gap. Where: a new
  `src/h264_cabac.cpp` next to `src/h264_cavlc.cpp`, plus `entropy_coding_mode_flag` in the PPS and the
  Main profile in the SPS. Cost: 28 percent of our 3.42 ms, that is about 1 ms per 1080p picture, which
  our two-slot pipeline already hides behind the GPU. Risk: large new normative surface (clause 9.3), but
  the `--encode` oracle against the inbox decoder catches every error in it. This is the single
  highest-value item that needs no GPU work at all.
* **The readback lever is live for us and partly unexploited.** We already avoid their worst mistake: we
  never stage the pre-quantisation coefficients, and our `MbInfo` already carries per-block non-zero
  counts (`gNnz`, written by `cs_mb.hlsl`). What we still do is read back all 204 words per macroblock of
  levels, about 6.7 MB per 1080p picture, including the blocks that the coded block pattern dropped. With
  their measured zero density (89.8 to 95.8 percent of blocks entirely zero) the live fraction is under
  1 MB. Two changes, in increasing order of work: (i) have `h264_cavlc.cpp` consult the per-block non-zero
  count before touching the levels of a block, which is a CPU-side change only; (ii) compact the levels on
  the GPU so only live blocks cross the bus. Their measurement says to expect the *second-order* effect to
  dominate: untouched CAVLC ran 30 to 37 percent faster purely from cache residency. Against our
  3.42 ms CAVLC plus about 1 ms readback wait, 30 percent is about 1 ms per 1080p picture, which is a
  quarter of the 1.7x gap to the inbox encoder. Where: `src/gpu_pipeline.cpp` (the copy and the two
  `memcpy` calls), `src/h264_cavlc.cpp`, `shaders/cs_mb.hlsl`. Risk: bytes must not change at all, and the
  `--encode` gate already proves that.
* **Their mapped-memory finding is a warning for ours.** Scattered CPU reads over a mapped
  `D3D11_USAGE_STAGING` buffer may behave like their uncached case. Our `src/encoder.h` already separates
  `readbackMs` from `mapWaitMs`, so the transfer cost is already visible; the open question is whether
  CAVLC reads the mapped pointer directly or a copy. If directly, their 13.76 against 329.92 ms result
  says to measure a `memcpy`-then-scan variant before anything cleverer.
* **GPU contention is the elephant for the game use case.** 66.2 to 1.48 fps under a compute-bound load.
  Our encoder would be sharing the same 40 CUs with The Witcher 3. Any throughput claim measured on an
  idle GPU does not transfer.

### 9.2 `MTSistemi/bc250-vaapi`

**Citation.** `MTSistemi/bc250-vaapi` (a fork lineage of `simpmix/bc250-encoding-decoding-fix`),
README.md, `docs/multicore_cavlc_design.md`, `docs/rate_control_audit.md`, read at
`https://raw.githubusercontent.com/MTSistemi/bc250-vaapi/main/...`.

**Claims, quotes and numbers.**

*(a) Throughput on this silicon.* README: "H.264 (Vulkan Compute): 640x480: 267 fps; 720p: 179 fps;
1080p: 100-134 fps; 1440p: 67-80 fps"; "Game Streaming Overhead: Only ~4.5% total GPU impact during
active 60 FPS gaming".

*(b) Their stated cause of a chroma PSNR deficit, for HEVC:* "Chroma Fidelity: Bit-exact non-linear
Table 8-10 QP mapping eliminates the standard chroma PSNR deficit." Worth recording because an
independent project on the same part names "the standard chroma PSNR deficit" and attributes it to the
chroma QP mapping. Our H.264 equivalent (Table 8-15) is already implemented, so this particular cause is
already excluded for us.

*(c) A measured CAVLC cost and the Amdahl argument.* `docs/multicore_cavlc_design.md`, measured with
`tools/perf_test.sh`, 60 frames:

| resolution | MBs/frame | GPU total | CPU CAVLC | frame wall | CAVLC share |
|---|---|---|---|---|---|
| 640x480 | 1200 | 0.459 ms | 54.534 ms | 55.465 ms | 98.3 % |
| 1280x720 | 3600 | 0.993 ms | 164.357 ms | 165.865 ms | 99.1 % |

"Dividing CAVLC time by MB count: 54.534 ms / 1200 MB = **45.4 microseconds/MB** (640x480),
164.357 ms / 3600 MB = **45.7 microseconds/MB** (720p) - a flat per-MB cost independent of resolution".
Their Amdahl table for slice-parallel CAVLC: at p = 0.98, N=4 gives 3.8x, N=8 gives 7.0x, N=16 gives
12.3x; at p = 0.60, N=16 gives only 2.3x.

*(d) The measured quality cost of slicing, which is the warning.* Same document, `tools/quality_test.sh`,
640x480, 25 fps, 50 frames, 4 Mbps, same build for every point:

| slices per frame | PSNR avg | SSIM | delta PSNR vs 1 slice |
|---|---|---|---|
| 1 | 37.66 dB | 0.9918 | baseline |
| 2 | 34.00 dB | 0.9886 | -3.66 dB |
| 4 | 29.27 dB | 0.9777 | -8.39 dB |
| 8 | 29.67 dB | 0.9783 | -8.00 dB |
| 16 | 28.91 dB | 0.9729 | -8.75 dB |

With their own caveat: "each additional slice boundary is also an additional point where CAVLC context
sharing, intra prediction, and deblocking are cut off - a genuine quality cost, independent of and
additional to the threading question", and "this document is not asserting that tradeoff is automatically
worth it, only reporting the real curve".

*(e) A rate control failure mode we have already avoided.* `docs/rate_control_audit.md`: "its QP
adjustment range is mathematically clamped to a narrow band (~+-6-8 QP steps) around a **hardcoded** QP
of 26, regardless of what bitrate is requested"; "`base_qp` is the crux: it is the one number that should
encode 'what QP roughly hits this target bitrate for this resolution,' and it is a literal constant (`26`
...) set once in `rc_init()` and never touched again - not derived from `target_bitrate`,
`target_bits_per_frame`, width x height, or anything else"; measured consequence: "measured output bitrate
for the same content varies by <5% across a 20x spread of requested bitrates, while switching content
complexity at a *fixed* requested bitrate swings output bitrate by >6x."

**Application to our encoder.**

* **Our CAVLC is about two orders of magnitude faster per macroblock than theirs.** 3.42 ms over 8160
  macroblocks at 1080p is 0.42 microseconds per macroblock against their measured 45.4 to 45.7. That
  reframes the CAVLC lever: multi-slice threading, which is the whole point of their document, buys us far
  less in absolute terms, and it costs the dB in their table (d). **Do not go multi-slice for speed.**
  Our single-slice-per-picture choice is correct and should be defended.
* Their throughput (1080p 100-134 fps) against ours (1080p 219.8/s pipelined, 86.9/s serial) says our
  architecture is already ahead on this silicon at the pipelined operating point. Conditions differ
  (Vulkan against D3D11, their encoder has features ours lacks and vice versa, their figures are on an
  idle GPU), so this is a sanity check, not a benchmark.
* **Their rate control audit is a checklist our `UpdateRateControl` already passes**, and that is worth
  recording as evidence rather than re-deriving. We do derive the operating quantiser from content:
  `want = 6 * log2(m_complexity / targetBits)` with `m_complexity` tracked as `bits * 2^(qp/6)`, which is
  precisely the "relocate the fixed point" that their loop cannot do. The two residual risks their audit
  points at are worth checking on our side: (i) our closed-loop step is clamped to `kRcMaxStep = 4` per
  frame and the bucket to 16 frames, so the convergence rate, not the reachable range, is our bound;
  (ii) they found a VBR target scaled by 2x because `target_percentage` was never read, and our MFT
  takes `meanBitRate` and `maxBitRate` from `ICodecAPI` - worth one check that a client setting
  `CODECAPI_AVEncCommonMeanBitRate` plus `..._MaxBitRate` is interpreted the way Media Foundation
  intends.

---

## 10. Metrics: what to measure T3 with, and how to compute the delta

### 10.1 SSIM

**Citation.** Z. Wang, A. C. Bovik, H. R. Sheikh, E. P. Simoncelli, "Image Quality Assessment: From Error
Visibility to Structural Similarity", IEEE Transactions on Image Processing, vol. 13, no. 4,
pp. 600-612, April 2004. DOI 10.1109/TIP.2003.819861. PDF read in full at
`https://ece.uwaterloo.ca/~z70wang/publications/ssim.pdf`.

**Claim and the quote that matters for us.** Fig. 2's caption is the whole argument in one line:
"Comparison of 'Boat' images with different types of distortions, all with MSE = 210." The three-term
decomposition is equations (6), (9) and the luminance/contrast/structure product; the stabilising
constants are `C1 = (K1 L)^2` and `C2 = (K2 L)^2`.

**Application.** If T3 is ever restated perceptually, SSIM is the cheapest credible axis, and section 7
measured it as one of the five metrics that "performs reasonably and almost equally well" on exactly the
kind of psychovisual change we might make. `libvmaf` already computes it (section 10.2), so no new code.

### 10.2 VMAF

**Citation.** Netflix, `vmaf` repository, README.md, read at
`https://raw.githubusercontent.com/Netflix/vmaf/master/README.md`. BSD+Patent licence (changed from
Apache 2.0 on 2020-02-27 per the README's own news entry).

**Claims and quotes.** "This software package includes a stand-alone C library `libvmaf` and its wrapping
Python library."; "Also included in `libvmaf` are implementations of several other metrics: PSNR,
PSNR-HVS, SSIM, MS-SSIM and CIEDE2000."; a v1 model set released 2026-06; `libvmaf v2.0.0` has "a new
fixed-point and x86 SIMD-optimized (AVX2, AVX-512) implementation that achieves 2x speed up compared to
the previous floating-point version"; and the caveat that matters when comparing two encoders: the README
points at "a codec evaluation-friendly NEG mode" (No Enhancement Gain), introduced because VMAF rewards
image enhancement operations, which is a trap when one of the two encoders sharpens.

**Application.** One binary gives PSNR, SSIM, MS-SSIM and VMAF over the same pair of YUV files, so the
cost of reporting all four in `--compare` is a build dependency, not new code. If we report VMAF against
the inbox encoder, use NEG mode, because an encoder comparison is exactly the case the README warns about.

### 10.3 BD-rate

**Citation.** G. Bjontegaard, "Calculation of average PSNR differences between RD-curves", ITU-T SG16/Q6
document VCEG-M33, Austin TX, 2001 (the originating document; see section 12, not opened). Read instead:
`FAU-LMS/bjontegaard`, README.md at
`https://raw.githubusercontent.com/FAU-LMS/bjontegaard/main/README.md`, BSD-3-Clause, a Python
implementation with the interpolation variants.

**Claims and quotes.** "The Bjontegaard-Delta (BD) metrics (delta bit rate and delta PSNR) described in
[1] are well known metrics to measure the average differences between two rate-distortion (RD) curves.
They are based on **cubic-spline interpolation (CSI)** of the RD curves"; "However, this way of
interpolation using a third-order polynomial leads to problems for certain RD curve constellations and
causes very misleading results. This has also been experienced during the standardization of HEVC.
Consequently, the so-called **piecewise cubic hermite interpolation (PCHIP)** has been implemented in the
JCT-VC Common Test Conditions (CTC) Excel sheet ... In further studies, it was found that **Akima
interpolation** returns more accurate and stable results."; "In our tests, the implementation of PCHIP
returns the same value as the Excel-Implementation ... with an accuracy of at least 10 decimal positions."

**Application.** Our sweep currently reports mean PSNR at one nominal rate per case, which is a single
point on a curve, and the inbox encoder "spends 4-7 % more bits" at that point. That is not a fair
comparison and it is why T3 reads as a dB gap rather than a rate gap. The fix is cheap: run each case at
four quantisers, compute BD-rate with PCHIP (to match what standards bodies report) or Akima, and state
T3 as a BD-rate against the inbox encoder as well as a dB figure. `pip install bjontegaard`; no code of
ours changes. **Do this before measuring any of the levers below**, or every result will be confounded by
the rate mismatch.

---

## 11. Two smaller sources, recorded for completeness

### 11.1 Macroblock-level QP in H.264, modern bound

**Citation.** Q. Xu, I. V. Bajic, "Differentiable Proxy Learning for Adaptive Quantization Control in
H.264 Video Coding", arXiv:2607.10478v1 [eess.IV], 11 Jul 2026, School of Engineering Science, Simon
Fraser University. PDF read; abstract and introduction read in full.

**Quote.** "The resulting proxy-based AQ framework consistently improves rate-task trade-offs over
fixed-QP H.264 baselines, achieving BD-rate reduction of up to 17.12% for semantic segmentation and
15.30% for MS-SSIM."

**Application.** The *mechanism* (a learned differentiable proxy of the codec, used to train a QP-map
network) is not applicable to a real-time MFT. The *bound* is: per macroblock QP, chosen well, is worth
something in the 15 percent BD-rate range on H.264 intra against a fixed QP. That is the upper envelope
against which the conformance cost of our per macroblock QP work should be judged, and it is consistent
with section 8's HEVC numbers.

### 11.2 Lookahead-driven rate control, modern restatement

**Citation.** "Revisiting Pre-analysis Information Based Rate Control in x265", arXiv:2109.12294v3. PDF
read; abstract and results scanned.

**Quote.** "proposed method can achieve 10.3% BD-rate gain with only 0.22% bitrate error which is
superior than the anchors both in" [BD-rate and bitrate error].

**Application.** Confirms that the macroblock-tree family is still the state of practice for
lookahead-based control and that 10 percent BD-rate is the order of the prize. Same latency blocker as
section 2.

---

## 12. Sources named but not opened

| Source | Why |
|---|---|
| P. List, A. Joch, J. Lainema, G. Bjontegaard, M. Karczewicz, "Adaptive deblocking filter", IEEE TCSVT 13(7):614-619, July 2003, DOI 10.1109/TCSVT.2003.815175 (metadata confirmed at the Crossref API) | The Semantic Scholar green open-access link (`cs.sfu.ca`) returned HTTP 500; every `iphome.hhi.de` and `hhi.fraunhofer.de` PDF URL in this environment returns the same 37208-byte placeholder with no extractable text, so the body and its measurement tables were not read. The widely repeated "bit-rate savings exceeding 9% at equal PSNR" is therefore **not** quoted here as a verified number. What I could establish at the source instead: the two tuning knobs exist in x264 as `i_deblocking_filter_alphac0` and `i_deblocking_filter_beta`, both clipped to [-6, 6], both defaulting to 0 (section 4); and both are already implemented in our `cs_deblock.hlsl` and `h264_syntax.cpp` but not exposed in `EncoderConfig`. |
| T. Wiegand, H. Schwarz, A. Joch, F. Kossentini, G. J. Sullivan, "Rate-constrained coder control and comparison of video coding standards", IEEE TCSVT 13(7):688-703, July 2003, DOI 10.1109/TCSVT.2003.815168 | Same placeholder interception of the HHI mirror. This is the canonical source for the H.264 Lagrangian `lambda = 0.85 * 2^((QP-12)/3)`. I therefore derived the operating values directly from x264's own tables instead (section 4(a)), which gives 0.9 rather than 0.85 and is in any case the implementation our comparison target family actually uses. |
| S. Momcilovic, A. Ilic, N. Roma, L. Sousa, "Dynamic Load Balancing for Real-Time Video Encoding on Heterogeneous CPU+GPU Systems", IEEE Transactions on Multimedia 16(1):108-121, Jan 2014, DOI 10.1109/TMM.2013.2284892 (metadata confirmed at Crossref) | Semantic Scholar reports `openAccessPdf: status CLOSED`; the author's institutional mirror returned an empty reply. Its reported speedups (up to 2.6x over a single GPU, up to 8.5x over optimised multicore CPU) are from a secondary description and are not quoted as verified. The BC-250 sources in section 9 cover the same ground on the actual hardware. |

Note on method: Unpaywall rejects the project's standard contact address (`research@example.com`) with
HTTP 422 "Please use your own email address in API calls". Per the workspace rule, no real address was
substituted, so Unpaywall was not used. Crossref accepts `mailto=research@example.com` and was used for
citation metadata.

---

## 13. Synthesis: ranked levers

### 13.1 For T3 as written (PSNR against the inbox encoder)

| # | Lever | Where | Evidence | Expected | Cost | Risk |
|---|---|---|---|---|---|---|
| 1 | Fix the motion cost function: make it relative to a predictor and logarithmic in the difference | `cs_me.hlsl` cost, `encoder.cpp` lambda | s.4.1, s.5 (x264's `BITS_MVD` is predictor-based; `logs[i] = 2 log2(i+1) + 1.718`) | large on panning content; today we overcharge the correct vector by up to 55x | one `MbInfo` read for the previous picture's co-located vector; no new dispatch | bytes only |
| 2 | Seed the search from predictors instead of always starting at (0,0) within +-21 samples | `cs_me.hlsl` stage 0 | s.5 ("adaptive radius ... as high as 24" around a predictor set) | large on fast motion; today motion above 21 px/frame is simply not found | negligible | bytes only |
| 3 | CABAC | new `src/h264_cabac.cpp`, PPS flag, Main profile | s.9.1, measured on this silicon with our exact mode set: 10-13 % smaller at matched QP, +28 % CPU | roughly 0.5-0.8 dB equivalent | about 1 ms per 1080p picture, hidden by the two-slot pipeline | big normative surface, fully covered by the `--encode` oracle |
| 4 | Inter partitions (16x8, 8x16, 8x8) | `cs_me.hlsl`, `cs_mb.hlsl`, `h264_cavlc.cpp` | s.5 (the SATD0 estimate for 16x8/8x16 from the 8x8 result avoids a search per shape) | the named cause of the gap; not separately quantified by any source I could open | the 32 lanes already form per-8x8 partials in `gPart`; the CAVLC side is new syntax | bytes; more groupshared pressure on an already occupancy-limited `cs_me` |
| 5 | Intra macroblocks in P pictures | `cs_mb.hlsl` | the second named cause; the file itself says why they are absent ("they would force the P picture onto the wavefront as well") | not quantified by any source here | **high**: it puts the P picture on the anti-diagonal wavefront, which is the one change that could cost T1/T2 | bytes; schedule risk |
| 6 | SATD instead of SAD, plus a bit term, in mode decision | `cs_mb.hlsl` `gRedCost` | s.5 (`SATD0 = SATD + lambda * bits`) | the third named cause (luma-only decision is #7) | a 4x4 Hadamard over data already in groupshared | bytes only |
| 7 | A chroma term in the mode decision, and a joint luma+chroma cost | `cs_mb.hlsl` | s.4.4 (`ssd_mb` adds weighted chroma SSD), s.8.1, s.8.2 | the chroma gap is the larger one (-3.8 to -6.0 dB) | small | bytes only |
| 8 | Trellis / soft-decision quantization against a CAVLC rate model | new pass, or CPU-side before CAVLC | s.5, measured: 5 % of bitrate against the uniform deadzone we use | about 5 % of rate | +31 % of x264's encode time; ours would be the per-coefficient work only | bytes; the CAVLC rate model has to be derived from our clause 9.2 tables |
| 9 | Per macroblock QP from all-three-plane variance | `cs_mb.hlsl`, `cs_deblock.hlsl` (qPav), `h264_cavlc.cpp` (`mb_qp_delta`) | s.8.1 and s.8.2 are the only AQ sources whose axis is compatible with T3; s.11.1 bounds it near 15 % BD-rate | up to about 15 % BD-rate, but see the warning | one variance reduction per macroblock plus three conformance surfaces | **conformance**: `cs_deblock.hlsl` currently assumes one QP per picture |
| - | **Not** luma-only variance AQ in the x264 style | - | s.8.1/s.8.2 beat exactly that anchor; s.3's own source says the constants are "tuned to 2" significant digits | would widen the PSNR gap | - | - |
| - | **Not** psy-RD | - | s.7, all twelve measured MOS cells negative, bitrate up at every target | negative | - | - |
| - | **Not** the quantiser deadzone | - | s.4.3: ours already matches x264's defaults within 3 % | zero | - | - |
| - | **Not** multi-slice | - | s.9.2 table (d): -3.66 dB at 2 slices, -8.39 dB at 4, on this silicon | negative | - | - |

### 13.2 For the 1.7x speed gap

| # | Lever | Where | Evidence | Expected |
|---|---|---|---|---|
| 1 | Skip the levels of zero blocks in CAVLC, using the per-block non-zero counts we already compute | `h264_cavlc.cpp` | s.9.1: 89.8-95.8 % of blocks are entirely zero; their mask change gave CAVLC -17.5 to -20.7 % | CPU-side only, no GPU change |
| 2 | Compact the levels on the GPU so only live blocks cross the bus (today 6.7 MB per 1080p picture) | `cs_mb.hlsl`, `gpu_pipeline.cpp` | s.9.1 section 20: untouched CAVLC ran 30-37 % faster from cache residency alone | about 1 ms per 1080p picture, that is a quarter of the gap |
| 3 | Check whether CAVLC scans the mapped staging pointer directly | `gpu_pipeline.cpp`, `h264_cavlc.cpp` | s.9.1 section 19.2: 13.76 ms cached against 329.92 ms uncached for the same work on this hardware | potentially large if we are on the wrong side of it |
| 4 | A third pipeline slot | `gpu_pipeline.cpp` | already named in LAB-B19-RESULT | one more picture of latency, which conflicts with `AVLowLatencyMode` |
| 5 | `cs_me` occupancy: the 9.8 KB of groupshared is the limiter | `cs_me.hlsl` | our own note; 64 KB LDS per CU on gfx1013 means 6 groups per CU today | levers 1-3 are cheaper and do not touch the GPU |

### 13.3 Before any of it

1. **Restate the comparison as a curve.** Four quantisers per case, BD-rate with PCHIP and Akima
   (section 10.3). The present single-point comparison at a 4-7 percent bitrate mismatch cannot support a
   1.0 dB / 1.5 dB acceptance criterion.
2. **Report SSIM and VMAF alongside PSNR in `--compare`.** One `libvmaf` build gives all four
   (section 10.2); use NEG mode for the encoder comparison.
3. **Expose `slice_alpha_c0_offset_div2` and `slice_beta_offset_div2` in `EncoderConfig`.** The shader and
   the slice header already carry them; only the configuration struct does not. That is the cheapest
   measurable knob in the whole encoder and it is currently unreachable.
4. **Decide whether T3 is a PSNR target or a perceptual target, in writing.** Sections 6 and 7 make the
   two mutually exclusive in the short term, and half the published literature on this topic optimises
   the axis we are not measuring.
