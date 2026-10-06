# Verification of h264-encoder-literature-mode-decision.md

Date: 2026-10-06. Checker: skeptical re-read of every source in
`research/h264-encoder-literature-mode-decision.md` at the source, plus our own primary evidence for every claim the sweep made
about our encoder.

Scope, per the brief: (1) is the citation real and correct, (2) does each quoted sentence appear
verbatim at the stated place, (3) do the numbers and their conditions match, (4) is the claim
attributed to the source what the source actually says.

Verdict counts: **8 VERIFIED, 2 CORRECTED, 0 REJECTED** among S1-S10 as citations. Four of the
sweep's **own derivations** are **REJECTED**: hypothesis H1, hypothesis H3, item 1.4 and item 1.5.
Two of these are the kind of error that would have cost lab time, so they lead.

Research only. Nothing was built, deployed or run on the lab for this check.

---

## 0. Headline: two of the sweep's ranked items are already done or are no-ops, and H1 is arithmetically wrong

### 0.1 H1 is wrong by a factor of two. Our quantiser rounding already equals x264's defaults.

The sweep's hypothesis H1 says our dead zone is "about twice as aggressive as x264's": ours
10.7/32 and 5.3/32 of the step against x264's 21/32 and 11/32.

The x264 code the sweep quoted is quoted correctly. The arithmetic drawn from it is not.
x264 quantises with (`common/quant.c`, `QUANT_ONE`):

```
(coef) = ((f) + (uint32_t)(coef)) * (mf) >> 16;
```

so one quantiser step in coefficient units is `65536/mf`, and the rounding offset is `f` in those
same units. `common/set.c:179` sets

```
h->quant4_bias[i_list][q][i] = X264_MIN( DIV(deadzone[i_list]<<10, j), (1<<15)/j );
h->quant4_bias0[i_list][q][i] = (1<<15)/j;
```

with `j` the same `mf`. The offset as a fraction of a step is therefore

    f / step = (deadzone * 1024 / mf) / (65536 / mf) = deadzone / 64,   not deadzone / 32.

The second line is the check: `quant4_bias0 = (1<<15)/j` is what the source comment on the line above
calls "round to nearest", i.e. exactly half a step, and `(1<<15) = 32<<10`, so `deadzone = 32` is half
a step and `deadzone = 21` is 21/64 of one.

| | intra | inter |
|---|---|---|
| x264 default (`deadzone[] = {21, 11, 21, 11}`) | 21/64 = **0.328** | 11/64 = **0.172** |
| ours (`f = 2^qbits/3` and `2^qbits/6`) | 1/3 = **0.333** | 1/6 = **0.167** |

The two agree to within 2 and 3 per cent, on luma and on chroma alike, and our DC variants
(`QuantLumaDc`, `QuantChromaDc`, which scale `f` by 4 and 2 against shifts of `qbits+2` and `qbits+1`)
carry the same fractions. **H1 is REJECTED: there is nothing to sweep here.** Whoever wrote
`h264_common.hlsli` picked 1/3 and 1/6 for a reason, and the reason holds.

### 0.2 H3 is refuted by our own measurement, which the sweep read and did not use

H3 says our intra chroma mode decision is missing its rate term and that this is chroma-specific.
The first half is true (`cs_mb.hlsl:278-291` is pure SAD). The second half cannot be the chroma gap,
because our intra chroma is **better** than the inbox encoder's, and the sweep's own source S10 says
so one sentence away from the lines it quoted (`README.md:195-198`):

> "on the intra picture we are 1.4 dB behind on luma and 3 dB **ahead** on chroma, and from there
> every predicted picture loses a little more"

We have no intra macroblocks in P pictures, so our intra chroma mode decision runs **only** in I
pictures, and in I pictures our chroma wins by 3 dB. **H3 is REJECTED as an explanation of the chroma
gap.** Adding the `lambda * bs_size_ue(mode)` term is still right (item 1.2), but it is a general
correctness item, not a chroma lever.

The same sentence is the strongest evidence for **H2**, which the sweep ranked below H1 and H3:
chroma ahead on the I picture, chroma collapsing through the P pictures, is precisely the signature of
"one luma vector per macroblock, chroma interpolated from it" (S1) plus no re-anchoring (no intra in
P). H2 should be the first hypothesis, not the second.

### 0.3 Item 1.4 is already implemented in our encoder

The sweep ranks "P_Skip only when 16x16 won and the residual is empty" as Tier 1 work. It is already
there. `src/h264_cavlc.cpp:249-252`:

```
if (m_pSlice && !intra && mb.cbpLuma == 0 && mb.cbpChroma == 0) {
    int32_t spx = 0, spy = 0;
    SkipMvPred(mbx, mby, &spx, &spy);
    if (spx == mb.mvx && spy == mb.mvy) {
```

Both conditions Hermansson (S3) and squeeze264 (S7) describe - the macroblock must be the 16x16 inter
mode and the residual must be empty - are enforced, plus the clause 8.4.1.1 predictor match. The bias
Hermansson warns about comes from `gSkipBias`, and `skipBiasScale = 0`, so it is off. **Item 1.4 is
REJECTED as work: no change to make.**

### 0.4 Item 1.5 (a chroma veto on P_Skip) cannot change our picture quality at all

This follows from 0.3. In our encoder P_Skip is a **pure syntax decision taken after the macroblock is
coded**: it is emitted only when the chosen vector already equals the skip predictor and the whole
coded block pattern is zero. Coding that macroblock as P_L0_16x16 instead would use the same vector
and the same (empty) residual, so the reconstruction is bit-identical either way. A veto would spend
more bits for exactly the same pixels.

OpenH264's `CheckChromaCost` is not this. It runs *before* the mode decision and decides whether to
take the skip mode early, which changes the vector and therefore the prediction. We have no such early
exit. **Item 1.5 is REJECTED as specified.** The real chroma-aware item for us is chroma distortion in
a P-picture mode decision - and we have no P-picture mode decision at all today, so that work is item
2.1 and item 2.2, not a separate cheap item.

### 0.5 Net effect on the ranking

Tier 1 loses two of its five items (1.4 done, 1.5 void) and its chroma-specific content. What survives
is 1.1 (SATD), 1.2 (rate term and the right lambda) and 1.3 (the motion vector rate term), all of which
I verified against their sources and all of which are sound. Tier 2 is unchanged and is now carrying
the whole chroma argument, which matches H2.

One operational note the sweep does not make: our fixed-quantiser conformance cases are pinned to the
exact byte (`README.md:186-192`: the byte counts of the constant-bit-rate runs are pinned "with a
tolerance and not to the exact byte, which the fixed-quantiser cases are"). Every Tier 1 item changes
the bitstream, so all of them re-baseline those expectations on their first commit. This is not a
conformance risk - a mode decision only picks among conformant choices - but it is a test-suite cost
the sweep's "no risk to bit exactness" line hides.

---

## 1. Source-by-source verdicts

### S1. Wiegand, Sullivan, Bjontegaard, Luthra, "Overview of the H.264/AVC Video Coding Standard" - VERIFIED

Citation checked at Crossref: DOI 10.1109/TCSVT.2003.815165 resolves to that title, those four
authors, IEEE TCSVT 13(7):560-576, July 2003. The PDF at the UBC URL carries the journal running head
"560 IEEE TRANSACTIONS ON CIRCUITS AND SYSTEMS FOR VIDEO TECHNOLOGY, VOL. 13, NO. 7, JULY 2003", so it
is the published article, as the sweep said.

All four quotations appear verbatim, at the stated places. Section letters confirmed from the headings:
`G. Intra-Frame Prediction`, `H. Inter-Frame Prediction`, `H.1) Inter-Frame Prediction in P Slices`.

| Quote | Where the sweep put it | Where it is |
|---|---|---|
| "Partitions with luma block sizes of 16x16, 16x8, 8x16, and 8x8 ..." | H.1, p. 569 | H.1, p. 569 |
| "The prediction values for the chroma component are always obtained by bilinear interpolation ..." | H.1, p. 570 | H.1, p. 570 |
| "The Intra_4x4 mode is based on predicting each 4x4 luma block separately ..." | G, p. 568 | G, p. 568 |
| "This may incur error propagation ... a constrained intra coding mode can be signaled ..." | G, p. 568 | G, p. 568 |

The sweep's disclosure that the PDF text layer renders "16x16" as "16 16" is accurate and the
restoration is correct in every case.

**One gap in the use, not in the quote.** The whole of item 2.2 rests on what
`constrained_intra_pred_flag` does, and an overview article is the wrong authority for normative
behaviour. The overview says prediction is allowed "only from intra-coded neighboring macroblocks"; it
does not say what an intra macroblock predicts from when *no* neighbour is available, which is the
load-bearing step in the sweep's argument ("predicts DC 128 with no dependency at all"). That rule is
in the Recommendation (Intra_16x16 DC falls back to `1 << (BitDepth - 1)` when neither neighbour is
available), not in S1. See section 3 for the missing source.

### S2. Krichene Zrida, Ammari, Abid, Jemai, "Complexity/Performance Analysis of a H.264/AVC Video Encoder" - CORRECTED (minor)

DOI 10.5772/16821 resolves at Crossref to that title, those four authors, book *Recent Advances on
Video Coding*, 2011, book chapter. The IntechOpen chapter page gives editor Javier Del Ser Lorente, as
the sweep said. Open access, CC BY-NC-SA 3.0.

**Correction 1 (citation).** The chapter is **pp. 27-50**, not 25-50. The downloaded chapter PDF has 24
pages; its running heads are "28" on PDF page 2 and "36", "37", "38" on PDF pages 10, 11, 12, which
puts page 1 at book page 27 and the last page at 50.

Because the text layer interleaves the table columns beyond repair, I rendered pages 36, 37 and 38 as
images and read the tables off the rendered page. **Every number in the sweep's Table 5 and Table 7
transcriptions is correct**, including each derived percentage, which I recomputed:

- Table 5, 4 partitions: +0.67/106.67 = 0.6 %, +14.53/678.46 = 2.1 %, +5.19/131.23 = 4.0 %,
  +1.32/81.44 = 1.6 %, +0.45/19.42 = 2.3 %, +0.18/30.36 = 0.6 %. All match.
- Table 5, 1 partition: 2.2 %, 8.6 %, 11.7 %, 11.2 %, 15.2 %, 9.3 %. All match.
- Table 7: 20.5 %, 21.2 %, 9.8 %, 39.9 %, 16.8 %. All match.

The row-block assignment the sweep used is also right, and it is not obvious: on the rendered page the
`dTEC` row belongs to the block **below** the one its printed position suggests. The rendered table
shows block "4" as {dTEC, ME C, dBit rate, dPSNR-Y} = {-11.32 ... -9.11 ..., 27.42 ..., 0.67 ...,
-0.06 ...} and block "1" as {-11.62 ... -14.17, 25.67 ..., 2.3 ..., -0.09 ...}. The sweep's "-9.1 % to
-11.9 % at 4 partitions and -11.2 % to -14.2 % at 1" is correct.

"Both against 7" is also correct, and I checked it rather than assuming: Table 6's absolute bit rate
for Bridge-close at 5 reference frames is 107.34, which is Table 5's 7-partition baseline 106.67 plus
the "4" delta 0.67; and Table 7's absolute 114.04 is Table 6's 107.34 plus the "3" delta 6.70. Deltas
in these tables are measured against the table's own baseline row, so the "1" row's deltas are against
"7". The chapter's prose ("an encoding time further reduced 10% average") is looser than its table.

**Correction 2 (small).** For Table 8 the sweep writes "PSNR-Y by -0.01 to -0.07 dB". The rendered row
is -0.01, -0.04, -0.05, **+0.01**, -0.03, -0.06, -0.07: one sequence (Bridge-far QCIF) gains 0.01 dB.
Everything else in the Table 8 reading, including "-1.41 % to -3.92 % complexity" and "at most +1.49 on
719.97", is exact.

**One caveat on the use of Table 7.** The sweep summarises it as "a real rate-distortion decision ... is
worth roughly 10 to 21 % bit rate". The same table shows PSNR-Y *rising* by 0.04 to 0.32 dB on four of
the five sequences when RD is switched off, because the encoder then spends more bits. The honest
reading is that the bit-rate column alone overstates the RD gain; a BD-rate would be smaller. The sweep
printed both columns, so the data is there, but the sentence is loose.

The two long quotations (section 4.3.3 on the four block sizes, and section 4.3.6 on the Hadamard)
appear verbatim.

### S3. Hermansson, "Optimizing an H.264 video encoder for real-time HD-video encoding" - VERIFIED

Title page confirms: Per Hermansson, Master of Science Thesis, KTH Information and Communication
Technology, Stockholm 2011, TRITA-ICT-EX-2011:74, Master's thesis at Ericsson Research.

Table 4.11 is on thesis page 46 (PDF page 48). I rendered that page and read it. **Every one of the
twelve rows the sweep transcribed is exact**, including BDBR, BDPSNR, SADs/Modes and dTime. The table
is independently corroborated inside the thesis: Table 4.15 on page 48 repeats the "Combined" column
(vidyo1 -1.7 / 0.06, blue_sky -0.3 / 0.02, pedestrian -13.6 / 0.7, riverbed -19.6 / 1.25), which also
fixes the sequence order the sweep assumed.

All quotations verbatim: the "Furthermore using SATD instead of SAD ..." passage, "The conclusion is
therefore that in order to get additional compression and quality ...", the Ref 1 definition, the
sub-8x8 experiment, the two algorithm descriptions, and the whole "SKIP-prediction" paragraph.

**Conditions the sweep left out, which matter for how far the numbers travel.** Section 4.1: all four
sequences are **720p at 25 fps, 300 frames, constant QP, I-P-P-P, measured on an Intel Core 2 Duo at
2.40 GHz**; the comparison runs with one reference frame, CAVLC and assembly optimisations off. The
sweep says "HD", which is true of 720p but reads as 1080p.

**A detail that supports the sweep's own recommendation.** The thesis adds "(but not during full-pixel
search)" to its SATD configuration. The sweep's item 1.1 says to keep SAD for the integer stages; the
source agrees explicitly, which strengthens the item.

Two small placements: the sub-8x8 passage straddles pages 30-31 rather than 31-32, and the thesis
spells the cited author "Haung et al." where the sweep wrote "Huang et al." (the sweep's spelling is
the correct one for that paper).

### S4. Su, Wen, Wu, Ren, Zhang, "Efficient Parallel Video Processing Techniques on GPU" - CORRECTED (one ordering)

DOI 10.1155/2014/716020 resolves at Crossref to that title, those five authors, The Scientific World
Journal, vol. 2014. Open access. I read the full text from Europe PMC rather than the PMC page, so the
quotations below are against the publisher's XML.

All five quotations verbatim, at the stated sections:

- Section 5.1.3: "Experiments to multiple test sequences show that some prediction methods, needing
  upper right reconstructed pixels (the third and the seventh method of the 4 x 4 prediction and the
  third of the 16 x 16 prediction), play a slight role. It increases the bit-rate for I-frames by less
  than 1%" - verbatim. The source continues "and has an even smaller impact on P-frames when dropping
  these three prediction ways", which strengthens the sweep's point rather than weakening it.
- Section 5.1.3: the "maximal number of blocks ... is only 2" and "7 steps ... parallel degree can
  reach 4" sentences - verbatim, with the sweep's ellipsis spanning the quote above.
- Section 5.1.2: the multislice and wave-front sentences - verbatim.
- Section 4: "a MB is divided into variable block sizes, such as 8 x 4, 4 x 8, 8 x 8, 16 x 8, 8 x 16,
  and 16 x 16" and "Using the generated SAD values of 4 x 4 subblocks, the SAD value for other sizes of
  block can be calculated" - both verbatim, the second in the CUDA implementation paragraph of the same
  section.
- Section 6.2: "a loss of PSNR value about 0.35 dB ~ 0.54 dB, 0.14 dB ~ 0.77 dB, and 0.33 dB ~ 0.57 dB
  for D1, 720 p, and 1080 p video formats, respectively" - matches the sweep's three ranges exactly.
- Section 6.3: intra prediction "about from 2.8 to 8.8" - matches.
- 30 fps at 720p and 20 fps at 1080p - matches.

**Correction (ordering).** The sweep writes "about 20x on a Tesla C2050, 16x on a GTX 460, 11x on a GTX
260; motion estimation 13x, 18x, 25x on those three". Section 6.3 reads "the interprediction achieves
the maximal speedup. The speedup ratios on three GPUs are about 13, 18, and 25, respectively", and the
figures it refers to are numbered 16 (GTX260), 17 (GTX460), 18 (C2050). The three interprediction
numbers are therefore **GTX260 13, GTX460 18, C2050 25**, i.e. the reverse of the order the sweep's
sentence implies. Also, the body says "more than 19 for 1080 p format on C2050"; only the abstract
rounds to 20.

Nothing in the sweep's reasoning depends on either, and the sweep's own warning that these 2010-2014
NVIDIA speedups do not transfer as numbers is the right call.

### S5. x264 - VERIFIED as quoted; one derived claim rejected (see 0.1), one file attribution corrected

Read from `mirror/x264` at `master`. Every quotation is verbatim and every constant is right:

- `common/tables.c:96` `/* lambda = pow(2,qp/6-2) */`, `:112` `/* lambda2 = pow(lambda,2) * .9 * 256 */`,
  `:133` and `:149` the two trellis lambda comments. All present, in the ranges the sweep gave.
- `encoder/analyse.c` `logs[0] = 0.718f;` and `logs[i] = log2f( i+1 ) * 2.0f + 1.718f;`,
  `h->cost_mv[qp][i] = X264_MIN( (int)(lambda * logs[i] + .5f), UINT16_MAX )`,
  `cost_i4x4_mode[i] = 3*lambda*(i!=8)`. All present.
- The intra block: `i16x16_thresh_lut[11] = { 2, 2, 2, 3, 3, 4, 4, 4, 4, 4, 4 }`, the `i16x16_thresh`
  expression, the "Plane is expensive" comment, `if( a->i_satd_i16x16 > i16x16_thresh ) return;`,
  `int i_cost = lambda * (24+16); /* 24from JVT (SATD0), 16 from base predmode costs */`,
  `i_satd_thresh = X264_MIN3( i_satd_inter, a->i_satd_i16x16, a->i_satd_i8x8 )`. All verbatim.
- Chroma: `satdu[i_mode] + satdv[i_mode] + a->i_lambda * bs_size_ue( i_mode )`, `b_chroma_me = 1` at
  `common/base.c:446`, and the P-slice block at `encoder/analyse.c:3181-3194` quoted exactly, including
  `analysis.i_satd_i16x16 += analysis.i_satd_chroma;`.
- Partition order and thresholds: `i_thresh16x8 = me8x8[1].cost_mv + me8x8[2].cost_mv`, the gate
  `i_cost8x8 < me16x16.cost + i_thresh16x8`, the "Early termination based on the current SATD score of
  partition[0]" comment and its condition, and `a->i_satd8x8[0][i] = m->cost - m->cost_mv;`. All verbatim.
- Intra in P is indeed unconditional, and the type is chosen from
  `X264_MIN3( analysis.i_satd_i16x16, analysis.i_satd_i8x8, analysis.i_satd_i4x4 )` against the inter
  cost, as the sweep says.
- `common/set.c:81-83` and `common/set.h:30-36` and `common/base.c:456-457`: exactly as quoted.

Line numbers drift against `master` by a few lines in places (the `logs[]` loop is nearer 192-197 than
179-200); the code is where the sweep says it is.

**Correction (attribution).** The sweep says x264's motion vector cost is "indexed by the motion vector
difference in quarter-pel units" and cites `encoder/analyse.c`. The table is built there, but the
indexing by difference happens in `encoder/me.c:211-212`
(`const uint16_t *p_cost_mvx = m->p_cost_mv - m->mvp[0];`) with `BITS_MVD( mx, my )` defined at
`encoder/me.c:60-61` as `(p_cost_mvx[(mx)*4] + p_cost_mvy[(my)*4])`. The claim is true; the file is
wrong. I checked this because the sweep's item 1.3 depends on it, and it holds.

**One imprecision.** The sweep says x264 does "P16x16, then P8x8 (only when `!b_early_terminate ||
i_cost8x8 < me16x16.cost`)". That condition governs *selecting* 8x8; the 8x8 search itself
(`mb_analyse_inter_p8x8`) runs unconditionally whenever `X264_ANALYSE_PSUB16x16` is set. The real
"search only if worth it" gate in the sweep's plan comes from OpenH264 (S6), which is where the sweep
put it.

**The derived claim H1 is REJECTED.** See section 0.1. The quoted code is right; the arithmetic on top
of it is off by a factor of two, and our encoder already matches x264's defaults.

### S6. OpenH264 - VERIFIED

Read from `cisco/openh264` at `master`. Everything checks out:

- `encoder_data_tables.cpp:59-67` `g_kiQpCostTable[52]`. I compared it element by element with
  `x264_lambda_tab[0..51]`: **identical in all 52 entries**. The sweep's strongest structural claim
  survives. The sweep reproduced the table as one flat line, which is a reformatting, not a
  transcription error.
- `encoder_ext.cpp:2627` and `:2633`: `pfMdCost` is `pfSampleSad` in `SetFastCodingFunc` and
  `pfSampleSatd` in `SetNormalCodingFunc`, selected at `:2670` by
  `bFastMode = (pCtx->pSvcParam->iComplexityMode == LOW_COMPLEXITY)`. `svc_encode_slice.cpp:699`
  verbatim.
- `svc_base_layer_md.cpp:365-416` `WelsMdI16x16`, with the three-mode combined path, the Plane mode at
  `+ iLambda * 4`, and the per-mode `iLambda * (BsSizeUE (g_kiMapModeI16x16[iCurMode]))`. Correct in
  substance. The sweep's one-line rendering
  `iCurCost = pfMdCost[BLOCK_16x16](...) + iLambda * BsSizeUE(...)` is a condensation of two
  statements, presented as code; it is accurate but it is not a verbatim quote.
- `svc_base_layer_md.cpp:867-931` `WelsMdIntraChroma`: SATD over Cb plus SATD over Cr plus
  `iLambda * BsSizeUE (g_kiMapModeIntraChroma[iCurMode])`. Verbatim.
- `svc_base_layer_md.cpp:1829-1856` `WelsMdFirstIntraMode`: the three quoted lines verbatim, including
  the `//compare cost_p16x16 with cost_i16x16` comment, and the sweep's reading - P16x16 first, then
  I16x16, and only on an intra win do the I4x4 refinement and the chroma search run - is exactly what
  the function does.
- `md.cpp:389-432` `MdInterAnalysisVaaInfo_c`: computes the mean of the four 8x8 SADs, the variance of
  the four, returns 15 below `INTER_VARIANCE_SAD_THRESHOLD` and otherwise a 4-bit above/below mask.
  Exactly as described. `md.h` carries the seven `MBVAASIGN_*` constants with the quoted values.
- `svc_mode_decision.cpp` `WelsMdInterFinePartitionVaaOnScreen`: the `MBVAASIGN_FLAT` early return, the
  `WelsMdP8x8` call, the `TryModeMerge` call, and `#if 0 //Disable for sub8x8 modes for now`. All
  verbatim.
- `TryModeMerge`: the pairwise vector equality, the 16x8 and 8x16 relabelling with no new search, and
  the five-line comment ending "(10/12/2012)". Verbatim. The line
  `pTarMe->uiSadCost = sSrcMe0.uiSadCost + sSrcMe1.uiSadCost;//not precise cost since MVD cost is not
  the same` is in the helper `MergeSub16Me` just above `TryModeMerge`, not inside it.
- `svc_mode_decision.cpp:173-214` `CheckChromaCost`, both `#define`s with their comments, the long
  01/17/13 comment block, and `bChromaTooLarge`. Verbatim. The function returns true when skip is
  allowed, so the sweep's "refuses a P_Skip when the chroma SAD is large" is the right reading.

Line numbers are within one or two of the sweep's throughout.

### S7. squeeze264 - CORRECTED (one quotation is not in the source)

Repository metadata checked through the GitHub API: `useless-husband/squeeze264`, MIT, default branch
`main`, and the repository description is word for word the sentence the sweep quoted as the citation.
Last push 2026-10-05, so this is current work, not an abandoned toy.

**Correction 1.** The sweep puts this in quotation marks:

> "The encoder lacks: B frames, CABAC, 8x8 transforms, multiple reference frames, rate-distortion
> optimization, trellis quantization, SIMD acceleration, and multi-threading."

**That sentence does not appear in the README.** The source text, under "Limitations", is:

> "Baseline tools only: no B frames, no CABAC, no 8x8 transform, no interlace, no weighted prediction,
> one reference frame, one slice per picture. No multi-threading, no SIMD."

and, in the next bullet:

> "Mode decisions use SATD estimates, not true rate-distortion costs; no trellis quantisation, no
> adaptive quantisation, no psycho-visual tuning, no look-ahead or scene-cut detection (a scene change
> inside a GOP is coded as a P frame full of intra macroblocks)."

The sweep's sentence is a faithful summary of the two, so no conclusion changes, but it is a
paraphrase dressed as a quotation.

**Correction 2 (ranges).** The BD-rate table is transcribed exactly (foreman +0.5 / +13.1 / +74.0;
akiyo -4.2 / -0.5 / +69.2; mobile -1.4 / +20.8 / +115.4; shields +1.6 / +14.2 / +111.9). The sweep's
prose summary, "within about +/- 2 to 4 % BD-rate of a matched x264 and within about +13 to +21 % of
x264 `medium`", rounds the matched column badly in one direction (-4.2 to +1.6) and drops akiyo's
**-0.5 %** from the medium column, which actually makes the sweep's own case stronger, not weaker. The
right statement is: -4.2 to +1.6 % against matched x264, and -0.5 to +20.8 % against x264 Baseline
medium.

Everything else verified verbatim: `lambda16` with its two-line doc comment; `bit_cost`; the partition
gate at `src/analysis.rs:177-178` with its comment; the sub-8x8 gate at `:205`; the intra-in-P call at
`:288-290` with "// Intra in a P slice, for occlusions and scene changes."; the Intra4x4 gate comment
at `:94` and `if i16_cost > limit.saturating_add(limit / 4) { return None; }` at `:95`; the chroma
`ue(v)` comment at `:60`; the 16x8/8x16 seeds `[mv8[i * 2], mv8[i * 2 + 1], mv16]` at `:259`; and the
final P_Skip condition at `:296` exactly as quoted.

One condition the sweep could have stated: the "matched tools" column restricts x264 to
`--subme 5 --trellis 0 --ref 1` Baseline, which is the configuration that makes the comparison fair,
and the README says so.

### S8. bc250-encoding-decoding-fix - VERIFIED

Local read-only copy, HEAD `774783d4be9c07a0a85531ec2feaf7df0e335d48`, dated 2026-09-27, as stated. No
code read beyond the two files the sweep names; facts only, per the directory's own warning.

The wavefront quotations from `approach1-compute-encoder/shaders/intra_wavefront.comp` are verbatim
(the sweep rewrapped the comment's line breaks), including the dispatch-shape sentence and the
"DELIBERATE SIMPLIFICATION" note about the P path reading source-frame neighbours.

`docs/DEVLOG.md` section 12.2: the four-row table is reproduced exactly, including "39.55 dB vs.
x264's 38.96 dB". The root cause is stated as the sweep says (chroma quantised at QPy instead of the
Table 8-15 QPc), and the methodology note is accurate: the comparison was against a real libx264
encode of identical captured frames at matched QP, scored against the same ground truth.

The sweep's conclusion that "this specific bug is not ours" is correct: `src/encoder.cpp:38-50` and
`src/h264_tables.cpp:164-165` implement Table 8-15 and the sweep verified it.

**One thing worth adding from the same document.** Section 12.5 reframes 12.2: the live corruption that
motivated the hunt persisted after the QPc fix and was traced upstream of the driver, so 12.2 is "a
real, independently-verified encoder defect that happened to exist and is now closed, not the cause of
the live symptom". That does not weaken the sweep's use of the numbers, and it reinforces the
methodology lesson the sweep drew.

Section 12.3 of the same log is also directly relevant to us and the sweep did not mention it: the four
true-diagonal quarter-pel positions (e/g/p/r) were being built by averaging `j` with an integer
neighbour instead of the two nearest half-pel samples, a genuine spec violation that produced **no
measurable PSNR change** when tested in isolation. Worth knowing before anyone attributes a dB to
interpolation detail.

### S9. Momcilovic, Ilic, Roma, Sousa, "Efficient Parallel Video Encoding on Heterogeneous Systems" - VERIFIED, with an overreach flagged

The PDF at the stated URL carries "First NESUS Workshop - October 2014 - Vol. I, No. 1", those four
authors, INESC-ID / IST-TU Lisbon. The abstract confirms "full HD video format, 64x64 pixels search
area and the exhaustive motion estimation". The quoted passage is verbatim, in section III.

**Overreach.** The sweep offers two options and presents both as "used in the literature": (a) use the
collocated previous-picture 16x16 vector as the predictor for the **cost** during the search, or (b)
search in parallel and recompute the costs with the real median predictor afterwards. The source
supports (b) and explicitly contradicts (a) in the sentence the sweep quoted:

> "In fact, this predictor is only used herein to compute the SA center, while the selected MVs are
> then post-computed according to real median vectors of the neighboring MBs."

The collocated vector is a **search-area centre**, not a rate predictor, and the paper also credits the
observation to its reference [12] rather than measuring it. Option (a) may still be a reasonable idea,
but it is ours, not theirs, and it should not be shipped under this citation.

### S10. Our own measurements - VERIFIED

Every number re-read from `LAB-B19-RESULT.md` and `driver/umd/mft-h264/README.md`:

- 1080p: serial 11.51 ms, GPU busy 6.53 ms, readback 7.57 ms, CAVLC 3.42 ms, pipelined 4.55 ms
  (219.8/s), inbox 2.65 ms (377.0/s). All match.
- PSNR: 1080p Y 43.75 / 46.06, Cb 40.56 / 45.50, Cr 39.16 / 45.16. All match, and the T3 deltas the
  sweep quotes recompute exactly: Y -1.34 (720p) and -2.31 (1080p), chroma -3.82, -4.94, -4.94, -6.00.
- The README quotations on the quantiser-matched comparison, the widening gap and the
  `--chroma-qp-offset` sweep are verbatim.

**One caution on the 7.57 ms figure**, which the sibling check of `h264-encoder-literature-entropy-pipelining.md` established and
which anyone reading this table should carry: that column is the *whole blocking readback*, of which
`lab-e52/out-20261006T081819Z/.../t1080-60.txt` line 12 shows 6.89 ms is waiting for the GPU and only
0.68 ms is transfer and memcpy. The columns are therefore not additive (6.53 GPU busy overlaps the 6.89
wait), and 0.25 + 6.89 + 0.68 + 3.42 = 11.24 ms accounts for the 11.51 ms serial time. Nothing in
h264-encoder-literature-mode-decision.md builds on the readback number, so no finding changes.

**And the omission that matters:** the sweep quoted the "widening across the GOP" sentence but dropped
its first clause, "on the intra picture we are 1.4 dB behind on luma and 3 dB **ahead** on chroma". See
section 0.2.

---

## 2. The sweep's description of our encoder: checked line by line

Every row of the sweep's section 1 table was checked against the `perf-wt` worktree. All of it is
accurate. Confirmed directly:

- `cs_me.hlsl:236` `gCost[tid] = sad + gLambda * uint(abs(ix) + abs(iy));` and `:288`
  `... + gLambda * uint(abs(mx) + abs(my)) / 4u;`. The cost is linear in the vector and measured from
  (0,0), exactly as the sweep says.
- `src/encoder.cpp:274` `gp.lambda = (1u + (qp / 8u)) * m_cfg.lambdaScale / 100u;` with
  `lambdaScale = 300` at `encoder.h:95`. At QP 20 this is 9 against x264's 3; at QP 40 it is 18 against
  x264's 25. Both of the sweep's arithmetic examples are right.
- `src/encoder.h:96` `skipBiasScale = 0`, and `encoder.cpp:280` makes `gp.skipBias` zero from it.
- `cs_mb.hlsl:265-300`: the I-picture mode decision is pure SAD for luma and for chroma, with no rate
  term. One small refinement the sweep's "the DC mode is never given its rate advantage" misses: the
  reduction at `cs_mb.hlsl:296-304` initialises the best cost to `gRedCost[2]` (luma DC) and
  `gRedCost[4]` (chroma DC) and compares with a strict `<`, so DC already wins ties. It gets a
  tie-break, not a rate discount.
- `cs_mb.hlsl:302-308`: P pictures set `gModeY = 0` unconditionally. No P mode decision exists.
- `h264_common.hlsli:250-294`: `f = (1u << qbits) / (intra ? 3u : 6u)` in all three quantisers.
- `cs_deblock.hlsl:103-106` does read an intra bit from MbInfo and returns bS 4 or 3, so the
  deblocking part of item 2.2 is indeed already in place.

**One number to re-derive before planning on it.** The sweep's Tier 2.1 argues from "cs_me is already
occupancy-limited at 9.8 KB". The groupshared arrays declared in `cs_me.hlsl` sum to about 8.9 KB
(gSrcMb 1024, gCost/gSad/gCandX/gCandY 512, gInt 2116, gBRaw 1656, gH 1296, gJ 1296, gPart 1152, plus
16 bytes of scalars). The 9.8 KB figure comes from `LAB-B19-RESULT.md`, presumably after allocation
granularity. The conclusion ("groupshared is the constraint, so per-4x4 partials must replace gPart,
not be added next to it") is unaffected, but the budget should be recomputed from the compiler's
report, not from either number, before the partition work starts.

---

## 3. Primary sources the sweep missed

### 3.1 ITU-T Recommendation H.264 itself

The sweep's Tier 2 is a plan to emit new normative syntax, and its only authority for that syntax is a
2003 overview article. The Recommendation is free from itu.int, and three of its clauses are
load-bearing for the plan:

- **7.4.2.2** `constrained_intra_pred_flag` semantics, which item 2.2 rests on entirely.
- **8.3.1.2 / 8.3.3** intra sample availability and the DC fallback to `1 << (BitDepth - 1)`, which is
  the step that makes the sweep's "an intra macroblock whose four neighbours are inter has no
  dependency at all" true (or not) for each prediction mode.
- **8.4.1.3** the median motion vector predictor per partition shape, which items 1.3 and 2.1 both
  need, and which the sweep only ever cites second hand.

Our repository has no local copy of the Recommendation. Getting one before the Tier 2 work starts is
cheaper than finding the fallback rule wrong on the lab.

### 3.2 minih264 (lieff), CC0-1.0

A fourth from-scratch encoder of exactly our class, and a useful counterweight to S5/S6/S7 because it
agrees with them on structure while disagreeing on lambda. Read from the single header
`minih264e.h` and the README:

- **Intra in P is unconditional**, as in every other encoder in this sweep: `intra_choose_16x16` runs
  for every macroblock after `inter_choose_mode`, and Intra4x4 is run in P slices only at the slower
  speed settings (`// enable intra4x4 on P slices`). A fourth independent vote for item 2.2.
- **Every decision carries a rate term**: the Intra16x16 cost adds
  `MUL_LAMBDA(bitsize_ue(pred_mode_luma + 1), g_lambda_q4[qp])`, the Intra4x4 search takes a
  `MUL_LAMBDA(3, g_lambda_q4[qp])` penalty for any mode other than the predicted one - the same
  `3 * lambda` as x264 - and the partition decision adds `MUL_LAMBDA(nbits[mb_type], g_lambda_q4[qp])`.
  A fourth vote for item 1.2.
- **It uses SAD, not SATD**, and says so under Limitations ("Select prediction mode using Sum of
  Absolute Transform Differences (SATD)" is listed as unsupported), while still measuring **better**
  luma PSNR than x264 veryfast baseline at a 22 per cent larger file on foreman CIF (33.32 against
  32.77 dB, 391 KB against 320 KB). That is a reminder that a PSNR comparison at unmatched rate says
  nothing, which is the discipline our own `--compare` already follows.
- **Its lambda tables are hand-tuned, not analytic.** `g_lambda_q4[52]` is non-monotonic
  (14, 14, ..., 14, 13, 11, 10, 8, 7, 11, 15, ...) and is marked `ADJUSTABLE`. So the sweep's "three
  independent production encoders agree on 2^(QP/6-2)" is true of x264, OpenH264 and squeeze264, and a
  fourth encoder deliberately does not. This does not weaken item 1.2 - a wrong shape is still wrong -
  but it does mean the analytic lambda is a starting point to be measured, not a law.

### 3.3 A gap no source fills: the cost of `constrained_intra_pred_flag`

Item 2.2 asserts that the flag "costs coding efficiency wherever an intra macroblock does have intra
neighbours, which by construction is rare". I looked for a measured number and found none: the flag is
discussed in the error-resilience literature, not quantified as a BD-rate in anything I could open.
Our own `--compare` harness is the only instrument that can price it, and the experiment is one PPS bit
wide. It should be measured before the dispatch design is committed to, not after.

---

## 4. What I did not or could not check

- The five paywalled sources in the sweep's own section 4: I verified each DOI resolves to the stated
  work with the stated authors, year, venue and pages through Crossref and OpenAlex, and I verified the
  access claims. OpenAlex reports `closed` with no OA location for Wiegand et al. 2003b
  (TCSVT.2003.815168), Cheung et al. 2009, Cheung et al. 2010, Ostermann et al. 2004, Sullivan and
  Wiegand 1998, Zhu et al. 2013 and Moon and Kim 2010. Three further mirrors of the Wiegand
  rate-constrained paper that the sweep did not try (NTU, an alternative HHI path, a CMU course page)
  all return 404. The Saponara 2004 EURASIP paper is listed as gold open access, and its SpringerOpen
  PDF endpoint returns a 3038-byte HTML stub, exactly as the sweep reported. **The sweep's section 4 is
  accurate, including its mitigations.**
- The sweep's note that Unpaywall rejected `research@example.com` with HTTP 422: not re-tested. Nothing
  in this document needed it; Crossref and OpenAlex take no address.
- I did not re-run any measurement. Every number about our own encoder here is read from the lab
  archive and the source tree.

---

## 5. Revised ranking

Only the parts that change. Everything else stands as the sweep wrote it.

| Item | Sweep | After this check |
|---|---|---|
| 1.1 SATD in the mode decision | Tier 1 | **Tier 1, first.** Sources verified; S3 even agrees on keeping SAD for the integer search. |
| 1.2 Rate term with lambda = 2^(QP/6-2) | Tier 1 | **Tier 1.** Four encoders now, one of which disagrees on the table but not on the principle. |
| 1.3 Motion vector rate term | Tier 1 | **Tier 1.** Verified; use S9's option (b), a second pass with the real median predictor, not option (a). |
| 1.4 P_Skip discipline | Tier 1 | **Done.** Already in `h264_cavlc.cpp:249-252`. |
| 1.5 Chroma veto on P_Skip | Tier 1 | **Void.** Our P_Skip cannot change a reconstruction. |
| 2.1 16x8 / 8x16 / 8x8 | Tier 2 | **Tier 2, and now the main chroma lever** (H2). |
| 2.2 Intra in P with constrained intra | Tier 2 | **Tier 2.** Price the flag with `--compare` first; no source prices it. |
| H1 quantiser rounding | hypothesis | **Rejected.** Ours already equals x264's defaults. |
| H2 chroma gap is mechanical | hypothesis | **Promoted.** Our I-picture chroma is 3 dB ahead; the gap is born in P. |
| H3 intra chroma rate term | hypothesis | **Rejected as a chroma explanation.** Folded into 1.2. |
