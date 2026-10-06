# Verification of h264-encoder-literature-rate-control.md: rate control and perceptual quality literature

Checked 2026-10-06. Research only: no code changed, nothing built, no lab access, nothing pushed.
Subject: `docs/research/h264-encoder-literature-rate-control.md`.

Method. Every source was fetched again by me, independently of the sweep, and read from the fetched
file: nine PDFs (publisher mirror, author mirror or arXiv), eight x264 source files from the videolan
mirror, five Markdown files from the two BC-250 repositories, two project READMEs, and the local
encoder sources the sweep maps its levers onto. Quotes were matched against the extracted text
character by character where the extraction is reliable (ligatures and the `fi`/`fl` of the IEEE PDFs
excepted). Numbers were re-read from the tables, not from the sweep. DOIs were resolved at
`doi.org` and the metadata read from the Crossref API with `mailto=research@example.com`.
Downloads and extracted text are in the session scratchpad, not in the workspace.

Verdict summary: 16 sources checked. **11 VERIFIED**, **5 CORRECTED**, **0 REJECTED**. No source is
invented; no quote is fabricated. One **fabricated factual detail** (not a quote) was found in the
psy-RD entry, and one **wrong DOI**. Three corrections change a conclusion of the sweep; the rest are
local. Section 1's description of our own encoder is accurate except for two items. The sweep's two
strongest findings (4.1 motion cost, 4.3 deadzone is a dead lever) survive checking intact.

---

## 1. What changes in the sweep's conclusions

Ordered by how much it matters. Each item is argued in the per-source entry named after it.

**(a) The mechanism the sweep gives for adaptive quantization is inverted (S2).** The sweep says "AQ
moves bits from flat areas to busy ones." x264 does the opposite: `x264_ratecontrol_mb_qp` computes
`qp += qp_offset` with `qp_offset = strength * (log2(energy) - 14.427)`, so a high-energy (busy)
macroblock gets a **higher** QP and fewer bits, and flat areas get more. That is the texture-masking
direction. The sweep's conclusion ("expect AQ to widen the PSNR gap") still holds, because at a fixed
rate the MSE-optimal allocation is the uniform one and any deviation in either direction costs PSNR,
but the stated reason is wrong and would mislead anyone implementing it.

**(b) x264's variance AQ is not luma-only, so the sweep's "do not do x264-style luma-only AQ" row is
mislabelled (S2).** `ac_energy_mb` sums `ac_energy_plane` over luma **and** the interleaved chroma
plane for 4:2:0. The luma-only tool that Prangnell's papers beat is HEVC HM's `AdaptiveQP`, which
Prangnell states explicitly ("This technique takes into account only the variance of luma samples").
The sweep's section 3 reads `ac_energy_mb` correctly and its section 13.1 then contradicts it. The
engineering consequence is mild and in the sweep's favour: an all-plane variance AQ is what both
Prangnell and x264 already do, so there is no "x264 anchor" to avoid.

**(c) The chroma distortion weight is the square of the step ratio, not the step ratio (S3).**
`x264_chroma_lambda2_offset_tab` reads 16, 20, 25, 32, ... 256 at index 12, 512 at index 15, 1024 at
index 18: it is `16 * 2^(i/3)`, doubling every **three** entries. With `index = qp - qp_c + 12` the
weight is `256 * 2^((qp - qp_c)/3)`, not `2^((qp - qp_c)/6)` as the sweep states. That is correct for
an SSD-domain lambda (`lambda2` itself scales as `2^((qp-12)/3)`), and it means the chroma term is
weighted up twice as fast in the exponent as the sweep claims. Anyone porting the weight from the
sweep's formula would under-weight chroma by a square root.

**(d) Lever 1 of the sweep's speed table is largely already implemented in our encoder (local code).**
The sweep proposes "skip the levels of zero blocks in CAVLC, using the per-block non-zero counts we
already compute", citing Shalasere's -17.5 to -20.7 percent. Our `src/h264_cavlc.cpp` already skips a
whole macroblock on `cbpLuma == 0 && cbpChroma == 0` (line 249) and skips each 8x8 group whose cbp bit
is clear (line 332: `if ((cbpLuma & (1u << (blk >> 2))) == 0) continue;`). Shalasere's gain came from
no longer **scanning the levels buffer to answer "is this block zero"**, work our encoder never does
because `cs_mb.hlsl` hands us `cbpLuma`, `cbpChroma`, `nnzLuma[4]`, `nnzChroma[2]` and `nnzDc` in
`MbInfo`. What is left of the lever is small and real: inside a coded 8x8 group we still read all 16
(or 15) coefficients of every 4x4 block even when `MbNnzLuma` for it is zero. Expect a fraction of
their number, not their number.

**(e) Lever 3 of the sweep's speed table is already answered, in our favour (local code).** The sweep
asks whether CAVLC scans the mapped staging pointer directly, citing Shalasere's 13.76 ms against
329.92 ms. It does not: `GpuEncoder::RetireFrame` does one bulk `memcpy(levels.data(), m.pData, ...)`
and one for `info`, then unmaps, and the entropy coder reads the `std::vector`. We are already on the
good side of that finding, and we already have Shalasere's "shadow copy plus cached staging"
architecture. The open headroom is only the **size** of that bulk copy (their section 20 lever, the
sweep's lever 2), which stands.

**(f) The "two orders of magnitude" CAVLC comparison rests on a number its own source disowns (S10).**
`docs/multicore_cavlc_design.md` introduces the 45.4 / 45.7 microseconds per macroblock figures as
measured "against an existing on-board build (pre-algorithmic-fix ... this driver build predates the
concurrent CAVLC fix, so these numbers are the 'before' baseline, not a claim about the 'after'
state)." The sweep quotes the numbers but not that sentence. Measured on the same silicon, Shalasere's
CAVLC is 11.19 ms over 14,400 macroblocks at 1440p, that is 0.78 microseconds per macroblock, and
0.55 after their section 20. Ours is 3.42 ms over 8,160 macroblocks, 0.42 microseconds. So we are
about 1.3 to 1.9 times faster than the current state of the art on this part, not 100 times. The
sweep's recommendation ("do not go multi-slice for speed") survives on the quality table alone, but
the comfort the comparison provides is not earned.

**(g) The two BC-250 repositories are not independent measurements (S9, S10).** The sweep treats 9.1
and 9.2 as two projects. The throughput block the sweep quotes from MTSistemi (640x480 267 fps, 720p
179, 1080p 100-134, 1440p 67-80) appears verbatim in Shalasere's README line 32, and MTSistemi's
README credits "Shalasere for SPS crop research, Table 8-10 chroma QP mapping, and CABAC residual
optimizations". They share lineage. Two sources quoting the same figure is one measurement.

**(h) The modern per-macroblock-QP bound is on MS-SSIM and a machine-vision task, not PSNR (S14).**
The sweep's table row 9 cites arXiv:2607.10478 to bound per-macroblock QP "near 15 % BD-rate" in a
list of T3-compatible (PSNR) levers. The paper's own numbers are "BD-rate reduction of up to 17.12%
for semantic segmentation and 15.30% for MS-SSIM". Neither axis is PSNR. By the sweep's own rule that
metric compatibility decides which levers can close T3, this citation does not belong in that column.
The Prangnell papers do, and they are enough: Prangnell 2016 states its BD-rate is taken "when the
reconstruction quality, as measured by the Peak Signal to Noise Ratio (PSNR) metric, is the same in
both techniques tested" - a sentence the sweep should have quoted, because it is the load-bearing one
for its own argument.

**(i) The psy-RD rejection needs one qualification, from the same paper (S6).** The content classes
where the study found psy-RD **improving** quality are "Animation, China, LoL, and Transformer", that
is animation, game footage and high-activity content, with the gain peaking at strength 0.6. Our
target content is game video. The paper's average is negative and its bitrate cost is real, so "do not
make psy-RD a default" is supported; "do not implement psy-RD" overstates what this source licenses
for our content class. The sweep also omits the abstract's own caveat that objective metrics' "false
alarm rates are moderately high" for predicting the direction of the psy-RD effect.

**(j) One citation is unresolvable as printed (S5).** The SSIM-RDO paper's DOI is
**10.1109/TCSVT.2011.2168269**, not 10.1109/TCSVT.2012.2187915. The sweep's DOI returns HTTP 404 at
both `doi.org` and the Crossref API. Everything else in that citation (authors, title, TCSVT vol. 22,
no. 4, pp. 516-529, April 2012) is right.

**(k) One factual detail in the psy-RD entry is not in the paper (S6).** "20 naive observers, one
removed as an outlier, 19 valid". The paper says "A total of 20 naive observers, including 12 males and
8 females aged between 20 and 40, participated"; the words "outlier", "19", and any subject rejection
do not occur anywhere in it. The number 19 is unsupported.

---

## 2. Per-source verification

### S1. Garrett-Glaser, macroblock-tree - VERIFIED (with notes)

Fetched: `https://huyunf.github.io/blogs/2017/12/06/x264_slice_type_decision/MBtree%20paper.pdf`,
12 pages, 13 references, read in full.

Citation real and correct, with one refinement: the byline is "**Jason** Garrett-Glaser, Department of
Computer Science, Harvey Mudd College". The sweep's "J. Garrett-Glaser (Fiona Glaser)" is the same
person and the parenthetical is an editorial note, not a quote. There is **no year and no venue** in
the document: it is not a peer-reviewed publication, and the sweep correctly gives none. It is
reference [5] of its own lineage in reverse (the Merritt/Vanam overview is reference [5] of this
paper, as the sweep says).

Quotes, all verbatim at the stated sections:

* Abstract: "This novel macroblock-tree approach provides PSNR improvements of up to 1.2db and SSIM
  improvements of up to 2.3db over existing fast ratecontrol algorithms at very low computational
  cost." OK.
* Section 5: "propagate_fraction. This is approximated by the formula 1 - intra_cost / inter_cost."
  OK (the source prints an en dash in the formula). "The total amount of information that depends on
  this macroblock is equal to (intra_cost + propagate_in)." OK.
  "Macroblock QP Delta = -strength * log2((intra_cost + propagate_cost) / intra_cost)" OK, as a
  display line. "where strength is an arbitrary factor derived from experimentation. Testing suggests
  that 2 is a near-optimal value for most videos." OK.
* Section 5, the VAQ paragraph: OK, verbatim and complete.
* Section 9: "The cost of macroblock-tree in our implementation is approximately 28 clock cycles per
  macroblock per lookahead frame. Even with a lookahead of size 50 frames, this is less than half the
  cost of a single rate-distortion mode analysis in x264" OK.
* Section 10: the reverse-propagation sentence OK.

Numbers and conditions: r1924, git `08d04a4d30b452faed3b763528611737d994b30b`, `--preset slow`,
one-pass constant quality, CRF 1 to 51, 50-frame lookahead, Viterbi adaptive B placement,
`--tune psnr --psnr` / `--tune ssim --ssim`, strengths 1/2/3 as `--qcomp 0.8/0.6/0.4`, baselines
`--no-mbtree` and `--no-mbtree --qcomp 1`, all inputs forced to 25 fps - all OK. Performance: 1.866 GHz
Core i7, x86_64 Gentoo, gcc 4.6, foreman CIF, `--threads 1`, 29.770 to 30.293 fps (+1.80 percent) with
B frames, 29.363 to 28.705 fps (-2.20 percent) without, rate matched to 456.99/457.16 and
456.96/456.84 kbps - all OK; the sweep's "within 0.04 percent" is 0.037 and 0.026 percent, fair.

Two notes, neither fatal:

1. The sweep narrows the baseline: "the paper's up to 1.2 dB PSNR, up to 2.3 dB SSIM is against x264's
   own `--qcomp`-based frame level control, not against a flat QP". The abstract says "over existing
   fast ratecontrol algorithms" and the conclusion says the gain is "both when compared to the naive
   constant quantizer algorithm and qcomp". Section 8's per-sequence values live **only in figures**
   (pages 7, 9, 10 extract no text), so no individual number in that section is checkable and the
   larger of the two baselines cannot be identified. Treat the 1.2 / 2.3 dB as abstract-level claims.
2. The paper adds, about the reverse variant, "This has also been implemented in x264, though a full
   analysis is outside the scope of this paper." The sweep omits that. I checked whether it is
   reachable today: in current x264 `common/base.c`, `--tune zerolatency` and the `ultrafast` and
   `superfast` presets all set `rc.b_mb_tree = 0` together with `rc.i_lookahead = 0`. So the
   low-latency variant is not a configuration one can switch on in x264 now; there is no reference
   implementation to copy. This strengthens the sweep's "not the first lever" verdict.

### S2. x264 `encoder/ratecontrol.c`, `common/base.c` (variance AQ) - CORRECTED

Fetched from `https://raw.githubusercontent.com/mirror/x264/master/`. GPL-2.0-or-later, read only.

Quotes verbatim: the `/* constants chosen to result in approximately the same overall bitrate as
without AQ. * FIXME: while they're written in 5 significant digits, they're only tuned to 2. */`
comment OK; `strength = h->param.rc.f_aq_strength * 1.0397f;` OK;
`qp_adj = strength * (x264_log2( X264_MAX(energy, 1) ) - (14.427f + 2*(BIT_DEPTH-8)));` OK;
the three autovariance lines OK individually, though the sweep prints them in a different order from
the source (the `strength` and `avg_adj` assignments precede the per-macroblock loop). The function is
`x264_adaptive_quant_frame` OK. `ac_energy_mb` summing luma plus the interleaved chroma plane for
4:2:0 OK.

Defaults in `common/base.c` all OK: `rc.i_aq_mode = X264_AQ_VARIANCE` (415), `f_aq_strength = 1.0`
(416), `i_lookahead = 40` (417), `f_qcompress = 0.6` (423), `b_mb_tree = 1` (427),
`analyse.f_psy_rd = 1.0` (440), `i_deblocking_filter_alphac0 = 0` and `_beta = 0` (396, 397),
`i_luma_deadzone[0] = 21` and `[1] = 11` (456, 457). Also `analyse.b_psy = 1` (441), which the
sweep's section 4 needs and does not state.

Three corrections:

1. **Direction inverted.** See item (a) above. `x264_ratecontrol_mb_qp` is
   `qp = h->rc->qpm; ... qp += qp_offset;` so a positive offset coarsens. High variance gives a
   positive offset. Bits move from busy to flat, not from flat to busy.
2. `ac_energy_var` returns `ssd - ((uint64_t)sum * sum >> shift)` with `shift = 8` for a 16x16 block,
   that is `N * variance` (the block's total AC energy), not "the plain population variance" as the
   sweep says. It matters only for anyone reproducing the 14.427 pivot: the pivot is on total energy,
   `2^14.427 = 22024`, which the sweep's "2^14.427 = 22000" states correctly.
3. The sweep's section 13.1 "not luma-only variance AQ in the x264 style" mislabels this source. See
   item (b).

### S3. x264 `tables.c`, `analyse.c`, `set.c`, `quant.c`, `rdo.c`, `me.c` - CORRECTED

All quotes checked line by line.

Verified exactly:

* `x264_lambda_tab` is `round(2^((qp-12)/6))`: table reads 1 at 12, 2 at 18, 4 at 24, 5 at 26, 8 at
  30, 16 at 36, 32 at 42 - every value the sweep lists is right.
* `x264_lambda2_tab` is `round(0.9 * 2^((qp-12)/3) * 256)`: 230 at qp 12, 5851 at qp 26 - right.
* `x264_analyse_init_costs`: `logs[0] = 0.718f;` and `logs[i] = log2f( i+1 ) * 2.0f + 1.718f;` OK;
  `init_costs`: `h->cost_mv[qp][i] = X264_MIN( (int)(lambda * logs[i] + .5f), UINT16_MAX );` OK; the
  comment "factor of 4 from qpel, 2 from sign, and 2 because mv can be opposite from mvp" OK.
* `encoder/me.c`: `#define BITS_MVD( mx, my ) (p_cost_mvx[(mx)*4] + p_cost_mvy[(my)*4])` OK, and the
  key claim is confirmed at line 211: `const uint16_t *p_cost_mvx = m->p_cost_mv - m->mvp[0];` - the
  table is rebased on the predictor, so the index is the coded difference. Also at 869 and 1276. The
  sweep's central finding (4.1) is correct and now independently confirmed.
* `common/set.c`: `int deadzone[4] = { 32 - h->param.analyse.i_luma_deadzone[1], 32 -
  h->param.analyse.i_luma_deadzone[0], 32 - 11, 32 - 21 };` OK, and
  `h->quant4_bias[i_list][q][i] = X264_MIN( DIV(deadzone[i_list]<<10, j), (1<<15)/j );` with
  `quant4_bias0[...] = (1<<15)/j` OK. The arithmetic checks out: `bias0` is the round-to-nearest 0.5
  reference, `bias/bias0 = deadzone/32`, so the offset is `deadzone/64` of a step; with the defaults
  the lists get 21, 11, 21, 11, that is 0.328 intra and 0.172 inter, luma and chroma alike. The
  sweep's section 4.3 conclusion - **our 1/3 and 1/6 already match x264, the deadzone is a dead
  lever** - is confirmed.
* `encoder/rdo.c`: both psy comment lines OK; the `satd = abs(...)` line OK; `int64_t tmp =
  ((int64_t)satd * h->mb.i_psy_rd * h->mb.i_psy_rd_lambda + 128) >> 8;` OK; the `return
  h->pixf.ssd[size](...) + satd;` OK; `i_ssd += ((uint64_t)chroma_ssd *
  h->mb.i_chroma_lambda2_offset + 128) >> 8;` OK in `ssd_mb` (and the same weighting in the 4x4 and
  8x8 paths).
* `encoder/analyse.c` `mb_analyse_init_qp`: the comment "Adjusting chroma lambda based on QP offset
  hurts PSNR but improves visual quality." OK, and the two following lines OK.
* `encoder/rdo.c` trellis entry points `trellis_cabac_4x4` / `_8x8` exist (lines 717, 722), as the
  sweep says, and the dispatcher is `quant_trellis_cabac`, confirming the sweep's caveat that x264's
  trellis is written against CABAC rate estimates.

Corrections:

1. **`x264_chroma_lambda2_offset_tab` doubles every three entries, not every six.** Item (c). The
   weight is `256 * 2^((qp - qp_c)/3)`, the square of the step ratio, which is the right shape for an
   SSD-domain term. The sweep's "weighted up by exactly the quantiser step ratio" is wrong by a
   square.
2. **One arithmetic slip in the 4.1 worked table.** For a one-full-sample vector against a zero
   predictor the index is 4 quarter-samples, so the penalty is `2 * 5 * (2*log2(5) + 1.718) = 63.6`,
   not 55. The ratio column should read 24/63.6 = 0.38x, not 0.44x. The other three rows are right:
   zero MVD `2*5*0.718 = 7.2`; (16,16) from a zero predictor `2*5*(2*log2(65)+1.718) = 137.6`; the
   pan row 384 against 7.2 is 53x, the sweep's "55x". The finding is unaffected.
3. `QUANT_ONE` is quoted abridged without ellipsis. The macro is
   `{ if( (coef) > 0 ) (coef) = ((f) + (uint32_t)(coef)) * (mf) >> 16; else (coef) = -(int32_t)(((f) +
   (uint32_t)(-coef)) * (mf) >> 16); nz |= (coef); }`. The arithmetic the sweep shows is the positive
   branch, verbatim; the sign handling and `nz` accumulation are dropped silently.
4. In 4.1 the sweep writes "x264 can follow a 200 pixel per frame pan, we cannot follow 22." The 200
   is rhetorical and has no source; what the sources support is that x264's radius is measured around
   a predictor set and ours around (0,0).

### S4. Merritt and Vanam, "x264: A High Performance H.264/AVC Encoder" - VERIFIED

Fetched `http://akuvian.org/src/x264/overview_x264_v8_5.pdf`, 13 pages, read in full. Header reads
"In Preparation"; authors "Loren Merritt and Rahul Vanam*", "*Dept. of Electrical Engineering,
University of Washington" - citation correct, and correctly marked as unpublished.

All quotes verbatim at the stated sections: the 50x / 5 percent sentence (section 1); `SATD0 = SATD +
lambda * bits` as equation (3) with "where SATD is the sum of absolute Hadamard-transformed
difference" (section 2.2 step 10); the adaptive-radius step 6 text and "The default search range is
16. This may decrease as low as 12 if SAD is small and the predictors are similar, and may increase
as high as 24 if SAD is large and the predictors differ much."; section 2.3's hybrid sentence, the
I16x16-first sentence, and the P16x8/P8x16 estimate; section 2.4.1's uniform-deadzone paragraph; the
trellis result sentences.

Conditions verified: 19 CIF sequences, five reference frames in both encoders, equal QP, QP 18 to 36
in steps of 3, Trellis-2 for the JM comparison, Trellis-0 as the reference for the trellis comparison
(Fig. 2(d)); x264 0.47.534 against JM 10.2; "x264 performs better than JM for PSNR greater than 38 dB
and for PSNR below 38 dB there is slight increase in bitrate up to 5%" - the sweep's direction is
right.

Two small things, both in the sweep's favour and worth carrying:

* The sweep's deadzone quote starts mid-sentence and so drops the rate condition: the source says
  "(**At low rates**, the vast majority of coefficients are zeros or ones, so the dependence between
  coefficients matters more ...)", inside a parenthetical about JM's adaptive deadzone. The sweep's
  framing ("why that assumption is wrong") is a fair reading of the trellis motivation, but the quote
  should carry "At low rates".
* The source says the `bits` in SATD0 is "the number of bits needed to encode the motion vectors using
  Context-based Adaptive Variable Length Coding (CAVLC)". The sweep does not mention this. It matters
  for us: the mode-decision bit term in the comparison target is already a CAVLC bit count, so our
  clause 9.2 tables in `src/h264_tables.cpp` are the right rate model and no CABAC model is needed for
  levers 1, 2, 4, 6 and 7.

### S5. Wang, Rehman, Wang, Ma, Gao, SSIM-motivated RDO - CORRECTED

Fetched `https://ece.uwaterloo.ca/~z70wang/publications/TCSVT_SSIM_RDO.pdf`, 14 pages, read in full.
Title, authors, venue (IEEE TCSVT vol. 22, no. 4, April 2012, first page 516) all correct.

**DOI wrong.** The sweep prints 10.1109/TCSVT.2012.2187915; `https://doi.org/10.1109/TCSVT.2012.2187915`
returns 404 and the Crossref API returns "Resource not found". A Crossref bibliographic query returns
**10.1109/tcsvt.2011.2168269** for this exact title, with volume 22, issue 4, pages 516-529, published
2012-04; `https://doi.org/10.1109/TCSVT.2011.2168269` redirects to
`ieeexplore.ieee.org/document/6020768/`. Use that DOI.

Quotes verbatim: the IPP/IBP rate-reduction paragraph (section VII-B); the PSNR caveat ("However, on
average PSNR decreases because our optimization objective is SSIM rather than PSNR."); the Fig. 10
sentence ("since our proposed RDO scheme is based on SSIM index optimization, higher SSIM and lower
PSNR are achieved"); "On average the computation overhead is 6.3%" (Table VI discussion); the Bridge
35 percent sentence. The coding conditions quote is verbatim and complete. The SSIM sliding window of
8x8 is stated as the sweep says, and the curve differences do use the Bjontegaard method (reference
[50] is "Calculation of average PSNR difference between RD curves").

One correction: the sweep says "Against **two prior** SSIM-based RDO schemes the paper reports 12.39
percent versus 9.79 percent at QP1 and 16.28 percent versus 11.58 percent at QP2." The source attaches
those four numbers to **one** named method: "when compared to Huang et al.'s method, on average the
proposed scheme achieves better rate reduction of 12.39% versus 9.79% for QP1 and 16.28% versus 11.58%
for QP2 while maintaining the same SSIM values for IPP GoP structure." Tables VIII and IX do contain
several prior schemes; the quoted pair is Huang et al.

The sweep's use of this source - as the clearest evidence that the perceptual family is the wrong
family for T3 as written - is exactly what the source says, twice, in its own words. Not an overreach.
The overhead figure's conditions (100 frames IPPP, Intel 2.83 GHz Core, 4 GB) should be carried with
it; the sweep gives the number without them.

### S6. Duanmu, Zeng, Wang, Eisapour, perceptual evaluation of psy-RD - CORRECTED

Fetched `https://ece.uwaterloo.ca/~z70wang/publications/HVEI17_PsyRD.pdf`, 6 pages, read in full.
Authors "Zhengfang Duanmu, Kai Zeng, Zhou Wang and Mahzar Eisapour", Dept. of Electrical and Computer
Engineering, University of Waterloo; the page footer reads "IS&T Electronic Imaging - Human Vision and
Electronic Imaging, Burlingame, CA, Jan.-Feb. 2017". Citation correct.

Quotes verbatim: the abstract sentences; the three numbered observations (strength/negative impact,
bitrate increase, content dependence with the peak at 0.6); the objective-metric sentence "Several
full-reference IQA models (SSIM, MSSSIM, SSIMplus, VIF, and VQM) performs reasonably and almost
equally well, although their computational cost is drastically different, ranked from SSIMplus, SSIM,
MSSSIM, VIF to VQM, from the lowest to the highest" - the sweep's "ranked by cost" reading is right.
The no-reference sentence is "state-of-the-art no-reference approach does not provide adequate
predictions of the Psy-RD optimized videos"; the sweep's attribution to BRISQUE is fair, since
BRISQUE is the only no-reference model in the set of 9+1.

Table 1 verified value for value: all twelve printed MOS gains are negative, from -0.2200 to -4.6433.
The sweep's range is exact. Conditions verified: 15 sources, 1280x720, 10 s, 25 fps, four bitrates
(250, 500, 950, 1300 kbps), four strengths (0, 0.6, 1.0, 2.0), 16 test sequences per source, so 240
sequences; 0-100 continuous scale; ITU-T BT.500 calibration on a 2560x1600 LCD; randomised order.

Three corrections:

1. **"one removed as an outlier, 19 valid" is not in the paper.** Item (k). The word "outlier" does
   not appear; no subject screening is described. Drop the sentence.
2. **"the delivered bitrate rising with strength at every target" is false at the fourth target.**
   Table 2's 1300 kbps row reads 1319.50 (strength 0), 1306.73, 1314.50, 1317.09 (strength 2): the
   highest value is at strength 0. The sweep lists the three rows that do rise and then generalises to
   "every target". The paper's own sentence ("The larger of the Psy-RD strength, the larger the bitrate
   of the encoded video") has the same defect; a checker should note that the source overstates its
   own table, and not repeat it.
3. **The content-dependence qualification matters for us.** Item (i). The clips psy-RD helped are
   Animation, China, LoL and Transformer; the ones it hurt most are low-complexity (Baby,
   DaNaoTianGong, Skii). The sweep quotes the content-dependence observation and then recommends a
   flat "do not implement psy-RD", without noting that our content class is the one where this very
   study found a gain. The abstract's "the false alarm rates are moderately high" is also omitted.

The sweep's engineering conclusion (do not build psy-RD now, do not build the SATD-complexity
machinery for it, and never as a default) remains well supported: the average is negative, the bitrate
cost is measured, and T3 is a PSNR target.

### S7. Prangnell, spatiotemporal adaptive quantization (arXiv:2005.07925) - VERIFIED

Fetched the arXiv PDF (5 pages) and the abstract page. Author Lee Prangnell, submitted 16 May 2020,
no journal reference. Abstract quotes verbatim, including "based only on the variance of samples in a
luma CB" and "Our method achieves a maximum BD-Rate reduction of 23.1% (Y), 26.7% (Cr) and 25.2% (Cb).
Furthermore, a maximum encoding time reduction of 4.4% is achieved." The introduction quote ("This
technique takes into account only the variance of luma samples; in addition, it does not account for
motion information in a CU, thus leaving room for improvement.") verbatim.

Conditions verified: HM 16.7, QPs 22/27/32/37 per the common test conditions, Random Access, Main
4:4:4 / 4:2:2 / 4:2:0 and 4:0:0 profiles, anchor = the AdaptiveQP tool, sequences FourPeople (720p),
KristenAndSara (720p), ParkScene (1080p), Traffic (1600p), four chroma versions each.

The sweep's 4:2:0 table is correct in every cell, which I checked against Table 1:
FourPeople -13.2 / -15.6 / -16.9, ET -2.0, DT +0.2; KristenAndSara -22.4 / -27.9 / -24.6, -0.8, -0.4;
ParkScene -6.5 / -15.2 / -16.0, -0.9, +3.0; Traffic -4.8 / -13.4 / -17.9, -0.8, +0.2. The 4:0:0 Y
column is -6.9, -4.2, -3.0, -3.4, so the sweep's "-6.9 to -3.0 percent Y" for the non-chroma part of
the method is right, and so is its statement that the paper does not ablate the three contributions
individually.

Two notes: the abstract's maxima come from the **4:2:2** KristenAndSara row, not 4:2:0 (the sweep does
not claim otherwise); and the abstract labels the two chroma maxima "26.7% (Cr) and 25.2% (Cb)" while
Table 1 has -26.7 under Cb and -25.2 under Cr - an inconsistency in the source, carried into the
sweep's quote because the quote is verbatim. The affiliation the sweep gives ("Department of Computer
Science, University of Warwick") is not in this PDF or on the arXiv page; it is correct for this author
from the 2016 paper, but it is unverified for this one.

### S8. Prangnell, Hernandez-Cabronero, Sanchez, cross-colour-channel AQ (arXiv:1612.07893) - VERIFIED

Fetched the arXiv PDF (10 pages, v4 of 12 Feb 2018) and abstract page. Authors and University of
Warwick affiliation confirmed on page 1. Abstract quote verbatim, including the maxima "15.9% (Y),
13.1% (Cr) and 16.1% (Cb) in addition to a maximum decoding time reduction of 11.0%". The HVS / chroma
QP ceiling passage verbatim: "Note that the HVS is typically more sensitive to gradations to the data
in the luma channel. Moreover, the data in the chroma channels is susceptible to severe artifacts
caused by very high levels of quantization. Therefore, in the HEVC standard the maximum QP permitted
for chroma data is QP = 39 (chroma QP offset) for YCbCr 4:2:0 chroma subsampled input video data."

The two numbers the sweep gives are verbatim from the body: 4:2:0 All Intra, 8-bit KristenAndSara,
Main profile, "-14.3% (Y), 12.3% (Cb) and 12.5% (Cr)"; Random Access, same sequence, "15.5% (Y), 12.8%
(Cb) and 11.8% (Cr)". Conditions (HM 16, All Intra and Random Access, 4:4:4 / 4:2:2 / 4:2:0 JCT-VC
sequences, AdaptiveQP anchor, plus a subjective paired comparison) all correct.

Addition in the sweep's favour, which it should have quoted: the paper defines its BD-rate "when the
reconstruction quality, as measured by the Peak Signal to Noise Ratio (PSNR) metric, is the same in
both techniques tested". That single sentence is what makes S7 and S8 the only AQ sources in the sweep
whose axis is compatible with T3, which is the sweep's own argument.

### S9. Shalasere/bc250-vulkan-encode-stopgap - VERIFIED

Fetched `README.md` and `docs/DEVLOG.md` (325 KB) from the repository's `main`. Licence badges:
driver GPL-3.0, audio module GPL-2.0 - the sweep's GPL-3.0 and its read-only, prior-art framing are
right.

Every quote verbatim, every number exact:

* README: the CABAC bullet ("10-13% smaller output than CAVLC at matched QP, ~28% more CPU, still well
  above real-time. Scope: I_16x16 intra / P_L0_16x16 inter only."); "GPU shaders are <1.5% of frame
  time; the remaining bottlenecks are CPU/memory-side."; the `qp_min=12` bullet including "taking the
  floor to 8 spent 14% more bits for -22% encode throughput and no visible quality change"; the
  contention sentence "a heavy compute-bound title can cost this encoder up to ~45x (measured 66.2 ->
  1.48 fps at 1440p under a synthetic worst-case load generator)"; "Output is not bit-reproducible on
  moving content"; the two conformance gaps, luma-only in-loop deblocking and source-neighbour I-slice
  intra, "together ~3.7 dB of per-GOP drift".
* DEVLOG section 19.1 table: CAVLC 5.44 / 11.19 ms, shadow_copy 3.03 / 2.92, GPU 4.08 / 4.93, wall
  13.69 ms (71.9 fps) / 19.26 ms (51.3 fps), 300 frames, mean over P frames, runtime-gated perf stats
  - exact. The diagnosis sentence and the zero-density figures ("89.8-95.8% of all blocks are entirely
  zero (95.6-99.4% have zero AC)") exact.
* Section 19.2: the HOST_CACHED sentence verbatim, and the 2x2 (13.76 ms cached with shadow copy
  against 329.92 ms uncached) exact; the sweep does not mention the other two cells (14.57 and
  862.26), which do not change its point.
* Section 19.5: CAVLC -17.5 to -20.7 percent, wall -6.7 to -11.2 percent, 1080p 8.82 to 8.15 and
  12.84 to 11.53 ms, PSNR ranges overlapping - exact.
* Section 20: staging 44.2 to 2.8 MB, GPU-to-host copy 22.1 to 1.4 MB, +30 percent at 1440p (19.532 to
  14.998 and 14.100 to 10.835 ms), byte-identical across five configurations - exact. The mechanism
  quote, including "**The larger share is CAVLC running 37% / 30% faster despite not being touched by
  this diff at all.**" and the cache-residency inference - verbatim.

One omission to flag: the DEVLOG labels the cache-residency mechanism as unproven - "it is an
inference from the stage budget, not something separately proven with cache counters". The sweep
presents "expect the second-order effect to dominate ... 30 percent is about 1 ms per 1080p picture"
without that caveat. The measured deltas are real; the explanation is the project's hypothesis. For
us the practical consequence is unchanged, because the same change (ship fewer bytes over the bus) is
the lever either way.

Also worth carrying from this source and used correctly by the sweep: every throughput figure there is
on an otherwise idle GPU, and the measured contention penalty is up to 45x. Any claim about our
encoder running under The Witcher 3 inherits that.

### S10. MTSistemi/bc250-vaapi - CORRECTED

Fetched `README.md`, `docs/multicore_cavlc_design.md`, `docs/rate_control_audit.md` from `main`.
Driver licence GPL-3.0 confirmed. Badges point at `simpmix/bc250-encoding-decoding-fix`, consistent with the
sweep's "fork lineage".

Quotes verbatim and numbers exact:

* README: the H.264 throughput list and "Game Streaming Overhead: Only ~4.5% total GPU impact during
  active 60 FPS gaming"; "Chroma Fidelity: Bit-exact non-linear Table 8-10 QP mapping eliminates the
  standard chroma PSNR deficit." The sweep correctly scopes the Table 8-10 claim to HEVC, where it
  sits in the README.
* `multicore_cavlc_design.md` section 3: 640x480 1200 MBs, GPU 0.459 ms, CAVLC 54.534 ms, wall 55.465
  ms, 98.3 percent; 720p 3600 MBs, 0.993 / 164.357 / 165.865 ms, 99.1 percent; 45.4 and 45.7
  microseconds per macroblock; 60 frames with `tools/perf_test.sh` - all exact.
* The Amdahl table (p = 0.98: 3.8x at N=4, 7.0x at N=8, 12.3x at N=16; p = 0.60: 2.3x at N=16) exact.
* The slice-count quality table (37.66 / 34.00 / 29.27 / 29.67 / 28.91 dB; SSIM 0.9918 / 0.9886 /
  0.9777 / 0.9783 / 0.9729; deltas -3.66, -8.39, -8.00, -8.75 dB) exact, and the conditions (640x480,
  25 fps, 2 s / 50 frames, default 4 Mbps, same driver build for every point) exact. Both caveat
  quotes verbatim, including "this document is not asserting that tradeoff is automatically worth it,
  only reporting the real curve."
* `rate_control_audit.md`: the "~+-6-8 QP steps around a hardcoded QP of 26" sentence, the `base_qp`
  crux paragraph, and "measured output bitrate for the same content varies by <5% across a 20x spread
  of requested bitrates, while switching content complexity at a fixed requested bitrate swings output
  bitrate by >6x" - all verbatim. The `target_percentage` finding is as the sweep describes (ffmpeg's
  default VBR sends `bits_per_second = 2X` with `target_percentage = 50`, and the driver uses the
  doubled figure).

Two corrections, both in the sweep's reading rather than its quoting:

1. The 45 microseconds per macroblock figures are explicitly a pre-fix baseline. Item (f). The
   derived claim that our CAVLC is "about two orders of magnitude faster per macroblock than theirs"
   should be retired and replaced with the Shalasere comparison: 0.42 against 0.78 microseconds per
   macroblock, about 1.3 to 1.9x, same silicon, both CPU-side CAVLC.
2. This source is not independent of S9. Item (g).

The sweep's reading of the audit as a checklist our `UpdateRateControl` already passes is correct and
I confirmed it in our code: `want = 6.0 * log2(m_complexity / targetBits)` with `m_complexity` tracked
from delivered bits and QP, so our fixed point does move with content and target. The two residual
risks it names (the `kRcMaxStep = 4` convergence bound, and the `ICodecAPI` mean/max bitrate
interpretation) are real and unchecked.

### S11. Wang, Bovik, Sheikh, Simoncelli, SSIM - VERIFIED

Fetched `https://ece.uwaterloo.ca/~z70wang/publications/ssim.pdf`, 14 pages. Header "IEEE
TRANSACTIONS ON IMAGE PROCESSING, VOL. 13, NO. 4, APRIL 2004", authors as cited. Crossref confirms
DOI 10.1109/TIP.2003.819861, pages 600-612, April 2004 - citation fully correct.

Fig. 2 caption verbatim: "Comparison of "Boat" images with different types of distortions, all with
MSE = 210." Equation numbering checks out: (6) is the luminance comparison, (7) is `C1 = (K1 L)^2`,
(9) is the contrast comparison with `C2 = (K2 L)^2`. The sweep's one-line summary of the paper's
argument is fair and its use (SSIM as the cheapest credible perceptual axis, already in libvmaf) is
not an overreach.

### S12. Netflix/vmaf README - VERIFIED

Fetched from `master`. All five quoted claims verbatim: the stand-alone `libvmaf` sentence; "Also
included in `libvmaf` are implementations of several other metrics: PSNR, PSNR-HVS, SSIM, MS-SSIM and
CIEDE2000."; the v1 model release dated 2026-06; the `libvmaf v2.0.0` news entry with the fixed-point
AVX2/AVX-512 2x speed-up; the 2020-02-27 licence change from Apache 2.0 to BSD+Patent. The NEG mode is
described in the overview paragraph as "a codec evaluation-friendly NEG mode" and in the 2020-7-13
entry as the answer to VMAF's behaviour "in the presence of image enhancement operations, its impact
on codec evaluation" - so the sweep's warning to use NEG for an encoder comparison is exactly what the
source says.

### S13. FAU-LMS/bjontegaard README - VERIFIED

Fetched from `main`. BSD-3-Clause confirmed. All quotes verbatim: the CSI description, the "very misleading
results ... also been experienced during the standardization of HEVC" passage, PCHIP in the JCT-VC CTC
sheet, Akima "returns more accurate and stable results", and "the implementation of PCHIP returns the
same value as the Excel-Implementation ... with an accuracy of at least 10 decimal positions". The
sweep's treatment of VCEG-M33 as named but not opened is honest, and the Austin, Texas, 2001
attribution is the standard one.

The sweep's recommendation built on this source (sweep four quantisers per case and state T3 as a
BD-rate as well as a dB figure, before measuring any lever) is the single most useful thing in the
document and is not disputed here: our present comparison is one point per case at a 4 to 7 percent
bitrate mismatch, which cannot support a 1.0 dB / 1.5 dB acceptance criterion.

### S14. Xu and Bajic, differentiable proxy for H.264 AQ (arXiv:2607.10478) - CORRECTED

Fetched the arXiv PDF (7 pages) and abstract page. Authors Qihan Xu, Ivan V. Bajic, submitted 11 Jul
2026. The quoted sentence is verbatim from the abstract.

Correction: the sweep uses this as a bound for a **PSNR**-axis lever ("per macroblock QP, chosen well,
is worth something in the 15 percent BD-rate range on H.264 intra against a fixed QP", and in table
13.1 as support for row 9). The paper's two figures are BD-rate for semantic segmentation (17.12
percent) and for MS-SSIM (15.30 percent). Neither is PSNR. Item (h). Either restate the row as
"perceptual and task BD-rate" or drop this citation and lean on S7/S8, which are PSNR-axis.

The sweep's dismissal of the mechanism (a learned differentiable proxy is not usable in a real-time
MFT) is sound.

### S15. Revisiting pre-analysis information based rate control in x265 (arXiv:2109.12294) - VERIFIED

Fetched the arXiv PDF (4 pages, v3 of 13 May 2022, cs.MM) and abstract page. Title as cited. Sole
author **Hewei Liu**; the sweep gives no author, which is an omission rather than an error.

The quoted sentence appears verbatim in the PDF body: "the complete proposed method can achieve 10.3%
BD-rate gain with only 0.22% bitrate error which is superior than the anchors both in the rate control
accuracy and the R-D performance." Two notes: the sweep's bracketed completion "[BD-rate and bitrate
error]" is not what the sentence says (it is "the rate control accuracy and the R-D performance"); and
the arXiv metadata abstract renders the error as `0.22\textperthousand`, i.e. 0.22 per mille, while
the PDF prints "0.22%". The PDF is what the sweep quotes, so the quote is faithful; the smaller unit
is probably the intended one, and either way the figure supports the same point.

The sweep's use (confirming that lookahead-driven control is the state of practice and that ~10
percent BD-rate is the order of the prize) is fair, and this source's BD-rate is on the conventional
PSNR axis, which the sweep could have said in its favour.

### S16. Section 12, the three sources the sweep could not open - VERIFIED as fair

I repeated each attempt.

* **List, Joch, Lainema, Bjontegaard, Karczewicz, "Adaptive deblocking filter"**: Crossref confirms
  IEEE TCSVT 13(7):614-619, July 2003, DOI 10.1109/TCSVT.2003.815175, those five authors. Semantic
  Scholar's green open-access URL (`cs.sfu.ca/CourseCentral/820/...`) returns HTTP 500 for me too.
  `https://iphome.hhi.de/wiegand/assets/pdfs/...` returns HTTP 200 with a **37208-byte HTML page**,
  exactly the size the sweep reports. The body was not read, and the sweep is right not to quote the
  widely repeated "9% bit-rate saving" figure as verified.
* **Wiegand, Schwarz, Joch, Kossentini, Sullivan, "Rate-constrained coder control and comparison of
  video coding standards"**: Crossref confirms TCSVT 13(7):688-703, July 2003, DOI
  10.1109/TCSVT.2003.815168, those five authors. The Semantic Scholar open-access host `ip.hhi.de`
  does not resolve; two further mirrors I tried return 404. Deriving the operating lambda from x264's
  own tables instead (0.9 rather than 0.85) is the right call for our purposes, and the sweep says so.
* **Momcilovic, Ilic, Roma, Sousa, "Dynamic Load Balancing for Real-Time Video Encoding on
  Heterogeneous CPU+GPU Systems"**: Crossref confirms IEEE TMM 16(1):108-121, January 2014, DOI
  10.1109/TMM.2013.2284892, those four authors. Not opened here either; the sweep is right to keep its
  speed-up figures out of the verified column.
* **Unpaywall**: `https://api.unpaywall.org/v2/<doi>?email=research@example.com` returns HTTP 422
  with exactly the message the sweep quotes ("Please use your own email address in API calls"). The
  workspace rule was honoured again: no real address was substituted. Crossref accepts
  `mailto=research@example.com` and was the metadata source here too.

So section 12 is accurate, including the byte count, and its refusals to quote unverified numbers are
the right behaviour.

---

## 3. Section 1 of the sweep: its description of our own encoder

Checked against `scratch\m15\video-encode\perf-wt\driver\umd\mft-h264` (read only, nothing modified).

Confirmed exactly: one 32-thread group per macroblock (`[numthreads(32,1,1)]` in `cs_me.hlsl`,
`cs_mb.hlsl`, `cs_deblock.hlsl`); three integer ME stages with steps 8, 2, 1 and sides 5, 5, 3,
starting at `cx = cy = 0` with no predictor, total reach +-21 full samples; the cost
`gCost[tid] = sad + gLambda * uint(abs(ix) + abs(iy))` and its quarter-sample form with `/ 4u`; the
snap-back at `gZeroSad <= gBestSad + gSkipBias`; P pictures all `P_L0_16x16` with `gMbCost` read back
from `MbInfo`; intra ranking by SAD with threads 0..3 on luma modes and 4..7 on chroma modes, minima
taken separately for luma and chroma; the quoted comment about intra macroblocks in P pictures,
verbatim including "onto the wavefront as well"; `Quant4x4`'s `f = (1u << qbits) / (intra ? 3u : 6u)`;
`cs_deblock.hlsl`'s `qPav` comment and `idxA` / `idxB` lines; `mb_qp_delta` written as `SE(0)` under
the comment "Rate control is per picture, so the quantiser never changes inside a slice";
`kRcBufferFrames = 16`, `kRcMaxStep = 4`, `want = 6.0 * log2(m_complexity / targetBits)`, `qpMin = 14`,
`qpMax = 46`, the IDR `qp - 3`; `gp.lambda = (1u + (qp / 8u)) * m_cfg.lambdaScale / 100u` with
`lambdaScale = 300` and `skipBiasScale = 0`; `chromaQpIndexOffset = 0` and `ChromaQpFromLuma`;
`kLevelsWordsPerMb = 204` and `kMbInfoWords = 16`, so 6.66 MB of levels plus 0.52 MB of macroblock
info per 1080p picture; `deblocking = false` by default; the deblocking offsets reaching the slice
header through `h264_syntax.cpp` lines 112-113 (`bw.SE(slice.alphaC0OffsetDiv2); bw.SE(slice.betaOffsetDiv2);`)
while nothing in `EncoderConfig` or `encoder.cpp` ever sets them, so they are stuck at 0 - the sweep's
cheapest-knob recommendation (13.3.3) is valid; `--compare` in `tests/mfthost.cpp` reporting
`PlanePsnr` only, with no SSIM and no VMAF, so 13.3.2 is valid; and the CABAC refusal in
`src/mft_h264.cpp` ("Refusing is the honest answer").

Two corrections and one addition:

1. **Groupshared in `cs_me.hlsl` is about 8.9 KB, not 9.8 KB.** Declared: `gSrcMb[256]`,
   `gCost[32]`, `gSad[32]`, `gCandX[32]`, `gCandY[32]`, four scalars, `gInt[23*23]`, `gBRaw[23*18]`,
   `gH[18*18]`, `gJ[18*18]`, `gPart[9*32]` - 9,068 bytes. The occupancy argument is unaffected (6
   groups per CU against a 64 KB LDS).
2. **`gPart` does not hold per-8x8 partition partials.** The sweep's lever 4 says "the 32 lanes
   already form per-8x8 partial sums in `gPart`". `gPart[c * 32u + tid]` holds, for each of nine
   sub-pel candidates, one lane's absolute-difference sum over **eight consecutive samples of one
   row** (`first = tid * 8u`, `sy = first >> 4u`, `sx = first & 15u`), and only in the sub-pel stages;
   the integer stages do not populate it at all. Per-partition costs would be new work in both stages,
   not a regrouping of something already there. The lever may still be right; its cost is
   understated.
3. Items (d) and (e) of section 1 above belong here too: `h264_cavlc.cpp` already skips zero
   macroblocks and zero 8x8 groups, and the readback is a bulk `memcpy` into a vector rather than a
   scattered scan of the mapped staging buffer.

Unsourced conversions worth marking as such: "10 to 13 percent of rate at matched QP converts to
roughly 0.5 to 0.8 dB at a typical rate-distortion slope" has no source and is at the optimistic end
of the usual rules of thumb. The defensible statement is the measured one: CABAC is lossless, so at
matched QP and matched decisions the reconstruction is bit-identical and the whole 10 to 13 percent is
rate. State it that way and let the BD-rate curve of 13.3.1 convert it.

The B19 numbers the sweep builds on are as recorded in `LAB-B19-RESULT.md`: 1080p 11.51 ms serial
(86.9/s), 4.55 ms pipelined (219.8/s), GPU busy 6.53 ms, CAVLC 3.42 ms; inbox 1.8x at 720p and 1.7x at
1080p; Y 43.75 against 46.06, Cb 40.56 against 45.50, Cr 39.16 against 45.16, with the inbox spending
4 to 7 percent more bits.

---

## 4. Primary sources the sweep missed, found while checking

1. **Prangnell 2016's PSNR definition of its BD-rate** (S8). One sentence that does the sweep's
   metric-compatibility argument better than anything else in the document. Quote it.
2. **The CAVLC bit count inside x264's SATD0** (S4). The comparison target's mode-decision rate term
   is already CAVLC, so levers 1, 2, 4, 6 and 7 need no CABAC rate model - only lever 8 (trellis)
   does. This removes a perceived blocker from five of the nine T3 levers.
3. **x264's `--tune zerolatency` turns mb-tree off** (`common/base.c` 675-683, with the `ultrafast`
   and `superfast` presets doing the same). The 2010 paper's reverse-propagation variant is not a
   reachable configuration in current x264, so there is no reference implementation to copy for a
   low-latency mb-tree. Reinforces the sweep's section 2 verdict.
4. **x264's `cost_i4x4_mode[i] = 3*lambda*(i!=8)` and `cost_table->ref[qp][i][j] = lambda *
   bs_size_te(i,j)`** (`encoder/analyse.c` `init_costs`). These are the missing half of the sweep's
   lever 6: x264 charges lambda times a real bit count not only for the vector but for the **mode
   symbol** and the reference index. Our `gRedCost` has no mode-bit term at all, which is cheap to add
   at the same time as SATD and is part of the same defect.
5. **Shalasere's own statement that the cache-residency explanation is an inference** (S9), and
   **MTSistemi's own statement that its 45 microseconds per macroblock is a pre-fix baseline** (S10).
   Both are caveats the sweep quotes around.
6. Not found, still open: no source read here measures what partitions below 16x16 or intra-in-P are
   worth on their own in H.264 at our operating point. The sweep's levers 4 and 5 remain the two
   named causes of the T3 gap with **no** external number attached to either. If one number is to be
   bought with lab time, that is where the ignorance is.

---

## 5. Bottom line

The sweep is sound work. Of its 16 sources, every citation names a real document that I opened
independently, and every quotation I checked appears in that document at the stated place, with two
exceptions of form (an abridged C macro, a quote starting mid-sentence) and none of substance. Its two
most useful technical findings - that our motion cost function is the wrong shape and measured from
the wrong origin, and that our quantiser deadzone already matches x264's defaults so the chroma gap is
not there - both survive independent checking against the primary source.

What must be fixed before the document is used: the inverted AQ direction (a), the chroma lambda
exponent (c), the DOI (j), the fabricated observer count (k), and the two speed levers that our own
code already implements (d, e). What must be softened: the "two orders of magnitude" CAVLC comparison
(f), the treatment of the two BC-250 repositories as independent (g), the PSNR-axis use of the
machine-vision bound (h), and the flat psy-RD rejection for game content (i).

What does not change: the ranking logic. T3 is a PSNR target; the perceptual family trades PSNR away
on purpose and three of the sources say so in their own words; the levers that raise PSNR at equal
rate are prediction and rate-distortion accuracy, led by the motion cost function and predictor
seeding, with CABAC as the one large gain that needs no GPU work and is measured on this exact
silicon with our exact mode set. And before any of it: four quantisers per case and a BD-rate, because
a single point at a 4 to 7 percent bitrate mismatch cannot answer a 1.0 dB question.
