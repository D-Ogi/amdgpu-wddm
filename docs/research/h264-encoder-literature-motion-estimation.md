# Literature sweep: GPU motion estimation for H.264, applied to our MFT encoder

> This sweep has an independent re-check, [`h264-encoder-literature-motion-estimation-check.md`](h264-encoder-literature-motion-estimation-check.md). The check opened every source again
> on its own and recorded a verdict for each. Some verdicts are VERIFIED WITH CORRECTIONS, and this
> sweep's own text was not rewritten from them. Read the check before you act on a quote here.

Date: 2026-10-06. Scope: research only. No code was changed, nothing was built, the lab was not touched.

Object of the research: our Media Foundation H.264 encoder MFT, which does prediction, transform,
quantisation, reconstruction and deblocking with D3D11 compute on the BC-250 (Cyan Skillfish, gfx1013,
RDNA1 class, 40 CUs), and codes the slice with CAVLC on the CPU. Source at
`driver/umd/mft-h264/`.

Measured baseline on unit A, 2026-10-06, train b19 (`<BC250_ROOT>\scratch\m15\video-encode\LAB-B19-RESULT.md`):
bit exact against the inbox decoder in 82/82 cases; 1080p serial 11.51 ms per picture (GPU busy
6.53 ms, readback wait about 1 ms, CAVLC 3.42 ms on the CPU), pipelined 4.55 ms (219.8 per second);
the Windows inbox software H.264 encoder on the same CPU is 1.7 to 1.8 times faster than our
pipelined path. Quality against the inbox at the same nominal rate: Y -1.3 to -2.3 dB, chroma
-3.8 to -6.0 dB PSNR.

## 1. Method, and what could not be opened

Every source below was read at the source: the publisher page or the paper PDF, the extension
specification text, or the code file itself. Nothing here is taken from an abstract, a summary page
or a secondary description. Where a source is a slide deck rather than the conference paper, that is
said.

Could not be opened, listed so the gap is visible:

- Unpaywall was not usable. Its API refuses the project contact address `research@example.com`
  ("Please use your own email address in API calls", HTTP 422), and the standing rule forbids
  substituting a real address without asking first. OpenAlex (no address required) was used instead to
  resolve DOIs and to check for open-access copies.
- W.-N. Chen and H.-M. Hang, "H.264/AVC motion estimation implementation on Compute Unified Device
  Architecture (CUDA)", IEEE ICME 2008, Hannover, pp. 697-700. IEEE paywall, no open copy found
  (OpenAlex records no DOI and no OA location). This is the most cited origin of the 5-stage CUDA ME
  decomposition and of the often repeated "12 times faster than CPU" figure. Not used as evidence.
- N.-M. Cheung, X. Fan, O. C. Au, M.-C. Kung, "Video Coding on Multicore Graphics Processors", IEEE
  Signal Processing Magazine 27(2), 2010, DOI 10.1109/MSP.2009.935416. Paywalled, OA: none. This is the
  standard reference for the motion-vector-predictor dependency problem; the same mechanism is
  evidenced below from code (S2, S4) instead.
- Y. D. Gao, J. Zhou, "Motion vector extrapolation for parallel motion estimation on GPU", Multimedia
  Tools and Applications, 2012, DOI 10.1007/s11042-012-1074-4. Springer paywall, OA: none.
- R. Rodriguez-Sanchez et al., "Optimizing H.264/AVC interprediction on a GPU-based framework",
  Concurrency and Computation: Practice and Experience, 2011, DOI 10.1002/cpe.1911. Paywall, OA: none.
- A. Fuentes-Alventosa, J. Gomez-Luna, J. M. Gonzalez-Linares, N. Guil, R. Medina-Carnicer, "CAVLCU: an
  efficient GPU-based implementation of CAVLC", The Journal of Supercomputing 78, pp. 7556-7590, 2021,
  DOI 10.1007/s11227-021-04183-8. OpenAlex reports this as open access at Springer, but both the
  article page and the content PDF answer with a 303 to an identity-provider URL and return HTML, so
  the text could not be read. Its headline claim (2.5x to 5.4x over the previous state-of-the-art GPU
  CAVLC) is recorded here as unverified. The older, fully readable GPU CAVLC paper (S10) is used
  instead.
- B. George, B. Ashbaugh, IWOCL 2017 (S8): the ACM paper is paywalled. The authors' own IWOCL slide
  deck, hosted by IWOCL, was read in full and is cited as such.
- `neuron2.net/library/avc/overview_x264_v8_5.pdf` (Merritt and Vanam, "x264: A High Performance
  H.264/AVC Encoder") no longer exists; the host 301-redirects to a parked domain. Not followed.

## 2. The object, as read

Facts taken from the source on 2026-10-06, because every lever below is scored against them.

`shaders/cs_me.hlsl`, P pictures only, one thread group of 32 threads (one wave32) per macroblock:

- Integer search is a fixed three-stage grid: 5x5 at step 8 (reach +-16), then 5x5 at step 2, then 3x3
  at step 1. 25 + 25 + 9 = 59 candidate positions, each a full 16x16 SAD, so 15104 reference-sample
  loads per macroblock.
- The integer stages read the reference straight from a `ByteAddressBuffer` through `RefY()`, which
  clamps and does a `Load` per sample. There is no group-shared cache of the search window.
- Lane occupancy inside a stage is 25 of 32, then 25 of 32, then 9 of 32. Each candidate is scored by
  one lane running a 256-iteration loop (`SadInteger`).
- The winner is found by `Reduce()`, which barriers and then runs a serial loop on lane 0 over up to
  32 entries.
- Sub-pel is a 3x3 half-sample stage then a 3x3 quarter-sample stage, 18 candidate evaluations (the
  centre is re-evaluated in both), scored out of four group-shared windows (`gInt` 23x23, `gBRaw`
  23x18, `gH` 18x18, `gJ` 18x18) built by `CacheSubpelWindow`. The cross-lane sum is again a serial
  32-iteration loop, run on 9 of 32 lanes.
- Motion cost is `sad + gLambda * (abs(mvx) + abs(mvy))`, that is, linear in the vector measured from
  **zero**. There is no motion vector predictor in the shader and no predictor-relative rate term.
- There are no predictor candidates at all: no median of the neighbours, no previous-picture vector, no
  co-located vector. The search always starts at (0,0).
- There is no early termination. The static-macroblock case is handled only afterwards, by snapping
  the vector back to (0,0) when `gZeroSad <= gBestSad + gSkipBias`.
- Group shared memory is about 9.8 KB (the four sub-pel windows alone are 6.4 KB as `int` arrays), and
  the task brief records that the shader is occupancy-limited by it.

`shaders/cs_mb.hlsl`:

- I pictures: Intra_16x16 only, four luma modes and four chroma modes, chosen by **SAD of the
  prediction** (`gRedCost`), luma and chroma scored separately, no rate term and no transform.
- The intra dispatch walks one anti-diagonal of the picture per dispatch (`mbx + mby == gDiagonal`),
  which at 1080p is 120 + 68 - 1 = 187 dispatches per I picture.
- P pictures: "Every macroblock is P_L0_16x16 with the vector cs_me.hlsl chose". The file states the
  reason for having no intra macroblocks in P pictures: "they would force the P picture onto the
  wavefront as well. Scene changes are handled by the rate controller forcing an IDR."
- Chroma QP does go through Table 8-15 (`encoder.cpp:ChromaQpFromLuma`, `gQpC`), and chroma AC
  coefficients are coded (`gCbpChroma` reaches 2), so the chroma deficit is not a missing QP map and
  not dropped chroma AC.

`src/h264_cavlc.cpp`: CAVLC on the CPU, strictly serial over macroblocks, with the two dependencies
that make it serial: `NcLuma`/`NcChroma` (the coeff_token context from the left and above blocks) and
`MvPred`/`SkipMvPred`.

`src/gpu_pipeline.*`: two slots shipped (`kShippedPipelineDepth = 2`), max 4. The header states the
present reasoning: "Two is enough to cover the entropy coding of one picture with the GPU work of the
next (...) a deeper pipeline only adds latency".

Levels staging buffer: `kLevelsWordsPerMb = 204` uints, 816 bytes per macroblock. At 1080p that is
6.66 MB read back per picture to emit a slice of a few tens of KB; at 219.8 pictures per second that
is about 1.46 GB/s of CPU reads of staging memory.

Build: `build.ps1` compiles every shader with `fxc /T cs_5_0`. Shader Model 5.0 has **no** wave
intrinsics. Any lever that says "use `WaveActiveSum`" therefore also means "move these shaders to DXC
and `cs_6_0`", which is a real and separately testable cost.

## 3. Sources

### S1. BC-250 VA-API driver and Vulkan compute encoder (same silicon)

Citation: simpmix, "AMD BC-250 VA-API Driver and Video Acceleration Suite
(`bc250-encoding-decoding-fix`)", GPL-3.0, local copy at
`<BC250_ROOT>\ref\bc250-encoding-decoding-fix__WARN-GPL-read-only-no-code-import`, git
`774783d4be9c07a0a85531ec2feaf7df0e335d48`, 2026-09-27. Files read: `README.md`,
`approach1-compute-encoder/shaders/motion_estimation.comp`, `docs/DEVLOG.md`,
`docs/multicore_cavlc_design.md`.

This is the single most relevant source in the sweep: a Vulkan compute H.264 encoder on the **same
part**, written because, as the README states, the BC-250's "physical VCN (Video Core Next) hardware
engine was permanently unprovisioned and eFused off at the factory". Licence is GPL-3.0 and the
directory name carries the trap: facts only, no code import.

What it claims, with the measured numbers:

- Throughput, README "Encoding Benchmarks (Measured on BC-250 Silicon)": H.264 Vulkan compute, "**720p**:
  179 fps", "**1080p**: 100-134 fps". Our pipelined path at 219.8 per second (1080p) and 363.3 per
  second (720p) is already faster than this.
- Their ME shader design (`motion_estimation.comp`): `layout(local_size_x = 64)`, with the comment
  "Wave64 / Dual-Wave32 - native-Wave32 SIMD32 execution model"; `shared float search_window[48][48]`
  and `shared float current_mb[16][16]`; a cooperative load, "2. Cooperative load of 48x48 search
  window (2304 pixels / 64 threads = 36 px/thread)".
- Cross-lane reduction with subgroup arithmetic, not a serial loop: `subgroupAdd(val)` under
  `GL_KHR_shader_subgroup_arithmetic`, with a shared-memory tree reduction only as the fallback. The
  comment: "On AMD RDNA architectures (BC-250 Oberon / Cyan Skillfish), subgroupAdd() reduces
  cross-lane registers directly".
- Search: "4. Hierarchical Adaptive Diamond Search", 4 diamond offsets per step, `for (int step = 8;
  step >= 1; step /= 2)`, i.e. 4 steps of 4 candidates, far fewer than our 59.
- Early termination on both ends: a zero-motion test before the search, `const uint
  STATIC_MB_THRESHOLD = 512;` with the comment "Early exit if the block is static (saves ~60% execution
  time in gaming/video)", and a mid-search break `if (shared_best_cost < 768) break;`.
- SAD subsampling: "2:1 Checkerboard subsampling cuts ALU and shared memory load by 50%", with the
  partial sums scaled by 2 to keep the magnitude comparable.
- Sub-pel is a search-then-refine of 16 candidates: "this refinement pass only evaluates 16 candidates
  total (8 half-pel + 8 quarter-pel) per MB". Their sub-pel reads the reference directly rather than
  from the cache, with an explicitly stated reason: "search_window's 48x48 footprint exactly covers
  this MB's +-16px integer search range with ZERO margin for the 6-tap filter's extra +-2/+3-pixel
  reach, so a best-integer match sitting at the edge of the search range would read out of bounds off
  that cache." Our `CacheSubpelWindow` solves the same problem the other way, by deriving the exact
  23x23 extent; that derivation is the better answer and should be kept.
- Their own measured honesty on sub-pel value: on the `testsrc` clip "17.10 -> 17.21 dB average PSNR
  (SSIM 0.846) - a real but SMALL gain", because instrumentation "shows only ~2.5% of macroblocks per
  frame ever land on a fractional motion vector at all" on that content.
- DEVLOG section 19, 1440p, 300 frames, mean over P pictures: CAVLC on the CPU "5.44 ms (40%)" at
  QP 12 and "11.19 ms (58%)" at QP 25, GPU total 4.08 and 4.93 ms, wall 13.69 and 19.26 ms. The
  diagnosis: "CAVLC is not compute-bound on entropy coding, it is bandwidth-bound on scanning a buffer
  that is almost entirely zeroes", with the arithmetic "At 1440p there are 14,400 macroblocks x 24
  blocks x 16 `int`s = **22.1 MB** of `quant_levels` and another 22.1 MB of `coeff` (...) to produce a
  14 KB frame."
- DEVLOG section 20 heading, measured: "The coefficient readback was a 16x overcopy. Removing it: +30%
  throughput, and CAVLC got 30-37% faster without being touched."
- Earlier DEVLOG, the two memory findings that dominated everything else: staging buffers allocated
  `HOST_VISIBLE|HOST_COHERENT` without `HOST_CACHED` cost "a ~36-38 µs/MB cost, identical at every
  resolution"; fixing it dropped "CPU CAVLC+bitstream time (...) 125-137x (360 ms -> 2.6 ms/frame at
  1080p)". A second, separate bug in `find_memory_type()` picking an uncached type gave another 5-8x.
- DEVLOG section 26.8, their answer to the CPU entropy bottleneck: OpenMP slice-parallel CAVLC/CABAC,
  with "~9.4 ms per frame at 1080p (>60% of total frame time)" before the change, phase 1 parallel
  encode into per-slice RBSP buffers and phase 2 sequential NAL assembly, "Guarantees 100%
  byte-for-byte determinism".
- `docs/multicore_cavlc_design.md` establishes why slices are the safe unit: neighbour availability is
  already clamped to the slice, and this "matches ITU-T H.264 6.4.9 exactly (a neighbouring macroblock
  belonging to a different slice than the current macroblock is defined as **not available**)". It also
  names the blocker honestly: "the current per-slice loop places each slice's EBSP bytes in-place at a
  *serially-computed* running offset".

Application to ours:

1. `cs_me.hlsl` has no search-window cache at all; theirs caches 48x48. Our integer stages do 15104
   clamped global loads per macroblock where a cached window needs about 3364 samples once. See S7 for
   the measured size of this effect in isolation.
2. `Reduce()` and the sub-pel 32-iteration sum are serial loops on one lane. Theirs uses `subgroupAdd`.
   Cost for us: `cs_5_0` to `cs_6_0` and fxc to DXC.
3. We have no early termination; they measure the zero-motion exit at about 60 percent of execution
   time on game and video content. For us this is a pure win on static content and costs nothing on
   moving content, because the zero-vector SAD is already computed (`gZeroSad`).
4. Their 2:1 checkerboard subsampling is a quality trade we should **not** copy while T3 is already
   missed. Note it as a later speed lever once quality is in range.
5. Their CAVLC history is our future: the levels buffer is the problem, not the entropy arithmetic. Our
   816 bytes per macroblock and 6.66 MB per 1080p picture is the same shape as their 22.1 MB. A
   GPU-side non-zero mask, or moving CAVLC to the GPU entirely (S10), attacks the readback and the CPU
   term at once.

### S2. x264 OpenCL lookahead motion search (code)

Citation: x264, `common/opencl/motionsearch.cl`, `common/opencl/subpel.cl`, `common/opencl/x264-cl.h`,
read from `https://code.videolan.org/videolan/x264/-/raw/master/...` on 2026-10-06. GPL-2.0.

This is the canonical open answer to "how do you do predictive, predictor-relative motion search on a
GPU when the predictor depends on the neighbour you have not coded yet". Four design facts, all of
which our shader lacks.

1. **The cost is relative to the predictor, and logarithmic, not linear.** In `motionsearch.cl` every
   candidate is scored `sad_8x8_ii(...) + lambda * mv_cost(abs_diff(trymv, mvp) << (2 + scale))`, and
   `x264-cl.h` defines

   ```
   int mv_cost( uint2 mvd )
   {
       float2 mvdf = (float2)(mvd.x, mvd.y) + 1.0f;
       float2 cost = round( log2(mvdf) * 2.0f + 0.718f + (float2)(!!mvd.x, !!mvd.y) );
       return (int) (cost.x + cost.y);
   }
   ```

   That is an approximation of the signed Exp-Golomb length of the MVD, which is what the bitstream
   actually pays. Ours is `gLambda * (abs(ix) + abs(iy))`, linear and measured from zero.

2. **The predictor dependency is broken by alternating the direction between iterations.** The kernel
   builds the MVP from neighbours of the *previous* iteration's vector field, and the direction flips:
   the comments are "/* even iterations: derive MVP from up and left */" and "/* odd iterations: derive
   MVP from down and right */", with `mvp = (i_mvc <= 1) ? mvc_local[0] : x264_median_mv(mvc_local[0],
   mvc_local[1], mvc_local[2])`. Nothing inside a dispatch depends on anything else inside that
   dispatch.

3. **The candidate list is predictors first, diamond second.** After the predictor-seeded diamond
   (`dia_offs[4] = {{0,-1},{-1,0},{1,0},{0,1}}`) which breaks as soon as no offset improves
   (`if( (cost >> 2) >= bcost ) break;`), the kernel tests the previous iteration's vector and each
   neighbour candidate, but only when they are not already covered: `if( diff.x > 1 || diff.y > 1 )`.

4. **A whole-macroblock early-out.** "current mvp matches the previous mvp and we have not changed
   scale. We know we're going to arrive at the same MV again, so just copy the previous result to our
   output."

Two more mechanics worth stealing:

- Argmin across lanes without a second array: `cost_local[mb_i] = (cost<<2) | mb_i;` then a 4-way
  `min`, so the winning index falls out of the low bits. Our `Reduce()` runs a serial loop plus
  tie-breaking arithmetic for the same job.
- `subpel.cl` uses **SAD to search and SATD only to re-score the winner**. `subpel_refine` runs
  `HPEL_QPEL(hpoffs, sad_8x8_ii_hpel)` then `HPEL_QPEL(dia_offs, sad_8x8_ii_qpel)`, and then: "/*
  remeasure cost of bmv using SATD */", `satd_8x8_ii_qpel_coop4(...)`, whose own comment is "Four
  threads measure 8x8 SATD cost at a QPEL offset into an HPEL plane. Each thread collects 1/4 of the
  rows of diffs and processes one quarter of the transforms".

Application to ours, in order of expected value:

- Replace the zero-anchored linear motion cost in `cs_me.hlsl` with a predictor-anchored logarithmic
  one. This is the cheapest quality lever in the sweep: it is a few lines, it costs no extra SAD, and
  it directly changes which vector wins toward the one the bitstream can afford. It needs the
  predictor, which needs item 2.
- Add a predictor pass. Our P pictures are already one dispatch with no inter-macroblock dependency, so
  the x264 shape fits exactly: pass A writes a raw vector field (what `cs_me.hlsl` does today), pass B
  reads pass A's neighbours, forms the median MVP, and refines. Two dispatches instead of one.
  Alternatively seed from the previous picture's vector field, which needs no second pass at all and no
  new dependency, only one more buffer.
- Both change the chosen vector, therefore the bitstream, therefore the stored reference vectors of the
  tests. Neither changes the reconstruction maths, so conformance (recon equals the inbox decoder's
  output) is untouched. That distinction matters for how the b20+ validation is written.

### S3. Intel device-side AVC VME (specification)

Citation: `cl_intel_device_side_avc_motion_estimation`, Khronos OpenCL extension registry,
`https://registry.khronos.org/OpenCL/extensions/intel/cl_intel_device_side_avc_motion_estimation.txt`,
read 2026-10-06.

Why it is in the sweep: it is the only public, exact description of what a dedicated hardware motion
estimator actually evaluates, and it is therefore the specification of what we are competing with.

- Distortion: SAD is "The sum of absolute differences of every full/sub-pixel location in the source
  block w.r.t every corresponding full/sub pixel in the reference block as specified by a given MV."
  And the transform-domain refinement: "A simple wavelet transform that is used to refine the
  distortion measure of SAD. The per pixel difference goes through a 4x4 Haar transform. Then the SAD
  is replaced by the sum of the absolute values of the transform domain coefficients in the distortion.
  Haar transform is used as a coarse estimation of the integer transform." The hardware offers a
  cheaper SATD stand-in, not the full Hadamard.
- Partitions in one call: major shapes 16x16, 16x8, 8x16, 8x8; minor shapes 8x8, 8x4, 4x8, 4x4. We
  evaluate one shape.
- Search windows are a small enumerated set: EXHAUSTIVE "48x40 single or 32x32 dual, exhaustive
  spiral", SMALL "28x28 exhaustive", TINY "24x24", EXTRA TINY "20x20", DIAMOND "48x40 single or 32x32
  dual, diamond then gradient", LARGE DIAMOND "48x40 single or 32x32 dual, extended diamond". Our
  effective reach is +-21.
- Rate term: "A table which specifies the cost penalties at 8 control points. The first 7 control
  points represent the distances from cost center at powers-of-two locations (2^0 to 2^6), and the last
  control point represents the base penalty for distances that are out of range." That is the same
  logarithmic shape as x264's `mv_cost`, implemented as a table. Separate shape penalties exist for
  16x8/8x16, 8x8, 8x4/4x8, 4x4 and 16x16.
- Up to 16 reference pairs, plus bidirectional refinement.
- Intra estimation is in the same engine: luma shapes 16x16, 8x8, 4x4 and the full nine-mode luma set,
  four chroma modes.
- Execution shape: "they are defined only for subgroup size of 16, and thus using these built-in
  functions in a kernel will force a subgroup size of 16", and "must be encountered by all work items
  in a subgroup".

Application to ours: three concrete targets drop out. (a) A powers-of-two cost table is a cheap exact
form of the x264 logarithmic cost, computable with `firstbithigh` and no float maths, which matters
because our shader is `cs_5_0`. (b) The 16x8/8x16/8x8 shapes with shape penalties are the documented
next step past 16x16-only; the hardware evaluates them in one pass because the sub-block SADs of a
16x16 search are already the sums of the 8x8 SADs. In our `SadInteger` the four 8x8 partial sums are
free if we keep them separately, so one extra reduction buys the 16x8/8x16/8x8 decision without a
second search. (c) The Haar-adjusted SAD is a cheaper alternative to a true 4x4 Hadamard SATD if the
mode decision needs a transform-domain metric.

### S4. OpenH264 motion estimation (code)

Citation: Cisco OpenH264, `codec/encoder/core/src/svc_motion_estimate.cpp`, BSD-2-Clause, read from
`raw.githubusercontent.com/cisco/openh264/master/...` on 2026-10-06.

A production CPU encoder with the same baseline-profile scope as ours, which shows the same four
design choices as x264 arrived at independently.

- Every cost in the file is predictor relative: `COST_MVD (pMe->pMvdCost, (sMv.iMvX * (1 << 2)) -
  ksMvp.iMvX, (sMv.iMvY * (1 << 2)) - ksMvp.iMvY)`, i.e. the cost of the MVD against `sMvp`, with the
  bit cost read out of a table (`pMvdCost`) rather than computed linearly.
- The search starts at the predictor, not at zero: "// Step 1: Initial point prediction / init with
  sMvp", `sMv.iMvX = WELS_CLIP3((2 + ksMvp.iMvX) >> 2, ...)`.
- Then a candidate list is tested, each candidate skipped when it coincides with the current best:
  `if (((iMvc0 - sMv.iMvX) || (iMvc1 - sMv.iMvY)))`.
- Early stop before any search: `if (iBestSadCost < (int32_t) pMe->uSadPredISatd.uiSadPred) { //
  Initial point early Stop ... return true; }`, where the threshold is a prediction from the
  neighbourhood rather than a constant.
- SAD to search, SATD to score the result: `CalculateSatdCost` computes `pSatd(...)` once on the final
  position and adds the same `COST_MVD` term. There is even a `NotCalculateSatdCost` no-op variant
  selected by the speed preset, which is exactly the switch we would want.
- A special path for static content, `WelsMotionEstimateSearchStatic`, which evaluates only the zero
  vector and ends.

Application to ours: this is independent confirmation of the S2 pattern, from a codebase with the same
profile scope, so the two together make the "predictor-anchored cost plus SAD search plus SATD final
score" design the evidence-backed default rather than one project's taste.

### S5. H.264 motion estimation, background and instruction profile (open-access chapter)

Citation: M. E. Krishnan, E. Gangadharan, N. P. Kumar, "H.264 Motion Estimation and Applications",
chapter 4 in *Video Compression*, InTech, 2012, pp. 57-78, read at
`https://cdn.intechopen.com/pdfs/33733/InTech-H_264_motion_estimation_and_applications.pdf`.

Used for two things only, both of which are stable background rather than new results.

- Table 1, "Instruction profiling in Baseline Profile H.264", measured with Intel VTune on a Pentium IV
  3 GHz, caption conditions "Baseline profile, 30 CIF frames, 5 reference frames, +-16-Pel search
  range, and QP = 20". Arithmetic MIPS share: integer-pel motion estimation 78.31 percent, fractional-pel
  motion estimation 17.55 percent, fractional-pel interpolation 0.46 percent, Lagrangian mode decision
  0.55 percent, intra prediction 0.44 percent, variable length coding 0.03 percent, transform and
  quantisation 2.64 percent, deblocking 0.02 percent.
- Section 1.2.3 on why SAD dominates search and why the transform-domain metric is kept for the final
  decision: "The H.264 reference model software [5] uses SA(T)D, the sum of absolute differences of the
  *transformed* residual data, as its prediction energy measure (for both Intra and Inter prediction).
  Transforming the residual at each search location increases computation but improves the accuracy of
  the energy measure. A simple multiply-free transform is used and so the extra computational cost is
  not excessive."
- Section 1.1.8 states the hierarchical rule we are missing: "The motion vectors from lowest resolution
  are scaled and passed on as candidate motion vectors for each block to next level. At the next level,
  the motion vectors are refined with a smaller search area."

Application to ours: the profile says mode decision and intra prediction are cheap (about 1 percent of
arithmetic together) while integer search is 78 percent. Our quality gap is in the cheap parts
(16x16-only, no intra in P, luma-only decision) and our speed gap is in the expensive part. That is a
convenient split: the quality work should not cost much time, and the time work should not cost much
quality.

### S6. Fast sub-pixel motion estimation with a modelled cost surface (arXiv, measured RD)

Citation: W. Lin, K. Panusopone, D. M. Baylon, M.-T. Sun, Z. Chen, H. Li, "A Fast Sub-pixel Motion
Estimation Algorithm for H.264/AVC Video Coding", arXiv:1503.00085, read in full.

Conditions: implemented on the JM reference software, 100 frames per sequence, IPPP, 30 fps, search
range 16 for QCIF and 32 for CIF and SD, one reference frame, full search for integer-pel ME.

The claims and the numbers that matter here:

- The reference baseline is explicit: "The computation in the 16-point sub-pixel search method used in
  the JM thus becomes comparatively large", and Table 2 confirms it, listing Full Search at SP/PT
  (search points per partition) of exactly 16 for every sequence.
- The sub-pel error surface, unlike the integer one, is well behaved: "for the sub-pixel matching error
  surface, the unimodal surface assumption holds in most cases because of the smaller search range of
  sub-pixel ME as well as the high correlation between sub-pixels due to the sub-pixel interpolation."
- The predicted sub-pel vector can be computed in closed form from integer SADs already in hand. With
  the second-order model `f(x,y) = c1 x^2 + c2 x + c3 y^2 + c4 y + c5` fitted to the best integer SAD
  and its four diamond neighbours, "SPMV = (xp, yp) = argmin f(x,y) = (-B/2A, -D/2C)" (Eqn 4), and the
  authors note the fit is free: "Since most fast integer ME algorithms (...) end the ME process by
  searching the 4-neighboring points around the best integer point, using the 4-neighbor COST
  information does not introduce any extra cost to the integer ME process."
- The cost used is the Lagrangian form, not plain SAD: `COST = SAD + lambda_MOTION * R(MV)` (Eqn 5),
  where "R(MV) is the number of bits to code the MV".
- Table 2, measured, PSNR / bitrate / search points per partition. Mobile SD 720x576 QP 28: Full Search
  33.8 dB, 8228.28 kbps, 16 SP/PT; RFSME-proposed 33.79 dB, 8293.79 kbps, 2.88 SP/PT. Flower SD QP 24:
  Full Search 37.95 dB, 8428.84 kbps, 16; RFSME 37.95 dB, 8431.03 kbps, 2.39. Football CIF QP 28: 36.03
  dB, 1440.84 kbps, 16; RFSME 36.01 dB, 1451.55 kbps, 3.13.
- Conclusion in text: "With the RFSME-proposed method, the SP per partition size can be reduced to less
  than 3 for most sequences", at a PSNR cost of at most 0.03 dB and a bitrate cost under 1 percent in
  the tabulated cases.

Application to ours, and this is the largest single speed lever inside `cs_me.hlsl`:

- We evaluate 18 sub-pel candidates per macroblock. The evidence says 16 can go to under 3 at a PSNR
  cost of 0.01 to 0.03 dB. Our stage-3 integer search is a 3x3 at step 1, so we already hold exactly
  the four diamond neighbours' SADs that Eqn 4 needs. The fit is five adds, two subtracts and two
  divides on lane 0.
- Doing this also shrinks `CacheSubpelWindow`. If the sub-pel candidate set collapses from a 3x3 at
  +-2 then a 3x3 at +-1 (reach +-3 quarter-samples) to a predicted position plus a small check, the
  window extent shrinks, and the window is 6.4 KB of our 9.8 KB group shared memory. That is the direct
  route to the occupancy limit named in the task brief.
- Risk: none to conformance. The sub-pel windows exist precisely so that the chosen candidate is scored
  against the samples the decoder will build; choosing a different candidate changes the bitstream, not
  the reconstruction maths. The b20+ test fixtures that pin exact bitstreams would need regenerating.
- Caveat the paper itself raises: the second-order model assumes a fixed interpolation filter. H.264's
  six-tap filter is fixed, so the model applies; it would not apply to an adaptive-filter codec.

### S7. GPU motion estimation and DCT, with the shared-memory step isolated (PLOS ONE, open access)

Citation: S. Agha, F. Jan, H. A. Khan, M. Kaleem, M. Khan, "Efficient motion estimation and discrete
cosine transform implementation using the graphics processing units", PLoS ONE 19(8): e0307217, 2024,
DOI 10.1371/journal.pone.0307217.

Hardware: NVIDIA GeForce GTX 1080, Intel Core i7 at 2.9 GHz. Content 3840x2160, 25 frames, macroblocks
16x16, "Size of search area is (31x31) pixels".

This paper is useful for one reason above all others: it measures the shared-memory tiling step **on
its own**, with everything else held fixed.

Table 1, full search, 25 frames of 4K:

| Variant | Time (25 frames) | Speedup vs serial |
|---|---|---|
| FS serial CPU | 874 s | 1 |
| FS macroblock-parallel, direct global reads | 11.4 s | 77 |
| FS macroblock-parallel, shared memory | 5.9 s | 149 |
| FS macroblock and search-area parallel | 1.4 s | 625 |

The step from "direct" to "shared memory" is 11.4 s to 5.9 s, that is **1.93x from caching the current
macroblock and the search area in shared memory alone**. The paper's statement of the mechanism:
"Shared memory is the fastest memory after register-file in GPU. Corresponding data is loaded into the
corresponding shared memories in a coalesced manner", and "If consecutive threads access consecutive
locations of global memory, then it leads to a coalesced memory access."

The hierarchical-search rows are also worth recording for scale: EHDS serial 16 s, EHDS
macroblock-parallel 0.24 s, EHDS with parallel SAD 0.15 s; TZS macroblock-parallel 1 s versus 69 s
serial.

Application to ours: our integer stages are exactly the "direct" variant. The measured 1.93x is on a
GTX 1080 with a 31x31 window and full search, so it is not transferable as a number, but it is the
cleanest available isolation of the mechanism, and it is corroborated independently by S1 (which chose
a 48x48 shared window on our own silicon) and by S11 (which says the same about reuse). Expected effect
on our 6.53 ms of 1080p GPU busy: the integer stages are the bulk of `cs_me`, which the file's own
history says was 70 percent of a P picture before the sub-pel windows existed, so a window cache plausibly
returns 1 to 2 ms per 1080p picture. That is a measurement to run, not a claim.

Sizing for us: reach is +-21 (16 + 4 + 1), so a complete window is 58x58 = 3364 bytes. Stored as packed
bytes (4 per uint) that is 3.4 KB, which fits beside the sub-pel windows only if the sub-pel windows
shrink first (S6). Stored as `uint` per sample it is 13.5 KB and does not fit. Order the work
accordingly: S6 first, then the window cache.

### S8. Wavefront parallel processing on GPUs (IWOCL 2017, authors' slides)

Citation: B. George, B. Ashbaugh, "Wavefront Parallel Processing on GPUs with an Application to Video
Encoding Algorithms", IWOCL 2017, DOI 10.1145/3078155.3078177. The ACM paper is paywalled; the
authors' slide deck was read in full at
`https://www.iwocl.org/wp-content/uploads/iwocl2017-ben-ashbaugh-wavefront.pdf`.

This is the direct literature on our I-picture dispatch-per-anti-diagonal shape.

- The problem statement names our case: video encoding "Exhibits 26 degree and 45 degree wavefront
  patterns for *Predicted Motion Vector* (PMV) and *Most Probable Mode* (MPM)".
- The challenges slide: "GPU schedulers not particularly designed to handle dependencies across
  work-groups (WGs)", "OpenCL spec allows launch order of WGs to be implementation specific",
  "Non-preemptable nature of WGs", and for the expanding and contracting parallelism of a diagonal
  sweep, "Not having enough compute to saturate machine" and "Idle polling".
- Four solutions were implemented and measured on Intel Graphics 530, with 15 planar YUV frames at
  480p, 720p, 1080p and 4K: distributed wavefront sweep, persistent threads with cyclic computation,
  distributed computation of wavefronts, and cyclic computation of multiple independent wavefronts.
- Measured conclusions, verbatim from the key observations slide: "Distributed wavefront sweep
  performed poorly despite most efficient sync", with the reasons "Low sampler utilization" and
  "Extracting parallelism more important"; "Cyclic & Distributed computation solutions performed
  identically"; "Multiple independent wavefront solution performed best specially for lower
  resolutions", quantified as "For 480p 21% over basic cyclic solution; sampler utilization up to 96%
  from 65%" and "For 4K no noticeable improvement over basic".
- The summary slide's two rules: "Cost of sync is not as significant when compared to the efficiency of
  extracting parallelism" and "In cases where only one encode stream is available basic cyclic
  computation solution is recommended unless multi-slice is an option."

Application to ours: our I picture issues 187 dispatches at 1080p, one per anti-diagonal, with
parallelism that rises and falls from 1 macroblock to 68 and back. The paper's measured answer is that
the synchronisation cost is not the problem; the idle machine at the ends of the sweep is. Two
affordable responses, in increasing cost: (a) process several independent wavefronts at once, which for
us means slices, and multiple slices are also the prerequisite for S1's parallel CAVLC, so one decision
serves both; (b) a persistent-kernel cyclic sweep with a global counter, which on `cs_5_0` we cannot
write safely (no forward-progress guarantee, no device-scope acquire-release) and which should be
deferred. Note also that I pictures are a small share of our frames, so this ranks below the P-picture
levers unless the GOP is short.

### S9. GPU H.264 encoder framework with per-stage speedups and quality cost (open access)

Citation: H. Su, M. Wen, N. Wu, J. Ren, C. Zhang, "Efficient Parallel Video Processing Techniques on
GPU: From Framework to Implementation", The Scientific World Journal 2014, article 716020,
DOI 10.1155/2014/716020, read at PMC3976889.

Hardware: NVIDIA GTX 260, GTX 460, Tesla C2050; Intel i7-2600; CUDA 4.2.

- Their ME is "Multiresolution Multiwindow (MRMW)" with "initial search range for MRMW is 16 x 16" and
  variable blocks "8 x 4, 4 x 8, 8 x 8, 16 x 8, 8 x 16, and 16 x 16".
- Measured ME speedups against the CPU: about 13x on GTX 260, 18x on GTX 460, 25x on Tesla C2050
  (Figures 16 to 18); 720p "50 fps for ME" (Table 5); the integrated encoder reaches 20 fps at 1080p.
- Shared memory for reuse is explicit: "pixels of a search window are loaded to shared memory and can
  be reused by all threads", with "One thread is assigned to process computation for a candidate search
  point" (Figure 5).
- Quality cost of the fast parallel search, which is the number that matters for us: "degradations of
  PSNR are from 0.08 dB to 0.56 dB" for MRMW, and overall "loss of PSNR value about 0.35 dB ~ 0.54 dB,
  0.14 dB ~ 0.77 dB, and 0.33 dB ~ 0.57 dB for D1, 720 p, and 1080 p video formats, respectively"
  (Section 6.2).
- On the entropy side they name the same dependency we have: "The value of nC of current block relies
  on nA and nB."

Application to ours: this is the best available calibration of what a GPU-side fast search costs in
quality against a reference encoder, and it is 0.1 to 0.8 dB for a search that already uses
multi-resolution and variable block sizes. Our luma gap is 1.3 to 2.3 dB with none of those. That is a
useful sanity bound: the published cost of going parallel is well under our present gap, so most of our
gap is not "because it is on the GPU", it is the three diagnosed causes.

### S10. Parallel CAVLC on a GPU (open-access journal, measured)

Citation: H. Y. Su, M. Wen, J. Ren, N. Wu, J. Chai, C. Y. Zhang, "High-Efficient Parallel CAVLC Encoders
on Heterogeneous Multicore Architectures", *Radioengineering* 21(1), April 2012, pp. 46-55, read at
`https://www.radioeng.cz/fulltexts/2012/12_01_0046_0055.pdf`.

Hardware: AMD Athlon 5200+ X2 2.7 GHz host, STORM-SP16 G220 stream processor at 700 MHz, NVIDIA GeForce
GTX 260+ at 1.29 GHz with 889 MB. Sequences `Into_tree` 720p and `Blue_sky` 1080p.

This is the paper that tells us exactly how to move CAVLC off the CPU. Its section 3 names the three
dependencies, which are precisely the three in our `h264_cavlc.cpp`:

- "Context-based data dependence (...) The value of nC of current block relies on nA and nB (...) the
  process to current block must wait until its top block and left block are processed."
- "Accessing dependence (...) the output of current MB must be behind the prior ones. (...) the first
  bit of current MB must connect to the last bit of the former MB."
- "Control dependence (...) in two layers: the frame layer and the block layer."

And its section 4.1 and 7 name the three fixes:

- "**Two scans:** to eliminate the context-based data dependence." A forward scan computes total_coeff
  per block for the whole frame, then a reverse scan derives nC from those counts, so no block waits
  for a neighbour's coding, only for its neighbour's count.
- "**Component-oriented coding:** to weaken the control dependence." The frame is processed component
  by component (Luma DC for the whole frame, then Luma AC, then Chroma DC, then Chroma AC) so that a
  warp never diverges on component type.
- "**Lag packing:** to solve the problem of parallel packing." Each macroblock is coded into
  code-words plus a length; the start positions then come from a parallel scan over the lengths (Fig.
  14 shows the iterative halving); then every thread writes its own bytes with the right shift
  (Fig. 15).

Measured numbers:

- Table 1, one 1080p I frame, serial versus parallel packing: execution time 29.8 ms to 2.53 ms
  (11.78x); total 52.2 ms to 2.92 ms (17.87x); and the one that matters most for us, **transform data
  size 23300.7 KB to 94.7 KB, a factor of 246**. For 720p: 15.6 to 1.35 ms, total 25.7 to 1.63 ms, data
  10279.8 KB to 51.4 KB.
- Table 3, whole CAVLC stage per frame: 720p CPU only 201 ms, GTX 260+ 5.29 ms (scan 1.47, coding 1.10,
  packing 1.35, others 1.37), speedup 38; 1080p CPU only 438 ms, GTX 260+ 9.05 ms (2.82, 1.86, 2.61,
  1.76), speedup 48.
- Table 2, whole encoder on the GTX 260+: 720p ME 15.4 ms, intra 3.21, CAVLC 5.29, filter 3.54, others
  4.42, 31.4 fps; 1080p ME 25.52, intra 6.01, CAVLC 9.14, filter 6.61, others 8.39, 18.0 fps.
- Figure 17(b): on the GPU the CAVLC time splits roughly evenly, "ranging from 20% to 30%" across order
  scan, inverse scan, coding, packing blocks and packing MB, with packing about 30 percent.

Application to ours, with the arithmetic:

- Our 1080p CAVLC is 3.42 ms on a Zen 2 core, which is already far better than their 438 ms CPU
  reference (different era, different code). But the data-volume argument transfers directly: we read
  back 204 uints per macroblock, 816 bytes, 6.66 MB per 1080p picture, to emit a slice of tens of KB.
  Their Table 1 measured the same ratio collapsing by 246x when packing moved onto the GPU.
- The pipeline consequence is the real prize. Today the CPU term and the readback are both on the
  critical path, and the two-slot pipeline exists only to hide them. A GPU CAVLC removes 3.42 ms of CPU
  work, removes the 6.66 MB readback, and makes the question of a third pipeline slot moot.
- Cost: this is the largest piece of work in the sweep. Lag packing needs a device-wide prefix sum over
  per-macroblock lengths, which in `cs_5_0` means a separate scan dispatch (we cannot use wave prefix
  intrinsics). The two-scan nC derivation needs a pass that currently does not exist, although
  `cs_mb.hlsl` already computes `gNnz[24]` per macroblock and stores a CBP, so a large part of the
  first scan is already done and simply not kept.
- Risk to bit exactness: high, but bounded and testable. Entropy coding is exactly reproducible, so the
  acceptance test is byte equality of the slice against the present CPU coder on the same levels. S1's
  OpenMP work makes the same promise ("Guarantees 100% byte-for-byte determinism") and is the model for
  how to phase it.
- Cheaper intermediate step, which S1 measured on our own silicon: a GPU-side non-zero mask so the CPU
  reads only the macroblocks and blocks that have coefficients. S1's DEVLOG section 20 measured "+30%
  throughput, and CAVLC got 30-37% faster without being touched" from removing a 16x coefficient
  overcopy alone.

### S11. AMD RDNA performance guidance (vendor)

Citation: AMD GPUOpen, "RDNA Performance Guide", `https://gpuopen.com/learn/rdna-performance-guide/`,
read 2026-10-06.

The parts that bear on `cs_me.hlsl`:

- "Thread group shared memory maps to LDS (Local Data Share)", and "LDS memory is banked on RDNA and
  GCN. It's spread across 32 banks. Each bank is 32 bits (1 DWORD). Bank conflicts increase latency of
  instructions." The guidance is to use "a struct of arrays or add padding to reduce access strides and
  bank conflicts".
- "GCN runs shader threads in groups of 64 known as wave64. RDNA runs shader threads in groups of 32
  known as wave32."
- "Make the workgroup size a multiple of 64 to obtain best performance across all GPU generations."
- On reductions: "GCN and RDNA support cross-wave ops via AGS, shader model 6, or SPIR-V subgroup
  operations. Cross-wave operators are great for reduction problems such as prefix sums, downsampling,
  filtering, etc."

Application to ours, with the occupancy arithmetic spelled out:

- Our group is 32 threads, one wave32. With about 9.8 KB of group shared memory and 64 KB of LDS per CU,
  at most 6 groups, hence 6 waves, can be resident per CU. An RDNA CU has two SIMD32 units with many
  wave slots each, so we are using a small fraction of them and have almost no latency hiding for the
  clamped global loads of `SadInteger`. Halving the group shared memory doubles the resident waves.
  This is the quantitative form of the "occupancy-limited" note in the task brief, and S6 is the lever
  that halves it.
- Our `gInt`, `gBRaw`, `gH`, `gJ` and `gSrcMb` all store one value per `uint`. Packing samples four per
  `uint` cuts the sub-pel windows from 6.4 KB to 1.6 KB, at the cost of shifts on read. Given that the
  shader is occupancy-bound rather than ALU-bound, that trade is likely favourable and is cheap to test.
- Row strides: `gInt` is 23 wide and `gH`/`gJ` are 18 wide, both odd or even in ways that interact with
  32 banks; the guide's padding advice applies and costs nothing.
- "Workgroup size a multiple of 64" conflicts with our 32-thread group. S1 chose 64 on this exact part.
  Worth one experiment, but note that 64 threads with the same group shared memory halves the groups per
  CU, so it only helps if the extra lanes are used (which S6 and the 8x8 partial sums of S3 would do).
- The cross-wave advice is the same conclusion as S1 and S2 reach in code, and again it costs the move
  from `fxc`/`cs_5_0` to DXC/`cs_6_0`.

### S12. Distortion-complexity of individual x264 parameters (measured)

Citation: R. Vanam, E. A. Riskin, S. S. Hemami, R. E. Ladner, "Distortion-Complexity Optimization of
the H.264/MPEG-4 AVC Encoder using the GBFOS Algorithm", read at
`https://mobileasl.cs.washington.edu/downloads/vanam-GBFOS.pdf`.

Conditions: x264 (26 March 2006), 30 fps, three target bitrates 30, 150 and 300 kb/s, three data sets
(15 standard QCIF sequences and two American Sign Language sets, QCIF and 320x240), Linux, 2.8 GHz
Intel CPU. Distortion is luma MSE averaged over the video; complexity is average encoding time per
frame.

The value here is Figure 4, which varies **one** parameter at a time and plots distortion against
encoding time, so the marginal value of each knob is visible.

- Figure 4(c), varying `subme` 1 to 7 on the ASL-1 set at 30 kb/s: MSE falls from about 14.85 at
  `subme=1` to about 11.4 at `subme=7`, while time rises from about 0.0155 to about 0.040 s per frame.
  The first step alone, `subme=1` to `subme=2`, moves MSE from about 14.85 to about 13.5 for about
  0.0045 s, which is about 0.41 dB of luma PSNR for a 29 percent time increase. x264's own
  `subpel_iterations` table, read in S13, defines what that step buys: `{0,0,0,0}` at subme 0 against
  `{1,1,0,0}` at subme 1 and `{0,1,1,0}` at subme 2, that is, half-pel and quarter-pel refinement
  moving from the winner only to all candidate block types.
- Figure 4(b), varying partitions: from `(P8x8)` at about 11.54 MSE to `(P8x8,P4x4,B8x8,I8x8,I4x4)` at
  about 11.37, over 0.033 to 0.040 s per frame. That is about 0.06 dB, which is small, but the baseline
  already includes P8x8; there is no 16x16-only point on this plot, so it does not bound what our
  missing 16x8/8x16/8x8 are worth.
- Figure 4(a), reference frames 1 to 16: MSE 12.22 to about 11.35, 0.021 to 0.040 s per frame.
- Headline: the two fast algorithms use about 1 percent and 8 percent of the tests an exhaustive search
  needs and give "a maximum decrease in peak-signal-to-noise ratio of less than 0.71 dB".

Application to ours: the measured shape of the `subme` curve says the sub-pel and mode-refinement
levels are the steepest part of the quality curve, which matches our diagnosis (luma-only decision,
no intra in P). It also warns that the returns flatten: `subme` 5 to 7 buys almost nothing for a large
time cost. Our target is to climb the steep part, not to reach the top.

### S13. x264 CPU motion search and sub-pel levels (code)

Citation: x264, `encoder/me.c`, read from `https://code.videolan.org/videolan/x264/-/raw/master/` on
2026-10-06. GPL-2.0.

Three facts used as design evidence rather than as measurements.

- The speed-quality ladder is explicit and was tuned empirically: "presets selected from good points on
  the speed-vs-quality curve of several test videos / `subpel_iters[i_subpel_refine] = { refine_hpel,
  refine_qpel, me_hpel, me_qpel }` / where me_* are the number of EPZS iterations run on all candidate
  block types, and refine_* are run only on the winner." The table runs from `{0,0,0,0}` to
  `{0,0,4,10}`.
- The reason the high levels pay for themselves is a rate-distortion argument, not a distortion one:
  "the subme=8,9 values are much higher because any amount of satd search makes up its time by reducing
  the number of qpel-rd iterations."
- The motion cost is again predictor relative and table driven:

  ```
  #define BITS_MVD( mx, my )\
      (p_cost_mvx[(mx)*4] + p_cost_mvy[(my)*4])
  ```

  with `p_cost_mvx = m->p_cost_mv - m->mvp[0]`, that is, the table is indexed by the vector but offset
  by the predictor, so the lookup yields the cost of the difference.
- The predictor set is searched before the pattern search (`x264_me_search_ref`): the clipped MVP
  first, then each supplied candidate through `x264_predictor_clip`, with a packed cost-and-index trick
  (`bpred_cost <<= 4; COPY1_IF_LT( bpred_cost, (cost << 4) + i );`) identical in spirit to the OpenCL
  one in S2, and then the zero vector only if the best is not already there (`if( bmx|bmy ) COST_MV( 0,
  0 );`).

Application to ours: this is the third independent instance of the same pattern, and it also gives the
right order of operations for us. x264 tests the predictor *first* and the zero vector *last*; we test
only a grid centred on zero. Changing the anchor is the single edit with the best ratio of quality to
effort in this sweep.

### S14. AMD Advanced Media Framework H.264 encoder API (vendor)

Citation: Advanced Micro Devices, "Advanced Media Framework - h.264 Video Encoder Programming Guide",
2025, read at `https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/AMF/master/amf/doc/
AMF_Video_Encode_API.md`.

Recorded because it defines what AMD's own fixed-function path exposes, which is the shape a Windows
application expects from an AMD H.264 encoder, and therefore the shape our MFT is eventually measured
against.

- `AMF_VIDEO_ENCODER_QUALITY_PRESET` with `BALANCED`, `SPEED`, `QUALITY`, `HIGH_QUALITY`, described as
  selecting "the quality preset in HW to balance between encoding speed and video quality".
- Sub-pel is a switch, not an algorithm: `AMF_VIDEO_ENCODER_MOTION_HALF_PIXEL` "Turns on/off half-pixel
  motion estimation" and `AMF_VIDEO_ENCODER_MOTION_QUARTERPIXEL` "Turns on/off quarter-pixel motion
  estimation".
- `AMF_VIDEO_ENCODER_B_PIC_PATTERN` 0 to 3, `AMF_VIDEO_ENCODER_MAX_NUM_REFRAMES` 0 to 16, six rate
  control methods, and `AMF_VIDEO_ENCODER_PRE_ANALYSIS_ENABLE` for lookahead.
- The work is done by the fixed-function "Video Compression Engine" (VCE), which on the BC-250 does not
  exist (S1).

Application to ours: a quality preset axis is the right place to put the levers from this sweep. Our
encoder has one operating point; the evidence (S12, S13) is that the useful settings lie on a curve.
A `SPEED` preset with the S1 early exits and subsampling, a `BALANCED` preset with the S6 predicted
sub-pel, and a `QUALITY` preset with SATD mode decision and intra in P would map onto what callers
already ask for through this API and through Media Foundation.

## 4. Levers, ranked

Ranked by expected value divided by cost, using only what the sources above measured. Every entry says
which file it touches and what it risks. "Conformance" below means the property our verdict tests:
that our reconstruction equals the inbox decoder's output. Changes that only alter which vector or
mode is chosen change the bitstream and the stored test vectors, but cannot break conformance; changes
to interpolation, transform, quantisation or reconstruction maths can.

**Q1. Predictor-anchored, logarithmic motion cost.** `shaders/cs_me.hlsl`, the two `gCost[...] = sad +
gLambda * ...` lines. Evidence: S2, S4, S13 all compute the cost of the MVD against the median
predictor with a table or a logarithmic approximation; S3 shows the hardware does the same with a
powers-of-two control-point table. Our linear, zero-anchored cost systematically prefers small vectors
regardless of what they cost in the bitstream. Expected: a share of the 1.3 to 2.3 dB luma gap at
constant rate, because it changes the rate side of the same operating point. Cost: small, once a
predictor exists (Q2). On `cs_5_0`, use `firstbithigh` for the log, not `log2`. Risk: bitstream
changes, conformance unaffected.

**Q2. A motion vector predictor at all.** `shaders/cs_me.hlsl` plus one buffer. Two options, both
evidenced: the x264 OpenCL shape of a second dispatch that reads the first dispatch's vector field and
alternates the neighbour direction (S2), or the cheaper temporal seed from the previous picture's
vector field, which adds no dependency and no dispatch. Start the search at the predictor and test the
zero vector as a candidate rather than as the origin (S13). Expected: this is what makes Q1 meaningful
and it also shortens the search, because the predictor is usually close. Cost: one buffer, optionally
one dispatch. Risk: bitstream changes only.

**S1-speed. Zero-motion early exit.** `shaders/cs_me.hlsl`. We already compute `gZeroSad` in stage 0.
Exiting when it is below a threshold costs nothing and S1 measured "saves ~60% execution time in
gaming/video" on this silicon; S4 does the same with a predicted rather than constant threshold.
Expected: a large share of `cs_me` time on desktop and game capture content, which is our actual
workload. Risk: changes which vector is chosen for near-static macroblocks; it is the same decision our
`gSkipBias` snap already makes, so the quality exposure is small and measurable.

**P1. Predicted sub-pel instead of an 18-point search.** `shaders/cs_me.hlsl`, the two refinement
stages and `CacheSubpelWindow`. Evidence: S6 measures 16 search points per partition falling to under
3 at 0.01 to 0.03 dB PSNR and under 1 percent bitrate, using a second-order fit to the best integer SAD
and its four diamond neighbours, which our stage-3 3x3 already computes. Expected: most of the sub-pel
cost, which the file's own history says was the dominant term before the windows existed, plus a
smaller group shared memory footprint, which is the occupancy lever (S11). Risk: bitstream changes
only; the windows that guarantee the scored samples equal the decoder's stay as they are.

**T1. Group shared memory diet, then an integer search window cache.** `shaders/cs_me.hlsl`. Pack the
window samples four per `uint` (6.4 KB to 1.6 KB) and pad rows against bank conflicts (S11), then add a
58x58 packed integer window (3.4 KB) so the integer stages stop doing 15104 clamped global loads per
macroblock. Evidence: S7 isolates 1.93x from shared-memory tiling alone; S1 and S9 both cache the
window; S11 is AMD's own guidance. Order matters: P1 first, because the window only fits after the
sub-pel windows shrink. Risk: none to the bitstream if the cached samples are the same samples
(the clamp must be reproduced exactly, as our `gInt` already does).

**T2. Cross-lane reductions.** `shaders/cs_me.hlsl` `Reduce()` and the sub-pel 32-iteration sum, and
`cs_mb.hlsl`'s `tid == 0` loops. Today a 32-iteration serial loop runs on one lane with 31 idle.
Evidence: S1 uses `subgroupAdd` on this part, S2 uses a 4-way `min` on a packed cost-and-index, S11 is
AMD's recommendation. Cost: this one is not free. It needs `cs_6_0` and DXC, which is a toolchain
change with its own validation. An interim step that works on `cs_5_0` is a log2 tree reduction (5
barriers instead of a 32-iteration loop) and the packed cost-and-index trick, which needs no new
toolchain at all. Do the interim step first.

**Q3. Partition shapes 16x8, 8x16 and 8x8.** `shaders/cs_me.hlsl` and `cs_mb.hlsl`. This is the first
of the three diagnosed quality causes. Evidence: S3 shows the hardware evaluates all shapes in one call
with per-shape penalties; S9's parallel encoder carries the full shape set and still lands within 0.1
to 0.8 dB of the reference. The implementation note from S3 is that the shape decision is nearly free
if the search keeps four 8x8 partial SADs instead of one 16x16 sum: the 16x8, 8x16 and 16x16 costs are
then sums of those four, and only the shape penalty and the extra vectors cost anything. Cost:
moderate, and it changes `cs_mb.hlsl`, the macroblock info layout and the CAVLC writer. Risk: a real
change to the coded syntax; conformance must be re-verified, not assumed.

**Q4. Mode decision on a transform-domain metric, luma and chroma.** `shaders/cs_mb.hlsl`, the
`gRedCost` block, and the inter path which today has no mode decision at all. Evidence: S5 quotes the
JM using SA(T)D "for both Intra and Inter prediction" and notes the transform is multiply free; S3's
hardware uses a 4x4 Haar as "a coarse estimation of the integer transform"; S2 and S4 both search with
SAD and score the winner with SATD. Our third diagnosed cause is "luma-only mode decision", and our
chroma gap is 3.8 to 6.0 dB, which is larger than the luma gap; scoring the final candidate on luma
SATD plus a chroma term is the direct answer. Note that we already have a Hadamard in `cs_mb.hlsl`
(lines 383 and 395), so the primitive exists. Cost: small for intra, since only four modes are scored.
Risk: bitstream changes only.

**Q5. Intra macroblocks inside P pictures.** `shaders/cs_mb.hlsl`. The file states the reason they are
absent: they would put the P picture back on the anti-diagonal wavefront. Evidence that this is
solvable without giving up the single-dispatch P picture: S8 measures wavefront strategies and
concludes that extracting parallelism matters more than synchronisation cost, and that multiple
independent wavefronts (slices) is the best answer at and below 720p. A cheaper route exists and should
be tried first: allow only Intra_16x16 DC in P pictures, which needs no neighbour samples when the
neighbours are unavailable, or code intra macroblocks with constrained intra prediction, which breaks
the dependency on inter-coded neighbours by definition. Cost: moderate. Risk: syntax change,
conformance must be re-verified. Expected: this is the lever for drift, and drift is the most likely
explanation for why our chroma gap is larger than our luma gap.

**C1. Non-zero mask on the levels readback.** `shaders/cs_mb.hlsl` and `src/gpu_pipeline.cpp`. We read
back 816 bytes per macroblock, 6.66 MB per 1080p picture, to emit tens of KB. Evidence: S1 measured
"+30% throughput, and CAVLC got 30-37% faster without being touched" from removing a 16x coefficient
overcopy on this silicon, and S10's Table 1 measured the transferred data falling from 23300.7 KB to
94.7 KB. `cs_mb.hlsl` already computes `gNnz[24]` and a CBP per macroblock, so the mask is almost free
to produce. Cost: small. Risk: none to the bitstream; a byte-equality test against the present coder
settles it.

**C2. Check the readback memory properties.** `src/gpu_pipeline.cpp`. Evidence: S1's two largest
findings by far were a staging buffer without a cached memory type (about 36 to 38 microseconds per
macroblock, fixed for a 125 to 137 times drop in CPU entropy time) and a memory-type selection bug
worth another 5 to 8 times. Both were on this exact board. Our "readback wait about 1 ms" suggests we
do not have their first bug, but the measurement is worth making deliberately rather than inferring,
because it is the cheapest check in this list.

**C3. CAVLC on the GPU.** `src/h264_cavlc.cpp` to new shaders. Evidence: S10 gives the full
architecture (two scans to kill the nC dependence, component-oriented coding to kill control
divergence, lag packing with a parallel prefix over the per-macroblock lengths to kill the bit-packing
dependence) and the measured result on a 2008-era GPU: 1080p CAVLC 9.05 ms, 48x over that era's CPU, and
the data volume collapsing 246x. S1 reached the same conclusion from the other side and took the
cheaper OpenMP slice-parallel route first. Expected for us: removes 3.42 ms of CPU per 1080p picture
and the 6.66 MB readback, which together are the whole of what the two-slot pipeline exists to hide.
Cost: the largest item here. Risk: must be byte-exact against the present CPU coder; that is the test.

**C4. Slice-parallel CAVLC on the CPU, as the cheap version of C3.** `src/h264_cavlc.cpp` and
`encoder.cpp`. Evidence: S1's section 26.8 did exactly this and documents both the correctness argument
(ITU-T H.264 6.4.9 makes a different-slice neighbour unavailable by definition, so slices are a sound
unit) and the one real blocker (the serially computed output offset, fixed by a parallel encode phase
into per-slice buffers and a sequential NAL assembly phase). Multiple slices are also the prerequisite
for S8's best wavefront strategy, so one decision serves the I picture too. Cost: moderate, entirely on
the CPU side. Risk: slices cost bitrate, because neighbour prediction stops at the boundary; that cost
must be measured, not assumed.

**X1. Third pipeline slot.** `src/gpu_pipeline.h`, `kShippedPipelineDepth`. Listed last deliberately.
The header already argues that two slots cover the one serialisation that exists, and if C1 and C3 land
there is less left to hide. Before spending a lab trial on it, settle the measurement question in
section 5 below, because the answer decides whether a third slot can help at all.

**Not recommended while T3 is missed:** S1's 2:1 checkerboard SAD subsampling, and any further search
truncation. They are speed levers that cost quality, and quality is the failing target. Record them for
a later `SPEED` preset (S14).

## 5. Open questions the sweep could not settle

1. **The b19 stage columns do not add up and should be read again before sizing X1.** At 720p the table
   gives serial 6.46 ms with GPU busy 3.61 ms, readback 4.25 ms and CAVLC 1.70 ms, which sum to more
   than the serial total; at 1080p, pipelined 4.55 ms per picture is less than the 6.53 ms of GPU busy.
   Either the columns measure overlapping spans or "GPU busy" is not exclusive occupancy. Until that is
   resolved it is not possible to say from the data whether our pipelined path is GPU bound or CPU
   bound, and that determines whether the third slot, the CAVLC work or the `cs_me` work is the real
   critical path.
2. **Why is the chroma gap (3.8 to 6.0 dB) larger than the luma gap (1.3 to 2.3 dB)?** Ruled out by
   reading the source: Table 8-15 chroma QP mapping is implemented, and chroma AC is coded. Three
   hypotheses remain, in the order this sweep would test them: (a) drift, because P pictures contain no
   intra macroblocks and chroma error accumulates over the GOP (Q5); (b) the chroma vector is derived
   from a luma vector chosen on luma SAD alone, so a vector that is marginally better for luma can be
   much worse for chroma (Q4); (c) `chromaQpIndexOffset` defaults to 0 while the inbox encoder may use
   a negative offset, which would show up as the inbox spending its "4-7 % more bits" on chroma. (c) is
   a one-line experiment and should be run first, because if it explains a large part of the gap it
   changes the priority of (a) and (b).
3. **Does a 64-thread group help on this part?** S1 chose 64 on the same silicon and S11 recommends a
   multiple of 64, but doubling the group halves the groups per CU at constant group shared memory.
   The answer depends on whether the extra lanes have work, which they only do after P1 and Q3. Defer.
4. **What does the toolchain move cost?** T2, and any wave-intrinsic lever, needs DXC and `cs_6_0`
   instead of `fxc /T cs_5_0`. That is a build, signing and validation question, not a research
   question, and it should be priced separately before T2 is scheduled.
5. **CAVLCU (DOI 10.1007/s11227-021-04183-8) remains unread** and claims 2.5x to 5.4x over the GPU CAVLC
   evidenced in S10. If C3 is scheduled, that paper should be obtained first, since it is the only
   modern measurement of this exact problem.

## 6. Source list

| # | Source | Kind | Opened |
|---|---|---|---|
| S1 | simpmix, BC-250 VA-API driver and Vulkan compute encoder, GPL-3.0, git `774783d4`, local `ref\bc250-encoding-decoding-fix__WARN-GPL-read-only-no-code-import` | code and project log, same silicon | yes |
| S2 | x264, `common/opencl/motionsearch.cl`, `subpel.cl`, `x264-cl.h`, GPL-2.0 | code | yes |
| S3 | `cl_intel_device_side_avc_motion_estimation`, Khronos OpenCL registry | vendor specification | yes |
| S4 | Cisco OpenH264, `codec/encoder/core/src/svc_motion_estimate.cpp`, BSD-2-Clause | code | yes |
| S5 | Krishnan, Gangadharan, Kumar, "H.264 Motion Estimation and Applications", in *Video Compression*, InTech, 2012, ch. 4, pp. 57-78 | open-access book chapter | yes |
| S6 | Lin, Panusopone, Baylon, Sun, Chen, Li, "A Fast Sub-pixel Motion Estimation Algorithm for H.264/AVC Video Coding", arXiv:1503.00085 | preprint, measured RD | yes |
| S7 | Agha, Jan, Khan, Kaleem, Khan, PLoS ONE 19(8): e0307217, 2024, DOI 10.1371/journal.pone.0307217 | open-access paper, measured | yes |
| S8 | George, Ashbaugh, "Wavefront Parallel Processing on GPUs with an Application to Video Encoding Algorithms", IWOCL 2017, DOI 10.1145/3078155.3078177 | authors' slide deck (paper paywalled) | slides only |
| S9 | Su, Wen, Wu, Ren, Zhang, The Scientific World Journal 2014, art. 716020, DOI 10.1155/2014/716020 | open-access paper, measured | yes |
| S10 | Su, Wen, Ren, Wu, Chai, Zhang, *Radioengineering* 21(1), 2012, pp. 46-55 | open-access paper, measured | yes |
| S11 | AMD GPUOpen, "RDNA Performance Guide" | vendor guidance | yes |
| S12 | Vanam, Riskin, Hemami, Ladner, "Distortion-Complexity Optimization of the H.264/MPEG-4 AVC Encoder using the GBFOS Algorithm" | paper, measured per-parameter | yes |
| S13 | x264, `encoder/me.c`, GPL-2.0 | code | yes |
| S14 | AMD, "Advanced Media Framework - h.264 Video Encoder Programming Guide", 2025 | vendor documentation | yes |
| - | Chen and Hang, ICME 2008, pp. 697-700 | paper | no, paywalled |
| - | Cheung, Fan, Au, Kung, IEEE SPM 27(2), 2010, DOI 10.1109/MSP.2009.935416 | paper | no, paywalled |
| - | Gao and Zhou, Multimedia Tools and Applications, 2012, DOI 10.1007/s11042-012-1074-4 | paper | no, paywalled |
| - | Rodriguez-Sanchez et al., CCPE, 2011, DOI 10.1002/cpe.1911 | paper | no, paywalled |
| - | Fuentes-Alventosa et al., J. Supercomputing 78, pp. 7556-7590, 2021, DOI 10.1007/s11227-021-04183-8 | paper, marked OA but gated | no |

Licence note: S1 is GPL-3.0 and S2, S13 are GPL-2.0. Nothing from them may be copied into our driver.
What is used above is measured facts, design shapes and published numbers, which is what this document
is for.
