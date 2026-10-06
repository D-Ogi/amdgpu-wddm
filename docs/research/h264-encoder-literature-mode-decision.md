# Literature sweep: mode decision, partitions and intra on the GPU

> This sweep has an independent re-check, [`h264-encoder-literature-mode-decision-check.md`](h264-encoder-literature-mode-decision-check.md). The check opened every source again
> on its own and recorded a verdict for each. Some verdicts are VERIFIED WITH CORRECTIONS, and this
> sweep's own text was not rewritten from them. Read the check before you act on a quote here.

Target: the Media Foundation H.264 encoder MFT at
`driver/umd/mft-h264/`, which runs motion estimation, mode
decision, transform, quantisation, reconstruction and deblocking as D3D12/D3D11 compute on the BC-250
(Cyan Skillfish, gfx1013, RDNA1 class, 40 CUs, 64 KB LDS per CU).

Question behind the sweep: criterion T3 is missed on unit A (`LAB-B19-RESULT.md`, 2026-10-06) by
Y -1.3 to -2.3 dB and chroma -3.8 to -6.0 dB against the Windows inbox software H.264 encoder at the
same nominal rate. The diagnosed causes are 16x16 partitions only, no intra macroblocks in P pictures,
and a luma-only mode decision. This document collects what the primary literature and the open
encoders actually measured for each of those tools, and what each one would cost us.

Date of sweep: 2026-10-06. Research only: nothing here was built, deployed or run on the lab.

---

## 1. What our encoder does today

Read from the source, not from the README. File and line references are to the `perf-wt` worktree.

| Stage | What it does | Where |
|---|---|---|
| Motion estimation | One 32-thread group per macroblock. Three integer stages (5x5 step 8, 5x5 step 2, 3x3 step 1, reach +-16), then half-sample and quarter-sample 3x3 refinements scored out of groupshared interpolation windows. 16x16 only. | `shaders/cs_me.hlsl:185-300` |
| ME cost | `cost = SAD + gLambda * (abs(mvx) + abs(mvy))` for the integer stages, `... / 4u` for the sub-pel stages. The motion vector is costed against **(0,0)**, not against the clause 8.4.1.3 median predictor. | `shaders/cs_me.hlsl:236, 288` |
| Lambda | `gp.lambda = (1 + qp/8) * lambdaScale / 100`, `lambdaScale = 300`. Linear in QP. | `src/encoder.cpp:274`, `src/encoder.h:95` |
| Skip | A vector snaps back to (0,0) when the zero vector costs at most `gSkipBias` extra SAD; `skipBiasScale = 0` today, so the bias is off. | `shaders/cs_me.hlsl` header, `src/encoder.h:96` |
| I picture mode decision | Intra_16x16, four luma modes, chosen by **SAD** with no rate term; four chroma modes chosen separately by **SAD** over both planes, also with no rate term. Threads 0..3 score luma, 4..7 chroma. | `shaders/cs_mb.hlsl:265-300` |
| I picture dispatch | One anti-diagonal (`mbx + mby == gDiagonal`) per dispatch, because Intra_16x16 reads the unfiltered reconstruction of the top and left neighbours. | `shaders/cs_mb.hlsl:8-11` |
| P picture mode decision | None. Every macroblock is `P_L0_16x16` with the vector cs_me chose. "Intra macroblocks inside a P picture are deliberately not produced: they would force the P picture onto the wavefront as well." | `shaders/cs_mb.hlsl:12-16, 302-306` |
| Chroma QP | Table 8-15 is implemented, `chroma_qp_index_offset` defaults to 0. | `src/encoder.cpp:38-50`, `src/encoder.h:72` |
| Quantiser rounding | `f = 2^qbits / 3` for intra, `2^qbits / 6` for inter, applied identically to luma and chroma. | `shaders/h264_common.hlsli:250-294` |
| Entropy coding | CAVLC on the CPU, baseline-profile style. | `src/h264_cavlc.cpp` |

Two facts from this table are not in the diagnosis list and matter for what follows.

- **The motion vector rate term is wrong in shape and in reference.** It is linear in `abs(mv)` where
  every production encoder uses a logarithmic estimate of the Exp-Golomb length, and it is measured
  from (0,0) instead of from the predicted vector, so a panning scene pays the global motion again in
  every macroblock even though its `mvd` is near zero.
- **Lambda is linear in QP** where the standard relation is exponential. At QP 20 our lambda is 9
  against the usual 3; at QP 40 it is 18 against the usual 25. A scale sweep cannot fix a shape error,
  which is consistent with the README's own observation that 3x, 4x and 6x the lambda all landed within
  0.1 dB of each other.

Already measured and ruled out by us: `--chroma-qp-offset` was swept and -6 reached parity on Cb while
costing 2.1 dB of luma (`driver/umd/mft-h264/README.md:207-209`). Chroma QP offset is not the lever.

---

## 2. Sources

### S1. Overview of the H.264/AVC Video Coding Standard (normative tool set)

**Citation.** T. Wiegand, G. J. Sullivan, G. Bjontegaard, A. Luthra, "Overview of the H.264/AVC Video
Coding Standard", *IEEE Transactions on Circuits and Systems for Video Technology*, vol. 13, no. 7,
pp. 560-576, July 2003. DOI 10.1109/TCSVT.2003.815165. Read at
`https://www.cs.ubc.ca/~krasic/cpsc538a/papers/h264avc-overview.pdf` (author-era PDF of the published
article; page headers and pagination match the journal). Quotes below normalise the PDF text layer,
which renders "16x16" as "16 16".

**What it fixes for us.** The exact set of modes we may add and the one spec switch that changes the
GPU dependency graph.

Partitions, section H.1, p. 569:

> "Partitions with luma block sizes of 16x16, 16x8, 8x16, and 8x8 samples are supported by the syntax.
> In case partitions with 8x8 samples are chosen, one additional syntax element for each 8x8 partition
> is transmitted. This syntax element specifies whether the corresponding 8x8 partition is further
> partitioned into partitions of 8x4, 4x8, or 4x4 luma samples and corresponding chroma samples."

Chroma follows the luma vector; there is no separate chroma vector (section H.1, p. 570):

> "The prediction values for the chroma component are always obtained by bilinear interpolation. Since
> the sampling grid of chroma has lower resolution than the sampling grid of the luma, the displacements
> used for chroma have one-eighth sample position accuracy."

This is the mechanism behind our chroma gap being roughly twice the luma gap. One vector per 16x16
macroblock is also one vector per 8x8 chroma block. Where a single vector is wrong for part of a
macroblock, chroma has no detail to mask the error and no separate vector to correct it.

Intra types, section G, p. 568:

> "The Intra_4x4 mode is based on predicting each 4x4 luma block separately and is well suited for
> coding of parts of a picture with significant detail. The Intra_16x16 mode, on the other hand,
> performs prediction of the whole 16x16 luma block and is more suited for coding very smooth areas of
> a picture."

And the switch that matters most for a GPU encoder, same section:

> "This may incur error propagation in environments with transmission errors that propagate due to
> motion compensation into inter-coded macroblocks. Therefore, a constrained intra coding mode can be
> signaled that allows prediction only from intra-coded neighboring macroblocks."

**Application.** `constrained_intra_pred_flag = 1` in the PPS changes nothing inside an I picture,
where every neighbour is intra anyway. In a P picture it marks inter neighbours unavailable for intra
prediction. If intra macroblocks in P pictures are sparse (they are: they appear at occlusions and
scene cuts), each one then has no reconstruction dependency on its inter neighbours at all, and the
P picture stays one dispatch except for the small clusters where two intra macroblocks touch. This
turns the exact objection written in `cs_mb.hlsl:14-16` into a bounded one. Cost: intra prediction in
P falls back to DC 128 where neighbours are inter, which is weaker prediction, so it only pays where
inter prediction is already very bad, which is exactly where we want intra. Risk to bit exactness:
the flag is normative syntax, decoders implement it, but it is new surface and needs its own
conformance case.

### S2. Complexity/Performance Analysis of a H.264/AVC Video Encoder (tool-by-tool ablation)

**Citation.** H. Krichene Zrida, A. C. Ammari, M. Abid, A. Jemai, "Complexity/Performance Analysis of
a H.264/AVC Video Encoder", in *Recent Advances on Video Coding*, J. Del Ser Lorente (ed.), InTech,
2011, ch. 2, pp. 25-50. DOI 10.5772/16821. Open access; read the publisher PDF
(`https://www.intechopen.com/chapter/pdf-download/16265`). Tables were transcribed from the rendered
pages 36-38, not from a text extraction, because the extraction interleaves the columns.

**Why it is the right source.** It ablates single tools in the JM reference encoder and reports bit
rate, PSNR-Y and encoding time for each. That is the only shape of measurement that answers
"what is 16x16-only costing us".

**Variable block sizes, section 4.3.3 and Table 5** (JM, CIF and QCIF, RD optimisation enabled,
search range 8). Rows: `7` = all of 16x16, 16x8, 8x16, 8x8, 8x4, 4x8, 4x4; `4` = 16x16, 16x8, 8x16,
8x8; `1` = 16x16 only. The `4` and `1` rows give deltas against the `7` row.

| Sequence | Bit rate at 7 partitions | delta at 4 | delta at 1 | delta PSNR-Y at 4 | delta PSNR-Y at 1 |
|---|---|---|---|---|---|
| Mobile (CIF) | 678.46 | +14.53 (+2.1 %) | +58.35 (+8.6 %) | -0.11 dB | -0.14 dB |
| Paris (CIF) | 131.23 | +5.19 (+4.0 %) | +15.36 (+11.7 %) | -0.13 dB | -0.17 dB |
| Bridge-close (CIF) | 106.67 | +0.67 (+0.6 %) | +2.30 (+2.2 %) | -0.06 dB | -0.09 dB |
| Foreman (QCIF) | 81.44 | +1.32 (+1.6 %) | +9.15 (+11.2 %) | -0.09 dB | -0.20 dB |
| Container (QCIF) | 19.42 | +0.45 (+2.3 %) | +2.96 (+15.2 %) | -0.09 dB | -0.18 dB |
| Mother & Daughter (QCIF) | 30.36 | +0.18 (+0.6 %) | +2.82 (+9.3 %) | -0.14 dB | -0.22 dB |

Complexity, same table: `delta TEC` (total encoder complexity) is -9.1 % to -11.9 % at 4 partitions and
-11.2 % to -14.2 % at 1, both against 7.

The chapter's own reading, p. 35:

> "Compared, with the 4 block size (16x16, 16x8, 8x16, and 8x8), we got a light video quality
> degradation with a negligible loss in bit rate (negligible loss for the QCIF version of 'bridge far'
> and less than 2.5% for the CIF version of 'mobile'), but with a 10% average complexity reduction.
> With only one 16x16 block size mode, we got more significant video quality degradation compared to
> that with four block sizes, but with an encoding time further reduced 10% average. These results
> confirm that block sizes smaller than 8x8 (i.e. the seven block size mode on) do not provide
> significant benefits compared with the 4 block size mode. However, with the use of only 16x16 block
> size, the encoding performance is significantly decreased."

**Application.** On moving content, 16x16-only costs about 9 to 15 % bit rate and 0.1 to 0.2 dB against
a full-partition encoder, and roughly two thirds to three quarters of that is recovered by adding only
16x8, 8x16 and 8x8. Sub-8x8 buys the remaining 1 to 4 %. For us that ranks the work: **16x8 / 8x16 /
8x8 first, sub-8x8 last or never.** Note this is CIF and QCIF; see S3 for the HD statistics on
sub-8x8, which point the same way harder.

**RD-Lagrangian mode decision, section 4.3.5 and Table 7.** Disabling the RD Lagrangian decision in JM:

| Sequence | delta bit rate | delta PSNR-Y | delta TEC |
|---|---|---|---|
| Bridge-close (CIF) | +23.34 on 114.04 (+20.5 %) | +0.24 dB | -61.65 % |
| Mobile (CIF) | +152.46 on 719.97 (+21.2 %) | +0.32 dB | -68.63 % |
| Paris (CIF) | +13.93 on 142.89 (+9.8 %) | +0.06 dB | -66.85 % |
| Bridge-far (QCIF) | +1.09 on 2.73 (+39.9 %) | -0.08 dB | -53.81 % |
| Foreman (QCIF) | +14.30 on 85.35 (+16.8 %) | +0.04 dB | -59.61 % |

> "The encoder without RD optimization is about 2~3 times faster and gives a noticeable loss in bit
> rate-distortion compared to the case with an RD-Lagrangian technique enabled (an average of 40% in
> bit rate increase in case of QCIF 'bridge far' sequence, as described in table 7)."

**Application.** A real rate-distortion decision (true bits, true SSD) is worth roughly 10 to 21 % bit
rate on moving CIF content, at 2 to 3x the encode time. We have no rate term at all in the mode
decision, so this is the headroom above a cost-approximated decision, not a target for our next step.

**Hadamard, section 4.3.6 and Table 8.** With RD optimisation on, disabling the Hadamard (SATD)
distortion changed the bit rate by at most +1.49 on 719.97 (+0.2 %, Mobile CIF) and PSNR-Y by -0.01 to
-0.07 dB, for -1.41 % to -3.92 % complexity.

> "activating the Hadamard transform causes a slight complexity increase without any coding efficiency
> gain. Thus, the Hadamard transform will be disabled for the optimized parameter configuration."

**Application, with a caveat.** This result is specific to an encoder that already runs full RD. When
the real RD cost is computed, the SATD approximation it replaces is redundant. It does **not**
transfer to an encoder whose decision *is* the approximation. S3 measures exactly that case and finds
the opposite sign.

### S3. Optimizing an H.264 video encoder for real-time HD-video encoding (SATD and RDO on HD, BD-rate)

**Citation.** P. Hermansson, "Optimizing an H.264 video encoder for real-time HD-video encoding",
Master of Science thesis, KTH Information and Communication Technology, Stockholm, 2011,
TRITA-ICT-EX-2011:74. Full text at
`https://www.diva-portal.org/smash/get/diva2:432684/FULLTEXT01.pdf`. Table 4.11 transcribed from the
rendered page 46.

**Why it is the right source.** It is a *low-complexity* real-time HD encoder, i.e. our regime, and it
reports BD-rate and BD-PSNR, which the InTech chapter does not. The reference algorithm "Ref 1"
compares INTER16x16 (and SKIP) with INTRA16x16 DC only, using the low-complexity rate-distortion
method. That is almost exactly our decision today.

Table 4.11, "Reference mode selection with higher complexity", p. 46, one reference frame, CAVLC,
assembly off:

| Sequence | Method | BDBR (%) | BDPSNR (dB) | SADs/Modes | delta Time |
|---|---|---|---|---|---|
| vidyo1 | Ref 1 + SATD | 0.7 | -0.02 | 2.2 | +25 % |
| vidyo1 | Ref 1 + High RDO | -3.2 | 0.11 | 4.0 | +40 % |
| vidyo1 | Ref 1 (Combined) | -1.7 | 0.06 | 4.0 | +72 % |
| blue_sky | Ref 1 + SATD | 0.7 | -0.03 | 2.3 | +26 % |
| blue_sky | Ref 1 + High RDO | 12.5 | -0.61 | 4.0 | +40 % |
| blue_sky | Ref 1 (Combined) | -0.3 | 0.02 | 4.0 | +70 % |
| pedestrian | Ref 1 + SATD | -2.5 | 0.13 | 2.3 | +26 % |
| pedestrian | Ref 1 + High RDO | -11.6 | 0.58 | 3.9 | +37 % |
| pedestrian | Ref 1 (Combined) | -13.6 | 0.7 | 3.9 | +71 % |
| riverbed | Ref 1 + SATD | -7.2 | 0.39 | 2.0 | +26 % |
| riverbed | Ref 1 + High RDO | -19.1 | 1.18 | 3.4 | +47 % |
| riverbed | Ref 1 (Combined) | -19.6 | 1.25 | 3.4 | +79 % |

> "Furthermore using SATD instead of SAD gives a slight improvement in both compression and quality,
> especially for higher motion sequences (pedestrian and riverbed). The best results are however
> achieved by combining SATD with high complexity rate-distortion."

> "The conclusion is therefore that in order to get additional compression and quality, high complexity
> rate-distortion should be considered before evaluating additional modes."

Sub-8x8 usage on HD, section 3.6.2, p. 31-32:

> "In this experiment it was shown that none of the macroblocks in the vidyo1 sequence were coded using
> any of the INTER8x4, 4x8 and 4x4 modes. The result of encoding the other sequences was that between
> 0.2% to 0.5% of the macroblocks were encoded with any of these modes. Since the computational cost of
> the INTER8x4, 4x8 and 4x4 modes are much higher they are considered less computationally effective
> and thus not used in the algorithms below."

Early termination designs it implemented, section 3.6.2:

- Algorithm I (after Huang et al.): early skip from an adaptive threshold over previously skipped
  macroblocks, plus neighbour-driven mode groups. "if all neighbors to the left and above have been
  coded as either SKIP or INTER16x16 then only the following block sizes are evaluated 16x16, 16x8,
  8x16, 8x8. The proposed algorithm allows even 8x8 to be skipped if the result of the first three
  searches resulted in 16x16 being the best match."
- Algorithm I intra gate: "the decision of checking either INTRA4x4 or INTRA16x16 is determined by
  checking if any of the neighbors have being coded with INTRA or INTER8x8. In that case INTRA4x4 is
  evaluated; otherwise INTRA16x16."
- Algorithm II (after Zhao et al.): order the modes by their frequency among the neighbours and stop
  at the first cost increase. "because the list has a descending order of probability to be the best,
  mode decision could be early terminated when a larger RD cost is obtained".

The author's own SKIP caveat is directly relevant to our `gSkipBias`:

> "A downside of using the low complexity rate-distortion evaluation is that SKIP prediction becomes
> problematic. When the SAD of the predicted SKIP block is compared with the SAD of the best predicted
> inter block the bit-cost of the motion-vectors makes SKIP prediction slightly biased. As a
> consequence of this I concluded that simply comparing the SKIP SAD is not enough for determining if a
> block should be coded with the SKIP mode. Instead the SKIP mode is only evaluated if INTER16x16 has
> been determined to be the best mode for that macroblock. Furthermore SKIP is also only used if coding
> the macroblock with INTER16x16 would result in no coefficients being sent in the compressed
> bit-stream."

**Application.** Three things.
1. In our regime SATD instead of SAD is worth -2.5 % to -7.2 % BD-rate and +0.13 to +0.39 dB BD-PSNR on
   moving HD content, for about +26 % decision time. On static or very smooth content it is neutral to
   slightly negative (+0.7 %). A 4x4 Hadamard costs 8 adds per row plus 8 per column per 4x4 block,
   which on the GPU is cheap next to the six-tap interpolation we already do: this is the single
   best effort-to-gain item in the whole sweep.
2. The neighbour-driven early termination of Algorithms I and II is a raster dependency. On a GPU we
   cannot use the *chosen* mode of the left neighbour inside the same dispatch. The GPU-shaped
   substitutes are in S4 and S6.
3. Our `skipBiasScale = 0` plus a SAD-only comparison reproduces exactly the bias Hermansson describes.
   The fix he used (only consider P_Skip when 16x16 won and the residual is empty) is what
   `squeeze264` also does (S7) and is free for us, because our CPU side already knows the cbp.

### S4. Efficient Parallel Video Processing Techniques on GPU (the intra dependency, on a GPU)

**Citation.** H. Su, M. Wen, N. Wu, J. Ren, C. Zhang, "Efficient Parallel Video Processing Techniques
on GPU: From Framework to Implementation", *The Scientific World Journal*, vol. 2014, article 716020,
2014. DOI 10.1155/2014/716020. Open access; read the full text at
`https://pmc.ncbi.nlm.nih.gov/articles/PMC3976889/`.

**Why it is the right source.** It is a full H.264 encoder on CUDA, not a motion-estimation-only
paper, and it states both the dependency trick and the price it paid for it.

Dropping the prediction modes that need the upper-right neighbour, section 5.1.3:

> "Experiments to multiple test sequences show that some prediction methods, needing upper right
> reconstructed pixels (the third and the seventh method of the 4 x 4 prediction and the third of the
> 16 x 16 prediction), play a slight role. It increases the bit-rate for I-frames by less than 1%."

Block ordering inside a macroblock, same section:

> "From the graph, we know that the maximal number of blocks within a MB that can be performed
> simultaneously is only 2 ... the intracoding of a MB can be completed in 7 steps and the parallel
> degree can reach 4 for a MB."

Macroblock-level parallelism, section 5.1.2:

> "In order to increase the parallel degree, multislice method is introduced. It partitioned each frame
> into multislice and processed each slice independently. At the same time, the wave-front method is
> adopted for parallelizing the MBs in the same slice."

Partition SADs, section 4:

> "A MB is divided into variable block sizes, such as 8 x 4, 4 x 8, 8 x 8, 16 x 8, 8 x 16, and 16 x 16"
> ... "Using the generated SAD values of 4 x 4 subblocks, the SAD value for other sizes of block can be
> calculated."

Measured, section 6.2 and 6.3: PSNR loss 0.35 to 0.54 dB (D1), 0.14 to 0.77 dB (720p), 0.33 to 0.57 dB
(1080p) at the same bit rate against the serial reference; overall speedup about 20x on a Tesla C2050,
16x on a GTX 460, 11x on a GTX 260; motion estimation 13x, 18x, 25x on those three; intra prediction
2.8x to 8.8x; 30 fps at 720p and 20 fps at 1080p.

**Application.**
- The partition SAD tree is the key cost fact for us: **16x8, 8x16 and 8x8 do not need six new
  searches.** Score the sixteen 4x4 SADs (or SATDs) once per candidate and sum them into every
  partition shape. `cs_me.hlsl` already computes a per-lane partial sum over eight samples
  (`gPart[9*32]`, `cs_me.hlsl:48`); the change is to keep the partials per 4x4 block instead of
  collapsing them, which costs groupshared, not arithmetic.
- The "drop the modes that need the upper-right neighbour" trick costs under 1 % on I frames and would
  let a future Intra_4x4 pass depend on top and left only. We do not use Intra_4x4 yet, and for
  Intra_16x16 the only upper-right user is already absent, so this is a note for later, not now.
- Their honest PSNR loss of 0.14 to 0.77 dB is the price of a GPU encoder that approximates. It is the
  same order as our luma gap, which says our luma deficit is roughly "normal for this class of design"
  and the chroma deficit is not.

### S5. x264 (production encoder, source read directly)

**Citation.** x264, GPL-2.0-or-later, VideoLAN. Read from the GitHub mirror `mirror/x264` at `master`:
`common/tables.c`, `encoder/analyse.c`, `common/base.c`, `common/set.c`, `common/set.h`. Not imported;
read for its design and its constants.

**Lambda.** `common/tables.c:96-127`:

> `/* lambda = pow(2,qp/6-2) */`
> `const uint16_t x264_lambda_tab[QP_MAX_MAX+1] = { ... }`
> `/* lambda2 = pow(lambda,2) * .9 * 256 */`
> `const int x264_lambda2_tab[QP_MAX_MAX+1] = { ... }`

and `common/tables.c:131-152`, the trellis table, which states the classical relation explicitly:

> `// inter lambda = .85 * .85 * 2**(qp/3. + 10 - LAMBDA_BITS)`
> `// intra lambda = .65 * .65 * 2**(qp/3. + 10 - LAMBDA_BITS)`

So the SSD-domain lambda is 0.85^2 * 2^(QP/3) up to scaling, its square root is the SAD/SATD-domain
lambda, and the comment on `x264_lambda_tab` records that square root as 2^(QP/6 - 2). x264 also uses a
**lower** lambda for intra than for inter (0.65 against 0.85), i.e. intra decisions are biased towards
quality rather than rate.

**Motion vector rate.** `encoder/analyse.c:179-200`:

> `logs[0] = 0.718f;`
> `for( int i = 1; i <= 2*4*mv_range; i++ )`
> `    logs[i] = log2f( i+1 ) * 2.0f + 1.718f;`

and `encoder/analyse.c:143-157`: `h->cost_mv[qp][i] = MIN( (int)(lambda * logs[i] + .5f), UINT16_MAX )`,
indexed by the **motion vector difference** in quarter-pel units, not by the vector itself. The cost of
a 4x4-mode flag is `cost_i4x4_mode[i] = 3*lambda*(i!=8)`, i.e. the predicted intra mode is 3 lambda
cheaper than any other.

**Intra mode decision.** `encoder/analyse.c:668-980`. Luma Intra_16x16: the three cheap modes are
scored together, each plus `lambda * bs_size_ue(mode)`; the Plane mode is only tried if one of the
three was good enough:

> `/* Plane is expensive, so don't check it unless one of the previous modes was useful. */`
> `if( a->i_satd_i16x16 <= i16x16_thresh )`

with `static const uint8_t i16x16_thresh_lut[11] = { 2, 2, 2, 3, 3, 4, 4, 4, 4, 4, 4 };` and
`i16x16_thresh = a->b_fast_intra ? (i16x16_thresh_lut[h->mb.i_subpel_refine]*i_satd_inter)>>1 : COST_MAX;`.
Early exit after the 16x16 stage: `if( a->i_satd_i16x16 > i16x16_thresh ) return;`. The 4x4 stage
starts from `int i_cost = lambda * (24+16); /* 24from JVT (SATD0), 16 from base predmode costs */` and
runs under `i_satd_thresh = X264_MIN3( i_satd_inter, a->i_satd_i16x16, a->i_satd_i8x8 )`.

**Chroma in the decision.** `encoder/analyse.c:586-665`: chroma intra modes are scored by the same
`mbcmp` (SATD by default) over Cb and Cr plus `a->i_lambda * bs_size_ue(i_mode)`. In a P slice,
`encoder/analyse.c:3181-3194`:

> `if( h->mb.b_chroma_me )`
> `{ ... mb_analyse_intra_chroma( h, &analysis ); mb_analyse_intra( h, &analysis, i_cost - analysis.i_satd_chroma ); }`
> `analysis.i_satd_i16x16 += analysis.i_satd_chroma;`

`b_chroma_me` defaults to 1 (`common/base.c:446`). So chroma distortion enters the **intra-versus-inter
comparison** in x264, while the inter search itself stays luma-only except for the sub-8x8 chroma
cost in `mb_analyse_inter_p4x4_chroma`.

**Partition order and early termination.** `encoder/analyse.c:3046-3112`: P16x16, then P8x8 (only when
`!b_early_terminate || i_cost8x8 < me16x16.cost`), then sub-8x8 per 8x8 under a per-8x8 threshold, then
16x8 and 8x16 gated by `i_cost8x8 < me16x16.cost + i_thresh16x8` where
`i_thresh16x8 = me8x8[1].cost_mv + me8x8[2].cost_mv`. The 16x8 search itself aborts halfway:

> `/* Early termination based on the current SATD score of partition[0] plus the estimated SATD score of partition[1] */`
> `if( a->b_early_terminate && (!i && l0m->cost + a->i_cost_est16x8[1] > i_best_satd * (4 + !!a->i_mbrd) / 4) )`

The 8x8 search caches `a->i_satd8x8[0][i] = m->cost - m->cost_mv`, and the 16x8 and 8x16 estimates are
built from those 8x8 SATDs. That is the SAD/SATD tree of S4 in production form.

**Intra in P.** Unconditional in x264: `mb_analyse_intra` is called in the P path with the inter cost
as the threshold, and the macroblock type is chosen from `X264_MIN3(i_satd_i16x16, i_satd_i8x8,
i_satd_i4x4)` against `i_satd_inter`.

**Quantiser dead zone.** `common/set.c:81-83` with `common/set.h:30-36`
(`CQM_4IY, CQM_4PY, CQM_4IC, CQM_4PC`):

> `int deadzone[4] = { 32 - h->param.analyse.i_luma_deadzone[1],`
> `                    32 - h->param.analyse.i_luma_deadzone[0],`
> `                    32 - 11, 32 - 21 };`

with defaults `i_luma_deadzone[0] = 21` (inter), `[1] = 11` (intra) in `common/base.c:456-457`. That
makes the rounding offset 21/32 of the step for intra (both luma and chroma) and 11/32 for inter (both
luma and chroma), capped at 1/2 by
`h->quant4_bias[i_list][q][i] = X264_MIN( DIV(deadzone[i_list]<<10, j), (1<<15)/j );` (`common/set.c:179`).

**Application.** Four concrete deltas against our encoder.
1. Our lambda shape is wrong; theirs is 2^(QP/6-2), confirmed twice more in S6 and S7.
2. Our MV rate term is linear and measured from zero; theirs is `lambda * (2*log2(mvd+1) + 1.718)`
   measured from the predictor.
3. Our mode decision has no rate term at all; every mode in x264 carries `lambda * bs_size_ue(mode)`.
4. Our quantiser rounds with 1/3 (intra) and 1/6 (inter) of the step, i.e. 10.7/32 and 5.3/32, against
   x264's 21/32 and 11/32. We zero roughly twice as aggressively as x264 at the same QP, on luma and
   chroma alike. This is a one-constant sweep and is listed as a hypothesis in section 3, not a claim:
   a larger dead zone also saves bits, so only a rate-matched sweep settles it.

### S6. OpenH264 (production encoder, source read directly)

**Citation.** Cisco OpenH264, BSD-2-Clause. Read from `cisco/openh264` at `master`:
`codec/encoder/core/src/md.cpp`, `svc_mode_decision.cpp`, `svc_base_layer_md.cpp`,
`encoder_data_tables.cpp`, `codec/encoder/core/inc/md.h`, `codec/encoder/core/src/sample.cpp`,
`codec/encoder/core/src/encoder_ext.cpp`.

**Why it is the right source.** OpenH264 is the low-complexity, no-RDO, real-time end of the design
space, which is where we are, and its decision structure maps onto a GPU far better than x264's.

**Lambda.** `encoder_data_tables.cpp:59-67`:

> `const int32_t g_kiQpCostTable[52] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 6, 6, 7, 8, 9, 10, 11, 13, 14, 16, 18, 20, 23, 25, 29, 32, 36, 40, 45, 51, 57, 64, 72, 81, 91 };`

This is element-for-element `x264_lambda_tab` over QP 0..51, i.e. round(2^(QP/6-2)). Two independent
production encoders agree on the constant.

**Cost metric.** `encoder_ext.cpp:2627-2633`: `pfMdCost` is `pfSampleSad` in `LOW_COMPLEXITY` mode and
`pfSampleSatd` otherwise; `svc_encode_slice.cpp:699` sets
`sMd.bMdUsingSad = (pEncCtx->pSvcParam->iComplexityMode == LOW_COMPLEXITY);`.

**Intra decision.** `svc_base_layer_md.cpp:365-416` (`WelsMdI16x16`): per available mode,
`iCurCost = pfMdCost[BLOCK_16x16](pred, 16, enc, stride) + iLambda * BsSizeUE(g_kiMapModeI16x16[iCurMode]);`
with a three-mode combined fast path and the Plane mode scored separately at `+ iLambda * 4`. Chroma is
the mirror image in `WelsMdIntraChroma` (`svc_base_layer_md.cpp:867-930`): SATD over Cb plus SATD over
Cr plus `iLambda * BsSizeUE(g_kiMapModeIntraChroma[iCurMode])`.

**Intra in P.** `svc_base_layer_md.cpp:1829-1856` (`WelsMdFirstIntraMode`):

> `int32_t iCostI16x16 = WelsMdI16x16 (pFunc, pEncCtx->pCurDqLayer, pMbCache, pWelsMd->iLambda);`
> `//compare cost_p16x16 with cost_i16x16`
> `if (iCostI16x16 < pWelsMd->iCostLuma) { pCurMb->uiMbType = MB_TYPE_INTRA16x16; ... }`

That is: P16x16 first, then I16x16, and only if intra wins does the I4x4 refinement and the chroma mode
search run. The whole intra test in a P macroblock is **one 16x16 SATD per available mode**.

**Partition decision without a search.** `md.cpp:389-432` (`MdInterAnalysisVaaInfo_c`) computes the
variance of the four 8x8 SADs of the macroblock and returns a 4-bit sign mask; `md.h:70-76` names the
patterns (`MBVAASIGN_FLAT 15`, `MBVAASIGN_HOR1 3`, `MBVAASIGN_HOR2 12`, `MBVAASIGN_VER1 5`,
`MBVAASIGN_VER2 10`, `MBVAASIGN_CMPX1 6`, `MBVAASIGN_CMPX2 9`). In
`svc_mode_decision.cpp:618-672` (`WelsMdInterFinePartitionVaaOnScreen`):

> `uint8_t uiMbSign = pEncCtx->pFuncList->pfGetMbSignFromInterVaa (&pEncCtx->pVaa->sVaaCalcInfo.pSad8x8[pCurMb->iMbXY][0]);`
> `if (MBVAASIGN_FLAT == uiMbSign) { return; }`
> `iCostP8x8 = WelsMdP8x8 (...); if (iCostP8x8 < iBestCost) { ... TryModeMerge (pMbCache, pWelsMd, pCurMb); }`

and sub-8x8 is disabled outright (`#if 0 //Disable for sub8x8 modes for now`).

**16x8 and 8x16 for free.** `svc_mode_decision.cpp:572-616` (`TryModeMerge`): if the four 8x8 vectors
agree in pairs, the macroblock is re-labelled 16x8 or 8x16 and the costs are summed, with no extra
search:

> `pTarMe->uiSadCost = sSrcMe0.uiSadCost + sSrcMe1.uiSadCost;//not precise cost since MVD cost is not the same`

The 16x16 merge is deliberately not done, with the reason recorded in the source:

> `//from test results of multiple sequences show that using the following 0x0F to merge 16x16`
> `//for some seq there is BR saving some loss`
> `//on the whole the BR will increase little bit`
> `//to save complexity we decided not to merge 16x16 at present (10/12/2012)`

**Chroma as a veto.** `svc_mode_decision.cpp:173-214` (`CheckChromaCost`) refuses a P_Skip when the
chroma SAD is large, with the thresholds and the reasoning in the comments:

> `#define KNOWN_CHROMA_TOO_LARGE 640`
> `#define SMALLEST_INVISIBLE 128 //2*64, 2 in pixel maybe the smallest not visible for luma`
> ... `//so the allowing chroma difference should be at least no larger than 20*8*8 = 1280 for U or V`
> ... `const bool bChromaTooLarge = (iCbSad > KNOWN_CHROMA_TOO_LARGE || iCrSad > KNOWN_CHROMA_TOO_LARGE);`

**Application.** This is the cheapest credible upgrade path for us, and all of it is data-parallel:
- A 2x2 grid of 8x8 SADs per macroblock, which `cs_me` can produce for free from the partials it
  already computes, decides whether to search 8x8 at all.
- If 8x8 is searched, 16x8 and 8x16 come from merging equal vectors: no extra search, no extra LDS.
- Intra in P costs one 16x16 SATD per available mode in the macroblock pass, not a second dispatch,
  provided the prediction samples are available (S1's `constrained_intra_pred_flag`).
- A chroma SAD veto on P_Skip is a direct, measured answer to a chroma deficit, and OpenH264 wrote down
  why: a luma-only decision can skip a macroblock whose colour is visibly wrong.

### S7. squeeze264 (from-scratch Constrained Baseline encoder, measured against x264)

**Citation.** `useless-husband/squeeze264`, "H.264 Constrained Baseline video encoder written from
scratch in Rust; every frame verified bit-exact against ffmpeg and VideoToolbox, with rate-distortion
curves against x264". MIT. Read `README.md`, `src/cost.rs`, `src/analysis.rs` at `main`.

**Why it is the right source.** It is the closest published analogue of our encoder: Constrained
Baseline, CAVLC, one reference frame, no RDO, no trellis, no 8x8 transform, verified bit exact against
two independent decoders. It therefore tells us what score our *class* of encoder can reach.

Measured, README "Performance vs. x264", BD-rate (positive is worse):

| Clip | Matched config | x264 baseline medium | x264 defaults |
|---|---|---|---|
| foreman CIF | +0.5 % | +13.1 % | +74.0 % |
| akiyo CIF | -4.2 % | -0.5 % | +69.2 % |
| mobile CIF | -1.4 % | +20.8 % | +115.4 % |
| shields 720p | +1.6 % | +14.2 % | +111.9 % |

Its stated limits: "The encoder lacks: B frames, CABAC, 8x8 transforms, multiple reference frames,
rate-distortion optimization, trellis quantization, SIMD acceleration, and multi-threading."

Lambda, `src/cost.rs`:

> `/// Lagrange multiplier for SATD-domain decisions, scaled by 16:`
> `/// lambda = 2^((QP - 12) / 6), the usual square root of the RD lambda.`
> `pub fn lambda16(qp: u8) -> u32 { (16.0 * 2f64.powf((qp as f64 - 12.0) / 6.0)).round().max(1.0) as u32 }`

Third independent agreement on the lambda shape, this time with the derivation spelled out.

Decision structure, `src/analysis.rs`:

- `fn bit_cost(lambda16: u32, bits: u32) -> u32 { (lambda16 * bits + 8) >> 4 }` - every mode carries a
  bit estimate.
- P path order: early skip, then 16x16, then (gated) 8x8 with optional sub-8x8, then 16x8 and 8x16
  seeded from the 8x8 vectors, then intra, then a final P_Skip check.
- The partition gate: `if self.cfg.partitions && c16 > 256 + Self::bit_cost(lam, 16)` with the comment
  `// Smaller partitions are only tried when 16x16 leaves a real residual.`
- The sub-8x8 gate: `if self.sub8x8_ok && self.cfg.sub8x8 && cost > 64 + Self::bit_cost(lam, 12)`.
- 16x8 and 8x16 are seeded, not re-searched from scratch: `[mv8[i * 2], mv8[i * 2 + 1], mv16]`.
- Intra in P, with the inter cost as the limit:
  `if self.cfg.intra_in_p { if let Some((mode, res, _)) = self.analyse_intra(qp, lam, BITS_INTRA_IN_P, best.cost) { return self.finish_intra(...); } }`
  under the comment `// Intra in a P slice, for occlusions and scene changes.`
- Intra early termination: `// Intra4x4 is only worth trying when 16x16 is within reach of the limit.`
  `if i16_cost > limit.saturating_add(limit / 4) { return None; }`
- Chroma mode by SATD over both planes with the `ue(v)` cost:
  `// intra_chroma_pred_mode is ue(v): 1, 3, 3, 5 bits.`

**Application.** A Constrained Baseline encoder with SATD mode decision, a lambda-weighted bit estimate,
all four partition shapes and intra in P reaches within about +/- 2 to 4 % BD-rate of a matched x264 and
within about +13 to +21 % of x264 `medium`, with no RDO and no trellis. That is the target for us and it
is reachable without a rate-distortion loop. Our present gap, 1.3 to 2.3 dB of luma, is far outside that
band, which says the missing items are structural, not a tuning deficit.

### S8. bc250-encoding-decoding-fix (same silicon, same approach, and a measured chroma trap)

**Citation.** `simpmix/bc250-encoding-decoding-fix`, GPL. Local read-only copy at
`<BC250_ROOT>\ref\bc250-encoding-decoding-fix__WARN-GPL-read-only-no-code-import`, HEAD
`774783d4be9c07a0a85531ec2feaf7df0e335d48` (2026-09-27). Read `docs/DEVLOG.md` and
`approach1-compute-encoder/shaders/intra_wavefront.comp`. **No code imported, facts only**, per the
directory's own warning.

**Why it is the right source.** It is a Vulkan compute H.264 encoder on the same BC-250 part, with the
same anti-diagonal intra structure we chose, and it has measured, same-hardware chroma numbers.

The wavefront, `intra_wavefront.comp` header:

> "The fix is diagonal-wavefront processing (a standard technique - this is literally what HEVC/VVC's own
> Wavefront Parallel Processing does, and GPU implementations of exactly this scheme for H.264 have been
> demonstrated in published research): every macroblock on anti-diagonal d = mbx+mby depends ONLY on
> macroblocks on diagonal d-1 ... Macroblocks WITHIN a diagonal are mutually independent."

> "DISPATCH SHAPE: gpu_compute_dispatch_encode() dispatches exactly
> min(d+1, width_in_mbs, height_in_mbs, width_in_mbs+height_in_mbs-1-d) workgroups in the X dimension for
> diagonal d"

and the cost it names for the alternative (their P path):

> "residual_predict.comp (still used, unmodified, for P-slices only) dispatches every macroblock of the
> frame in one fully-parallel pass with no cross-MB ordering, so it deliberately reads source-frame
> neighbor pixels instead - a correctly-formed but not decoder-accurate approximation for I-slices."

The chroma trap, `docs/DEVLOG.md` section 12.2, measured against a real libx264 encode of identical
captured frames at matched QP, scored against the same ground truth:

| QP | Metric | Before | After |
|---|---|---|---|
| 22 | avg PSNR gap | ~1 dB (baseline, QPc approx QPy here) | 1.30 dB (U 0.94 / V 0.98) |
| 32 | chroma-U / chroma-V gap | 8.2 dB / 6.4 dB | U 1.44 dB / V 1.27 dB |
| 42 | chroma-U / chroma-V gap | 12.6 dB / 13.6 dB | U 0.20 dB / V 0.37 dB |
| 42 | overall avg PSNR | notably worse than x264 | 39.55 dB against x264's 38.96 dB |

The cause was quantising chroma at QPy instead of the Table 8-15 QPc.

**Application.** Two things.
1. **This specific bug is not ours.** We implement Table 8-15 (`src/h264_tables.cpp:164-165`,
   `src/encoder.cpp:38-50`). Their numbers nonetheless calibrate the shape: a chroma-only defect on this
   hardware produced exactly the 4 to 13 dB chroma-only gap signature we are looking at, so a chroma
   deficit of 3.8 to 6.0 dB with a luma deficit of only 1.3 to 2.3 dB is a chroma-specific defect, not a
   general quality shortfall. Worth re-deriving ours from first principles before attributing all of it
   to mode decision.
2. Their methodology note is worth copying: the bug was invisible to any metric comparing the encoder's
   own two internal buffers, and only an independent comparison against libx264 on identical source at
   matched QP found it. Our `--compare` against the inbox encoder is that independent metric; keep it.

### S9. Momcilovic, Ilic, Roma, Sousa (GPU inter loop, and the predictor dependency)

**Citation.** S. Momcilovic, A. Ilic, N. Roma, L. Sousa, "Efficient Parallel Video Encoding on
Heterogeneous Systems", *First NESUS Workshop*, vol. I, no. 1, October 2014, INESC-ID / IST-TU Lisbon.
Read at `http://web.tecnico.ulisboa.pt/~ist14359/cv/doc/nesus14.pdf`.

The one fact we need, section III:

> "In order to relax spatial data dependences imposed by the definition of SA center, a set of temporary
> dependent predictors was analyzed in [12]. It was observed that the best MV found for the 16x16
> partitioning mode in the previous frame for the collocated MB represents a good compromise for the SA
> center predictor. In fact, this predictor is only used herein to compute the SA center, while the
> selected MVs are then post-computed according to real median vectors of the neighboring MBs."

Measured: real-time full HD with a 64x64 search area and exhaustive motion estimation on CPU+GPU
systems; the paper is about load balancing, so the rest does not transfer.

**Application.** This is the GPU answer to our broken MV rate term. The clause 8.4.1.3 median predictor
is a raster dependency, so it cannot be used inside a single fully-parallel dispatch. Two options, both
used in the literature: use the collocated 16x16 vector of the previous picture as the predictor for the
*cost* during the search, or run the search fully parallel and then recompute the costs with the real
median predictor in a second, cheap pass. Either removes the "cost measured from (0,0)" error in
`cs_me.hlsl:236` without serialising anything.

### S10. Our own measurements (primary)

`<BC250_ROOT>\<BC250_ROOT>\scratch\m15\video-encode\LAB-B19-RESULT.md` (unit A, 2026-10-06, 60 pictures, bit exact
against unit A's inbox decoder in 82/82 cases) and `driver/umd/mft-h264/README.md:193-209`.

- 1080p: ours serial 11.51 ms (GPU busy 6.53 ms, readback 7.57 ms, CAVLC 3.42 ms CPU), pipelined 4.55 ms
  (219.8/s); inbox CPU encoder 2.65 ms (377.0/s).
- Quality at the nominal rate, 1080p mean PSNR, ours against inbox: Y 43.75 / 46.06, Cb 40.56 / 45.50,
  Cr 39.16 / 45.16, with the inbox spending 4 to 7 % more bits.
- README: "at a quantiser that matches our byte count to the inbox encoder's within 1 %, our luma is 0.3
  to 1.1 dB behind it and our chroma 1.7 to 3.6 dB behind" and the gap widens across the group of
  pictures: "from there every predicted picture loses a little more, down to 3.1 dB behind on luma by
  picture 34 of a 60 picture group."
- README: `--chroma-qp-offset` swept; -6 reached parity on Cb while costing 2.1 dB of luma.

**The widening-across-the-GOP shape is the strongest single clue in our own data.** A deficit that grows
monotonically through a 60-picture group is drift in the prediction loop, not a per-picture decision
error. The two tools that stop that drift are intra macroblocks in P pictures (which re-anchor a region
the single vector cannot track) and finer partitions (which stop injecting residual the quantiser then
has to throw away). It is consistent with S2's 16x16-only penalty and with the absence of intra in P.

---

## 3. What this implies for our encoder, ranked

Effort-to-gain ranking, each item with the source it rests on, the file it touches, and its risk to bit
exactness. "Bit exactness" here means our output continuing to decode identically on unit A's inbox
decoder: a mode decision change never breaks it by itself, because it only picks among conformant
choices, but anything that emits new syntax is new normative surface and needs its own conformance case.

### Tier 1 - cheap, no new syntax, no new dispatch

**1.1 SATD instead of SAD in the mode decision.** Sources S3 (-2.5 % to -7.2 % BD-rate, +0.13 to +0.39 dB
BD-PSNR on moving HD, +26 % decision time), S5, S6, S7 (all three production encoders default to SATD).
Touches `shaders/cs_mb.hlsl:265-300` (intra mode choice) and `shaders/cs_me.hlsl:270-292` (sub-pel
refinement; keep SAD for the integer stages, as x264 and Hermansson both do). A 4x4 Hadamard is 8 adds
per row plus 8 per column, entirely in registers, next to the six-tap filters we already run. Expected
GPU cost: a few tenths of a millisecond on the 6.53 ms 1080p budget. **No syntax change. No risk to bit
exactness.**

**1.2 A rate term in the mode decision, with the right lambda.** Sources S5 (`lambda = 2^(QP/6-2)`,
`cost += lambda * bs_size_ue(mode)`), S6 (identical table), S7 (same formula, derivation stated).
Touches `src/encoder.cpp:274` (replace the linear lambda), `shaders/cs_mb.hlsl:266-300` (add
`lambda * bs_size_ue(mode)` to each intra mode, and the `3*lambda` discount for the predicted mode once
we have one). Today our intra mode choice is pure SAD, so the DC mode is never given its rate advantage.
**No syntax change. No risk to bit exactness.**

**1.3 Fix the motion vector rate term.** Sources S5 (`lambda * (2*log2(mvd+1) + 1.718)`, indexed by the
difference from the predictor), S9 (collocated previous-picture vector as a parallel-safe predictor).
Touches `shaders/cs_me.hlsl:236, 288`. Two steps: replace the linear term with a small log2 table in a
constant buffer, and feed a predictor that is not (0,0). **No syntax change. No risk to bit exactness.**
This one is the most likely explanation for why a lambda *scale* sweep was flat.

**1.4 P_Skip only when 16x16 won and the residual is empty.** Sources S3 (the exact bias described), S7
(`if best.part == PART_16X16 && skip_ok && best.mvs[0] == skip_mv && res.cbp == 0`). Today we snap to
(0,0) inside `cs_me` on a SAD slack with `skipBiasScale = 0`. The CPU side already has the cbp, so this
is a condition, not a computation. **No syntax change beyond what we already emit.**

**1.5 A chroma veto on P_Skip.** Source S6 (`CheckChromaCost`, thresholds 640 per plane and 128 for the
"smallest invisible" luma difference, with the reasoning recorded in the source). Directly aimed at our
chroma deficit, and the cheapest chroma-specific item in the sweep: two 8x8 chroma SADs per macroblock.
**No syntax change.**

### Tier 2 - new syntax, same dispatch shape

**2.1 16x8, 8x16 and 8x8 partitions.** Sources S2 (16x16-only costs 9 to 15 % bit rate and 0.1 to 0.2 dB
on moving content; adding 16x8/8x16/8x8 recovers most of it for about +10 % encoder complexity), S4 (the
SAD tree: score the sixteen 4x4 blocks once and sum into every shape), S5 (`i_satd8x8` reuse and the
16x8 estimate from it), S6 (`TryModeMerge`: 16x8 and 8x16 come free from equal 8x8 vectors), S7 (16x8 and
8x16 seeded from the 8x8 results).

How it fits our shader. `cs_me.hlsl` already keeps per-lane partials (`gPart[9*32]`, each lane summing
eight samples of a candidate). Keeping those partials per 4x4 block instead of collapsing them to one
number per candidate gives all sixteen 4x4 costs at the same arithmetic cost, and every partition shape
is a sum of them. Only the 8x8 search needs real extra search points, and S6's variance gate decides per
macroblock whether to spend them. Groupshared is the constraint: `cs_me` is already occupancy-limited at
9.8 KB of the 64 KB per CU, so the 4x4 partials must replace `gPart`, not be added next to it.

Syntax: `mb_type` for P_L0_L0_16x8 and P_L0_L0_8x16, `sub_mb_type` for P_8x8, one `mvd_l0` pair per
partition, and the clause 8.4.1.3 predictor rules per partition shape. CAVLC `nC` neighbour derivation
is unchanged (it is per 4x4 block already), and the deblocking boundary strength gains internal edges
with different vectors, which `cs_deblock.hlsl` must see. **New syntax: yes. Needs its own bit-exactness
cases before it can be trusted.**

**2.2 Intra macroblocks in P pictures, with `constrained_intra_pred_flag = 1`.** Sources S1 (the flag's
normative meaning), S6 (one 16x16 SATD per mode, compared against the P16x16 luma cost), S7 (intra in P
gated by the inter cost, "for occlusions and scene changes"), S10 (our deficit widens monotonically
across the group of pictures, which is what intra in P is for).

This is the item that `shaders/cs_mb.hlsl:14-16` explicitly declined, with the correct reason: intra
prediction reads reconstructed neighbours, which would put the P picture on the anti-diagonal.
`constrained_intra_pred_flag = 1` removes that reason for the common case, because an intra macroblock
whose four neighbours are inter has no available neighbours and predicts DC 128 with no dependency at
all. The remaining dependencies are between *adjacent* intra macroblocks in a P picture, which form
small clusters, not a picture-wide wavefront. The practical shape: one fully parallel pass marks the
macroblocks where intra wins, a second pass handles only the clusters.

Cost: the intra test itself is four 16x16 SATDs per macroblock in the pass that already has the source
and the prediction in groupshared. The flag costs coding efficiency wherever an intra macroblock does
have intra neighbours, which by construction is rare if intra in P is rare.

Syntax: `mb_type` values for I_16x16 inside a P slice, `intra_chroma_pred_mode`, the PPS flag, and the
deblocking boundary strength rules that treat an intra edge as bS 3/4 (`cs_deblock.hlsl:104-106` already
reads an intra bit from MbInfo, so that part exists). **New syntax: yes.**

### Tier 3 - only if Tier 1 and 2 do not close T3

**3.1 Intra_4x4.** Sources S1 (nine modes, "well suited for coding of parts of a picture with significant
detail"), S4 (the two 4x4 modes that need the upper-right neighbour can be dropped for under 1 % on I
frames; seven steps and a parallel degree of 4 inside a macroblock), S5 and S7 (both gate it behind
Intra_16x16 so that it only runs when 16x16 is already close). Intra_4x4 has a 4x4-level dependency
inside the macroblock on top of the macroblock-level one, which is the hardest thing in this sweep to
run well on a GPU. It is also already covered for its main purpose, detailed I pictures, by whatever
quantiser we use. Defer.

**3.2 Sub-8x8 partitions.** Sources S2 ("block sizes smaller than 8x8 ... do not provide significant
benefits compared with the 4 block size mode"), S3 (0 % of macroblocks on vidyo1 and 0.2 to 0.5 %
elsewhere, on HD), S6 (disabled outright in OpenH264). At 1080p they are close to dead weight. Skip.

**3.3 True rate-distortion mode decision.** Source S2 (worth 10 to 21 % bit rate on CIF at 2 to 3x the
encode time), S3 (High RDO alone -3.2 % to -19.1 % BD-rate at +37 to +47 % time, but +12.5 % on one
sequence, so it is not free). This needs the real bit count, which for us lives in the CPU CAVLC stage,
so it would couple the GPU decision to the CPU entropy coder and break the pipeline that gets us from
11.51 ms to 4.55 ms. Not worth it until the CAVLC is on the GPU.

### Hypotheses to test, not conclusions

**H1. Our quantiser rounding is about twice as aggressive as x264's.** We use `f = 2^qbits/3` (intra) and
`2^qbits/6` (inter), i.e. 10.7/32 and 5.3/32 of the step, on luma and chroma alike
(`shaders/h264_common.hlsli:250-294`). x264 uses 21/32 (intra) and 11/32 (inter), on luma and chroma
alike (S5, `common/set.c:81-83` with `common/set.h:30-36` and `common/base.c:456-457`). A larger dead
zone saves bits as well as quality, so only a rate-matched sweep settles the sign. This is one constant
and one sweep, and it touches both luma and chroma, which no other item in this sweep does.

**H2. The chroma gap is partly mechanical, not decisional.** S1 fixes that chroma has no vector of its
own and is interpolated bilinearly at one-eighth sample accuracy from the luma vector. With one vector
per macroblock, a vector that is right for most of a macroblock is wrong for all of its chroma in the
part it misses. If that is the dominant term, the chroma gap should close with 2.1 (partitions) more
than with 1.5 (chroma veto) or any chroma-only tuning. The cheap test: re-measure the chroma gap against
the inbox encoder on a static scene and on a scene with object motion, separately. S8's experience says
to do that comparison against an independent encoder on identical source at matched QP, not against our
own internal buffers.

**H3. Our intra chroma mode decision has no rate term.** `cs_mb.hlsl:278-291` picks the chroma mode by
pure SAD over both planes. Both x264 (S5, `lambda * bs_size_ue(i_mode)`) and OpenH264 (S6,
`iLambda * BsSizeUE(g_kiMapModeIntraChroma[iCurMode])`) add the mode's own bit cost, which systematically
favours DC. Covered by item 1.2, listed here because it is chroma-specific.

---

## 4. Sources I could not open

- **T. Wiegand, H. Schwarz, A. Joch, F. Kossentini, G. J. Sullivan, "Rate-Constrained Coder Control and
  Comparison of Video Coding Standards", IEEE TCSVT 13(7):688-703, July 2003, DOI
  10.1109/TCSVT.2003.815168.** The canonical source for the lambda formula and the Lagrangian mode
  decision. IEEE paywalled; OpenAlex lists it as not open access; the Fraunhofer Publica mirrors
  (`publica.fraunhofer.de/handle/publica/203630`) returned an access-denied page; the OpenAccess link
  Semantic Scholar holds (`ip.hhi.de/imagecom_G1/assets/pdfs/csvt_ratecontrol_0305.pdf`) no longer
  resolves; `iphome.hhi.de` serves an HTML error page with a 200 status for every filename tried.
  **Mitigation:** the two formulas this paper is cited for are stated verbatim in the source of three
  independent production encoders (S5, S6, S7), which agree element for element on the integer lambda
  table, so nothing in this document depends on the inaccessible text.
- **N.-M. Cheung, O. C. Au, M.-C. Kung, P. H. W. Wong, C. H. Liu, "Highly Parallel Rate-Distortion
  Optimized Intra-Mode Decision on Multicore Graphics Processors", IEEE TCSVT 19(11):1692-1703, 2009,
  DOI 10.1109/TCSVT.2009.2031515.** The closest paper in the literature to item 2.2: it proposes
  "novel greedy-based encoding orders to achieve highly parallel processing of data blocks" against the
  macroblock dependency. IEEE paywalled; the HKUST repository record
  (`repository.hkust.edu.hk/ir/Record/1783.1-17946`) redirect-loops; CiteSeerX holds only a summary
  record. Only the abstract was read, so no number from it is quoted here. **Mitigation:** S4 covers the
  same ground with an open full text and measured numbers, and S1's `constrained_intra_pred_flag` gives
  us a different and, for our case, better answer than reordering.
- **N.-M. Cheung, X. Fan, O. C. Au, M.-C. Kung, "Video Coding on Multicore Graphics Processors", IEEE
  Signal Processing Magazine 27(2):79-89, 2010, DOI 10.1109/MSP.2009.935416.** Survey; paywalled, no
  open location in OpenAlex.
- **J. Ostermann, J. Bormans, P. List, D. Marpe, M. Narroschke, F. Pereira, T. Stockhammer, T. Wedi,
  "Video Coding with H.264/AVC: Tools, Performance, and Complexity", IEEE Circuits and Systems Magazine
  4(1):7-28, 2004, DOI 10.1109/MCAS.2004.1286980.** Paywalled; Fraunhofer Publica mirror returned
  access-denied. **Mitigation:** S2 covers tool-by-tool ablation with an open full text.
- **S. Saponara, K. Denolf, G. Lafruit, C. Blanch, J. Bormans, "Performance and Complexity Co-evaluation
  of the Advanced Video Coding Standard for Cost-Effective Multimedia Communications", EURASIP JASP
  2004, DOI 10.1155/S111086570431019X.** Listed as open access, but SpringerOpen now redirects to
  link.springer.com behind an authorisation gate and the PDF endpoints return a 3 KB stub.
- **G. J. Sullivan, T. Wiegand, "Rate-Distortion Optimization for Video Compression", IEEE Signal
  Processing Magazine 15(6):74-90, 1998, DOI 10.1109/79.733497.** Paywalled; no open location.
- **Zhu, Liu, Wang, Han, "Fast prediction mode decision with Hadamard transform based rate-distortion
  cost estimation for HEVC intra coding", IEEE ICIP 2013, DOI 10.1109/ICIP.2013.6738407**, and
  **J. M. Moon, J. H. Kim, "A New Low-Complexity Integer Distortion Estimation Method for H.264/AVC
  Encoder", IEEE TCSVT 20(2):207-212, 2010, DOI 10.1109/TCSVT.2009.2031389.** Both are on-topic for the
  SATD-based cost approximation; both paywalled and not pursued further once S3 supplied measured
  BD-rate for the same question.

Unpaywall could not be used at all: its API rejected the project's contact address
(`research@example.com`) with HTTP 422 on every request, and this workspace's rules forbid substituting a
real address without the owner's word. OpenAlex and Crossref were used instead; neither needs an address.

---

## 5. What the literature does not settle for us

- **No source measures a luma-only against a chroma-aware mode decision in dB.** The three production
  encoders all include chroma in the intra-versus-inter comparison (S5) or use it as a veto (S6), and
  OpenH264 wrote down a visual argument rather than a PSNR one. Our own `--compare` harness is the only
  instrument that can answer this for our content; the experiment is item 1.5 plus H2.
- **No source measures intra-in-P on its own.** Every encoder that has it has it unconditionally, so no
  ablation exists. S10's widening-across-the-GOP curve is our own best evidence that it matters here, and
  it is suggestive, not conclusive.
- **The two open ablations disagree about SATD**, and the disagreement is informative rather than a
  problem: S2 finds it worthless (under 0.2 % bit rate) in an encoder that already runs full RD on
  CIF/QCIF; S3 finds it worth -2.5 % to -7.2 % BD-rate in a low-complexity HD encoder whose decision is
  the approximation. We are the second case. If a future version of our encoder ever gains a real RD
  decision, expect the SATD gain to shrink, not to add.
- **Nothing in the literature is specific to gfx1013, wave32/wave64 or a 9.8 KB groupshared budget.** The
  occupancy arithmetic for every item above has to be done against our own shader, and S4's and S9's
  speedups were measured on NVIDIA parts of 2010-2014 and do not transfer as numbers.
