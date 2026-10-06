# Verification of `h264-encoder-literature-motion-estimation.md`: GPU motion estimation for H.264

Date: 2026-10-06. Scope: checking only. No code was changed, nothing was built, the lab was not touched.

Object: `<BC250_ROOT>\scratch\m15\video-encode\research\h264-encoder-literature-motion-estimation.md`, the literature sweep on GPU motion
estimation for H.264 written for our Media Foundation H.264 encoder MFT.

Method: every source was opened again at the source by this check, independently of the sweep. Papers were
downloaded as PDF and converted with `pdftotext`; tables that are images in the PDF (S5 Table 1, S6 Table 2,
S10 Tables 1 to 4, S12 Figure 4) were rendered to PNG and read as images, because a text extraction of those
pages interleaves columns and would have produced a false check. Code was downloaded from the upstream raw
URLs and grepped. DOIs were resolved against Crossref and OpenAlex with `mailto=research@example.com`.

Summary of verdicts:

| Source | Verdict |
|---|---|
| S1 BC-250 VA-API / Vulkan compute encoder | VERIFIED (two notes) |
| S2 x264 OpenCL lowres motion search | VERIFIED with CORRECTIONS (quote, candidate order, scope) |
| S3 Intel device-side AVC VME extension | VERIFIED |
| S4 OpenH264 `svc_motion_estimate.cpp` | VERIFIED with CORRECTION (one quote is a composite) |
| S5 Krishnan et al., InTech chapter | VERIFIED (Table 1 OCR-checked) |
| S6 Lin et al., arXiv:1503.00085 | CORRECTED (attribution of Eqn 3/4, two overstated conclusions) |
| S7 Agha et al., PLoS ONE 2024 | VERIFIED |
| S8 George and Ashbaugh, IWOCL 2017 slides | VERIFIED |
| S9 Su et al., Scientific World Journal 2014 | VERIFIED (three small quote slips) |
| S10 Su et al., Radioengineering 2012 | VERIFIED (one source claim contradicted by its own figure) |
| S11 AMD RDNA Performance Guide | VERIFIED (the occupancy arithmetic built on it is off) |
| S12 Vanam et al., GBFOS | CORRECTED (digitised Figure 4 values and the derived percentages) |
| S13 x264 `encoder/me.c` | VERIFIED |
| S14 AMD AMF H.264 encoder guide | VERIFIED with CORRECTION (seven rate-control methods, not six) |
| "could not open": Chen and Hang, Cheung et al., Gao and Zhou, Rodriguez-Sanchez et al. | citations VERIFIED real; still paywalled |
| "could not open": CAVLCU, DOI 10.1007/s11227-021-04183-8 | REJECTED. The paper is open access and downloads fine. It was read for this check |

Nothing in the sweep was found to be invented. One source (CAVLCU) was wrongly written off as unreadable, one
source (S6) carries an attribution error and two overstated conclusions, and S12's chart readings are loose
enough that a derived percentage in the sweep is wrong by about a quarter. The ranked lever list survives, with
the adjustments in section 4.

---

## 1. Source by source

### S1. simpmix, BC-250 VA-API driver and Vulkan compute encoder. VERIFIED

Local copy `<BC250_ROOT>\ref\bc250-encoding-decoding-fix__WARN-GPL-read-only-no-code-import`. Checked
`git log -1`: `774783d4be9c07a0a85531ec2feaf7df0e335d48`, Sun 27 Sep 2026. `LICENSE` first line
`SPDX-License-Identifier: GPL-3.0-only`. Citation correct.

Checked verbatim and found exact:

- `README.md:10` "whose physical VCN (Video Core Next) hardware engine was permanently unprovisioned and eFused
  off at the factory".
- `README.md:22-27` heading "### 1. Encoding Benchmarks (Measured on BC-250 Silicon)", "**720p**: 179 fps",
  "**1080p**: 100-134 fps" (the file uses an en dash in the range).
- `motion_estimation.comp:67` `layout(local_size_x = 64) in; // Wave64 / Dual-Wave32 - native-Wave32 SIMD32
  execution model`; `:217-218` `shared float search_window[48][48];` and `shared float current_mb[16][16];`;
  `:298` "2. Cooperative load of 48x48 search window (2304 pixels / 64 threads = 36 px/thread)"; `:233` "On AMD
  RDNA architectures (BC-250 Oberon / Cyan Skillfish), subgroupAdd() reduces cross-lane registers directly";
  `:242` `uint sub_sum = subgroupAdd(val);` under `#extension GL_KHR_shader_subgroup_arithmetic` (`:66`).
- `:333` "4. Hierarchical Adaptive Diamond Search", `:340` `ivec2 diamond[4] = { ivec2(0,1), ivec2(0,-1),
  ivec2(1,0), ivec2(-1,0) };`, `:342` `for (int step = 8; step >= 1; step /= 2)`, so 4 steps of 4 candidates,
  16 at most. The sweep's count is right.
- `:276` `const uint STATIC_MB_THRESHOLD = 512;`, `:324` "Early exit if the block is static (saves ~60%
  execution time in gaming/video)", `:373` `if (shared_best_cost < 768) break;`, `:353` "2:1 Checkerboard
  subsampling cuts ALU and shared memory load by 50%".
- `:145-152` the sub-pel cache argument, "search_window's 48x48 footprint exactly covers this MB's +-16px
  integer search range with ZERO margin for the 6-tap filter's extra +-2/+3-pixel reach, so a best-integer
  match sitting at the edge of the search range would read out of bounds off that cache", and "this refinement
  pass only evaluates 16 candidates total (8 half-pel + 8 quarter-pel) per MB".
- `:32-35` "17.10 -> 17.21 dB average PSNR (SSIM 0.846) - a real but SMALL gain" and "shows only ~2.5% of
  macroblocks per frame ever land on a fractional motion vector at all".
- `DEVLOG.md:2265` section 19 heading and the 19.1 table: CAVLC (CPU) 5.44 ms (40%) and 11.19 ms (58%),
  `shadow_copy` 3.03 and 2.92, GPU total 4.08 and 4.93, wall 13.69 and 19.26 ms, 1440p, 300 frames, mean over
  P frames, `testsrc` QP 12 and `testsrc2` QP 25. All exact.
- `DEVLOG.md:2269-2271` "CAVLC is not compute-bound on entropy coding, it is bandwidth-bound on scanning a
  buffer that is almost entirely zeroes"; `:2291-2294` the 14,400 x 24 x 16 = 22.1 MB arithmetic and "to
  produce a 14 KB frame".
- `DEVLOG.md:2487` section 20 heading, exact including "+30% throughput, and CAVLC got 30-37% faster without
  being touched" (the file writes "16x" with a multiplication sign).
- `DEVLOG.md:187-196` "~36-38 us/MB cost, identical at every resolution", the `HOST_VISIBLE|HOST_COHERENT`
  without `HOST_CACHED` cause, "CPU CAVLC+bitstream time dropped 125-137x (360 ms -> 2.6 ms/frame at 1080p)";
  `:196-209` the separate `find_memory_type()` defect, "~5-8x".
- `DEVLOG.md:3803-3812` section 26.8, "~9.4 ms per frame at 1080p (>60% of total frame time)", per-slice RBSP
  buffers, "Guarantees 100% byte-for-byte determinism".
- `multicore_cavlc_design.md:14` "EBSP bytes in-place at a *serially-computed* running offset"; `:45-47` the
  ITU-T 6.4.9 argument.

Two notes, neither fatal:

1. The 6.4.9 quote is cut short. The source reads "a neighbouring macroblock belonging to a different slice
   than the current macroblock is defined as **not available**, independent of physical adjacency"; the sweep
   closes the bracket after "not available" with no ellipsis. Substance unchanged.
2. The same design document states, two lines above the quoted blocker, "Real measurements below show CAVLC is
   currently 97-99% of frame time", which is a much stronger figure than the 40 to 58 percent of DEVLOG 19.1.
   The two were measured at different times and in different configurations. Anyone using S1's CAVLC history to
   size our own work should carry both numbers, not only the one the sweep quotes.

**Observation the sweep missed, and it weakens one of its own arguments.** S1's integer search scores with
`cost = cand_sad + pcs.lambda_motion * uint(abs(test_mv.x) + abs(test_mv.y))` (`motion_estimation.comp:365`),
that is, zero-anchored and linear, exactly the cost model the sweep criticises in our `cs_me.hlsl`. S1 is
therefore not evidence for lever Q1. Q1 rests on S2, S4, S13 and S3 alone, which is still three independent
codebases and one hardware specification.

### S2. x264 OpenCL lowres motion search. VERIFIED with corrections

Downloaded `common/opencl/motionsearch.cl`, `common/opencl/subpel.cl`, `common/opencl/x264-cl.h` from
`code.videolan.org/videolan/x264/-/raw/master/`. GPL-2.0. All four design facts are real:

- `x264-cl.h:127-131` `mv_cost` is exactly as quoted, including `round( log2(mvdf) * 2.0f + 0.718f +
  (float2)(!!mvd.x, !!mvd.y) )`.
- `motionsearch.cl:144` and `:158` the two direction comments; `:132-136` the `MVC` macro reads `in_mvs`, the
  previous iteration's buffer, and the kernel writes `out_mvs` at `:244`. The claim that nothing inside a
  dispatch depends on anything else inside it is correct.
- `:173-180` the "same MV again" early-out is verbatim.
- `:200-206` the packed cost-and-index trick and `if( (cost >> 2) >= bcost ) break;`.
- `x264-cl.h:36-39` `constant int2 dia_offs[4] = {{0, -1}, {-1, 0}, {1, 0}, {0, 1},};`.
- `subpel.cl:62-66` the four-thread SATD comment; `:231-237` `HPEL_QPEL( hpoffs, sad_8x8_ii_hpel );`,
  `HPEL_QPEL( dia_offs, sad_8x8_ii_qpel );`, `/* remeasure cost of bmv using SATD */`,
  `satd_8x8_ii_qpel_coop4(...)`. The "SAD to search, SATD to re-score the winner" reading is right.

Corrections:

1. The sweep prints `mvp = (i_mvc <= 1) ? mvc_local[0] : x264_median_mv(...)`. The source is
   `mvp = (i_mvc <= 1) ? convert_int2_sat(mvc_local[0]) : x264_median_mv( mvc_local[0], mvc_local[1],
   mvc_local[2] );` (`:171`). Cosmetic.
2. The candidate order is described loosely. The real order after the diamond is: `COST_MV_NO_PAD( 0 )` with
   `trymv = 0`, that is the zero vector at zero lambda (`:224`), then the previous iteration's vector
   (`:226-232`), then each neighbour candidate (`:235-240`). The sweep omits the zero-vector test. The
   `if( diff.x > 1 || diff.y > 1 )` guard is not "already covered by the diamond" but the source's own "try
   cost at ... MV, if MVP was too far away": it skips any candidate within one of the MVP, which the diamond
   around the MVP has effectively covered. The sweep's reading is in the right direction but is not what the
   code says.
3. **Scope, and this matters for how the sweep applies it.** The file's first line is
   `/* Hierarchical (iterative) OpenCL lowres motion search */`, `const int mb_size = 8;` (`:120`), and the
   SAD helpers are `sad_8x8_ii`. This is x264's lookahead on half-resolution planes, not its final motion
   estimation. The sweep's heading says "lookahead", but the application section carries the design straight
   over to our full-resolution 16x16 search without saying that the lambda, the vector scale and the
   `<< (2 + scale)` shift are lowres-specific. The design shape transfers; no number in it does.

### S3. Intel `cl_intel_device_side_avc_motion_estimation`. VERIFIED

Downloaded from the Khronos registry (286 KB of text). Every quote is verbatim at the stated place:

- SAD and Haar definitions, lines 270-285, including "Haar transform is used as a coarse estimation of the
  integer transform."
- Major shapes 16x16, 16x8, 8x16, 8x8 (`:194-197`); minor shapes 8x8, 8x4, 4x8, 4x4 (`:199-204`).
- The search-window table (`:405-437`): EXHAUSTIVE 48x40 or 32x32 dual with a spiral, SMALL 28x28, TINY 24x24,
  EXTRA TINY 20x20, DIAMOND and LARGE DIAMOND 48x40 or 32x32 dual with diamond then gradient.
- The motion vector cost table, 8 control points, 2^0 to 2^6 plus a base out-of-range penalty (`:328-335`).
- "Up to 16 pairs of reference image parameters" (`:232`).
- Intra: luma modes 0 to 8 (nine) and four chroma modes (`:644-663`); luma partition masks for 16x16, 8x8,
  4x4 (`:633-636`); shape penalties for inter and intra (`:1136-1193`).
- The subgroup-16 constraint (`:900-914`), both sentences verbatim.

The sweep's inference that keeping four 8x8 partial SADs makes the 16x8/8x16/8x8 decision nearly free is the
sweep's own arithmetic, not a statement of the specification. It is correct arithmetic, but it should not be
presented as something Intel documents.

### S4. Cisco OpenH264 `svc_motion_estimate.cpp`. VERIFIED with one correction

Downloaded from `raw.githubusercontent.com/cisco/openh264/master/`. BSD-2-Clause. Confirmed:

- `:240-243` "// Step 1: Initial point prediction", "// init with sMvp", `sMv.iMvX = WELS_CLIP3 ((2 +
  ksMvp.iMvX) >> 2, ksMvStartMin.iMvX, ksMvStartMax.iMvX);`.
- `:255` `if (((iMvc0 - sMv.iMvX) || (iMvc1 - sMv.iMvY)))`.
- `:278-279` the early stop and its comment "//Initial point early Stop".
- `:286-292` `CalculateSatdCost` and the `NotCalculateSatdCost` no-op variant.
- `:184-196` `WelsMotionEstimateSearchStatic` evaluates the zero vector and ends.
- The early-stop threshold really is a neighbourhood prediction: `uSadPredISatd.uiSadPred` is set from
  `pWelsMd->iSadPredMb` (`svc_base_layer_md.cpp:988`, and >>1 or >>2 for the sub-partitions), which comes from
  `PredictSad(pMbCache->sMvComponents.iRefIndexCache, pMbCache->iSadCost, 0, &pWelsMd->iSadPredMb)`
  (`svc_base_layer_md.cpp:1891`). The sweep's claim is right.

Correction: the quoted `COST_MVD (pMe->pMvdCost, (sMv.iMvX * (1 << 2)) - ksMvp.iMvX, (sMv.iMvY * (1 << 2)) -
ksMvp.iMvY)` appears at no single line. Line 207 uses `pMe->sMv` and `pMe->sMvp`; line 248 uses `kpMvdCost`,
`((sMv.iMvX) * (1 << 2))` and `ksMvp`. The sweep mixed the two. Also the early-stop line uses
`static_cast<int32_t>`, not a C cast. Substance unaffected: every cost in the file is the cost of the MVD
against the predictor, read from a table.

### S5. Krishnan, Gangadharan and Kumar, InTech 2012. VERIFIED

PDF fetched from `cdn.intechopen.com/pdfs/33733/`. Chapter 4 of *Video Compression*, pages 57-78, authors and
affiliation as cited.

Table 1 is an embedded image, so `pdftotext` yields nothing. It was extracted with `pdfimages` and read. Every
figure the sweep quotes is exact: integer-pel ME 78.31, fractional-pel ME 17.55, fractional-pel interpolation
0.46, Lagrangian mode decision 0.55, intra prediction 0.44, variable length coding 0.03, transform and
quantization 2.64, deblocking 0.02, total 100.00, all in the Arithmetic percentage column. The caption reads
"(Baseline profile, 30 CIF frames/s, 5 reference frames, +-16-Pel search range, and QP = 20)"; the sweep wrote
"30 CIF frames" and dropped the "/s".

Quotes: section 1.2.3 is "Sum of absolute difference" (`:392`) and contains the SA(T)D passage verbatim, except
that the source prints "SA (T) D" with spaces. Section 1.1.8 is "Hierarchical block matching" (`:244`) and
contains the scaled-motion-vector sentence verbatim.

One internal inconsistency in the source, which the sweep inherited without noticing: the body text that
introduces the table says "H.264/AVC SD in main profile encoding solution", while the table caption says
Baseline profile. The sweep follows the caption. Use the table as an order-of-magnitude profile, not as a
baseline-profile measurement.

### S6. Lin, Panusopone, Baylon, Sun, Chen and Li, arXiv:1503.00085. CORRECTED

PDF fetched from `arxiv.org/pdf/1503.00085`. Title, six authors and affiliations as cited.

Conditions verified exactly (`:377-382`): JM reference software, 100 frames per sequence, IPPP, 30 frames/sec,
search range 16 for QCIF and 32 for CIF and SD, one reference frame, full search for the integer-pel ME.

Quotes verified: "The computation in the 16-point sub-pixel search method used in the JM thus becomes
comparatively large" (`:50`); the unimodal-surface passage (`:74-76`); `COST = SAD + lambda_MOTION * R(MV)`
with "where R(MV) is the number of bits to code the MV" (`:212-214`); "the SP per partition size can be reduced
to less than 3 for most sequences" (`:492`); and the "does not introduce any extra cost to the integer ME
process" passage (`:505-508`).

Table 2 was read from a render of page 12, because the text extraction interleaves the PSNR, BR and SP/PT
columns across blocks and would have produced a false result. The three rows the sweep quotes are exactly
right:

| Sequence, QP | Full Search | RFSME-Proposed |
|---|---|---|
| Mobile SD 720x576, QP 28 | 33.8 dB, 8228.28 kbps, 16 | 33.79 dB, 8293.79 kbps, 2.88 |
| Flower SD 720x576, QP 24 | 37.95 dB, 8428.84 kbps, 16 | 37.95 dB, 8431.03 kbps, 2.39 |
| Football CIF, QP 28 | 36.03 dB, 1440.84 kbps, 16 | 36.01 dB, 1451.55 kbps, 3.13 |

Full Search is 16 SP/PT in every one of the six blocks, as the sweep says.

Four corrections:

1. **Attribution.** The sweep presents the second-order model `f(x,y) = c1 x^2 + c2 x + c3 y^2 + c4 y + c5`
   and `SPMV = argmin f = (-B/2A, -D/2C)` as this paper's method. In the paper these are Eqn (3) and Eqn (4)
   in the background section, attributed to references [7] and [9]: "In [7, 9], Eqn (3) was used to determine
   one of the SPMVs, which used the best integer-pixel SAD and the SADs of its four diamond integer neighbors"
   (`:116-118`). The paper's own contribution, RFSME, is a rough search over all partitions followed by a
   precise search on the best one. The lever is still sound, but the citation for the closed-form fit should be
   [7] and [9] (Yang et al. and the FPME paper), not this work.
2. **"16 can go to under 3"** overstates. RFSME's SP/PT across the six blocks is 0.87, 3.1, 3.13, 3.69, 2.88,
   2.39. Three of six are above 3. The paper says "less than 3 for most sequences", which is what it is.
3. **"a bitrate cost under 1 percent in the tabulated cases"** is false. Akiyo QCIF QP 24 goes 56.05 to 57.05
   kbps, +1.78 percent, at -0.02 dB. The other five are +0.03 to +0.80 percent. PSNR cost is 0.00 to 0.03 dB
   everywhere, so that half of the sweep's claim holds.
4. **Scope.** SP/PT is search points *per partition size*, over JM's seven partition sizes, with a full-search
   integer ME. Our encoder has one partition size and 18 sub-pel candidates per macroblock. The published
   saving does not transfer as "18 goes to 3"; what transfers is that a closed-form fit over the four diamond
   neighbours predicts the sub-pel winner well enough to replace most of a 16-point search. Lever P1 should be
   stated that way and measured, not predicted from this table.

### S7. Agha, Jan, Khan, Kaleem and Khan, PLoS ONE 19(8): e0307217. VERIFIED

PDF fetched from the PLOS printable endpoint. DOI, authors, journal and year as cited.

Conditions verified (`:1411-1421`): "The CPU utilized is the Intel(R) Core i7 running @ 2.9 GHz. The GPU
utilized is the Nvidia GeForce GTX 1080", sequence "park joy" at 3840x2160, macroblock 16x16, "Size of search
area is (31x31) pixels", 25 frames. The sweep omits the clip name; everything else matches.

Table 1 is interleaved by the text extractor, but the body text states each figure separately and they agree
with the sweep: FS_Serial 874 s; FS_MB_par_direct 11.4 s, speedup 77 (`:1426-1432`); FS_MB_par_shared_mem
5.9 s, speedup 149 (`:1443`); FS_MB_SA_par 1.4 s, speedup 625 (`:1448`). The 1.93x figure is the sweep's own
division of 11.4 by 5.9 and is correct. The isolation claim holds: `:373-375` says Algorithm 3 "is same as that
of Algorithm 2, except the shared memory is used to store the current macroblock (CMB) and search area (SA)".

Quotes "Shared memory is the fastest memory after register-file in GPU. Corresponding data is loaded into the
corresponding shared memories in a coalesced manner" (`:375-376`) and "If consecutive threads access
consecutive locations of global memory, then it leads to a coalesced memory access" (`:195-196`) are verbatim.
The EHDS and TZS rows (16, 0.24, 0.15, 69, 1 seconds) match the table ordering.

### S8. George and Ashbaugh, IWOCL 2017. VERIFIED (slides, as stated)

DOI 10.1145/3078155.3078177 resolves in Crossref to "Wavefront Parallel Processing on GPUs with an Application
to Video Encoding Algorithms", George and Ashbaugh, Proceedings of the 5th International Workshop on OpenCL,
2017-05-16. The slide deck at `iwocl.org/wp-content/uploads/iwocl2017-ben-ashbaugh-wavefront.pdf` was fetched
and converted.

Verbatim: the 26 and 45 degree wavefront patterns for PMV and MPM; all four challenge bullets; "480p (858x480),
72-p (1280x720), 1080p (1920x1080), 4k (3840x2160) - 15 planar YUV frames" on Intel Graphics 530; "Four WPP
OpenCL solutions implemented and evaluated"; "Distributed wavefront sweep performed poorly despite most
efficient sync", "Low sampler utilization", "Extracting parallelism more important", "Cyclic & Distributed
computation solutions performed identically", "Multiple independent wavefront solution performed best specially
for lower resolutions", "For 480p 21% over basic cyclic solution; sampler utilization up to 96% from 65%", "For
4K no noticeable improvement over basic"; and both summary lines.

The summary slide adds a line the sweep could have used: "Cyclic computation with multiple independent
wavefronts solution performed best overall particularly for 720p resolutions and below", which is the cleanest
statement of the result for a 1080p encoder like ours (that is, at our resolution the gain is small).

### S9. Su, Wen, Wu, Ren and Zhang, Scientific World Journal 2014, art. 716020. VERIFIED

DOI resolves in Crossref to the stated title, five authors, journal and year, pages 1-19. Full text read from
PMC3976889.

Verified: MRMW named and described; "The initial search range for MRMW is 16 x 16"; variable block sizes "8 x
4, 4 x 8, 8 x 8, 16 x 8, 8 x 16, and 16 x 16"; "the degradations of PSNR are from 0.08 dB to 0.56 dB compared
with the reference software"; "a loss of PSNR value about 0.35 dB ~ 0.54 dB, 0.14 dB ~ 0.77 dB, and 0.33 dB ~
0.57 dB for D1, 720 p, and 1080 p video formats, respectively"; the interprediction speedups "about 13, 18, and
25, respectively" on GTX 260, GTX 460 and C2050; Table 5 row "GTX260 The proposed MRMW, x264, 720 p, ME, 12~14,
50 (for ME)"; "for 1080 p, the encoding speed achieves 20 fps". Platform: Intel i7-2600 quad-core 3.4 GHz,
CUDA-4.2, the three GPUs of Table 4.

Three small quote slips, none of which change anything: the source says "The value of nC of the current block
relies on nA and nB"; "One thread is assigned to process the computation for a candidate search point"; and
"the pixels of a search window are loaded to the shared memory and can be reused by all threads of the same
thread-block" (the sweep truncated the last clause).

The sweep's use of this source as a calibration of the quality cost of a parallel fast search is fair: the
0.08 to 0.56 dB is MRMW against the reference software, and the 0.14 to 0.77 dB is the whole CUDA encoder.

### S10. Su, Wen, Ren, Wu, Chai and Zhang, Radioengineering 21(1), 2012, pp. 46-55. VERIFIED

PDF fetched from radioeng.cz. Title, six authors, volume, issue, year and pages all correct.

The three dependences are quoted verbatim from page 3: "The value of nC of current block relies on nA and nB"
and "the process to current block must wait until its top block and left block are processed"; "the output of
current MB must be behind the prior ones", "the first bit of current MB must connect to the last bit of the
former MB"; "The control dependence lies in two layers: the frame layer and the block layer". The three fixes
appear as section 4.1 bullets and again in the section 7 conclusion, worded as the sweep gives them.

Tables 1 to 4 were read from renders of pages 7 and 9. Table 1 (I frame) is exact:

| | Blue_sky 1080p serial / parallel / speedup | In_to_tree 720p serial / parallel / speedup |
|---|---|---|
| Execution time (ms) | 29.8 / 2.53 / 11.78 | 15.6 / 1.35 / 11.56 |
| Transform time (ms) | 23.4 / 0.39 / 60 | 10.1 / 0.28 / 36.1 |
| Total (ms) | 52.2 / 2.92 / 17.87 | 25.7 / 1.63 / 15.77 |
| Transform data size (KB) | 23300.7 / 94.7 / 246 | 10279.8 / 51.4 / 200 |

Table 3 is exact as quoted, including the 720p CPU-only 201 ms against GTX 260+ 5.29 ms (1.47 / 1.10 / 1.35 /
1.37) at speedup 38, and 1080p 438 ms against 9.05 ms (2.82 / 1.86 / 2.61 / 1.76) at speedup 48.

Table 2 is exact and, importantly, the sweep attributed the rows correctly: the 720p row 15.4 / 3.21 / 5.29 /
3.54 / 4.42 at 31.4 fps and the 1080p row 25.52 / 6.01 / 9.14 / 6.61 / 8.39 at 18.0 fps are the GTX 260+ rows,
not the STORM rows. Platform is as cited: AMD Athlon 5200+ X2 2.7 GHz, STORM G220 at 700 MHz, GeForce 260+GTX
at 1.29 GHz with 889 MB.

One caveat the sweep should carry. Its sentence "on the GPU the CAVLC time splits roughly evenly, 'ranging from
20% to 30%'" is a correct verbatim quote of the paper's own text (page 8), but Fig. 17(b) on page 9 shows
15.52, 16.12, 20.63, 23.07, 5.01 and 19.65 percent. Two of the six parts lie outside the range the sentence
claims. Trust the figure. The sweep's other derived number, packing at about 30 percent, is the paper's own and
is supported by the figure (23.07 + 5.01).

### S11. AMD RDNA Performance Guide. VERIFIED

Page fetched. All five quoted passages appear as quoted: "Thread group shared memory maps to LDS (Local Data
Share)"; "LDS memory is banked on RDNA and GCN. It's spread across 32 banks. Each bank is 32 bits (1 DWORD).
Bank conflicts increase latency of instructions." with the struct-of-arrays and padding advice; "GCN runs
shader threads in groups of 64 known as wave64. RDNA runs shader threads in groups of 32 known as wave32.";
"Make the workgroup size a multiple of 64 to obtain best performance across all GPU generations."; "GCN and
RDNA support cross-wave ops via AGS, shader model 6, or SPIR-V subgroup operations. Cross-wave operators are
great for reduction problems such as prefix sums, downsampling, filtering, etc."

Correction to the arithmetic the sweep builds on this source. The group shared memory in `cs_me.hlsl` is about
8.9 KB, not 9.8 KB: `gInt` 23*23*4 = 2116, `gBRaw` 23*18*4 = 1656, `gH` and `gJ` 18*18*4 = 1296 each,
`gSrcMb` 256*4 = 1024, `gPart` 9*32*4 = 1152, the four 32-entry arrays 512, scalars 16, total 9068 bytes. At
64 KB per CU that is 7 resident groups, not 6. The four sub-pel windows are 6364 bytes, which the sweep rounds
to 6.4 KB correctly. The direction of the argument (occupancy-bound, halve the windows first) is unchanged.

### S12. Vanam, Riskin, Hemami and Ladner, GBFOS. CORRECTED (chart readings)

PDF fetched from `mobileasl.cs.washington.edu/downloads/vanam-GBFOS.pdf`. Four authors as cited.

Conditions verified (`:210-219`): 15 standard QCIF sequences, ASL-1 (10 QCIF clips) and ASL-2 (10 clips at
320x240), x264 "(March 26, 2006 version)", 30 fps, target bitrates 30, 150 and 300 kb/s, "a Linux machine with
a 2.8 GHz Intel CPU". Distortion is "the MSE of luma (Y) component for each frame and use its average over the
video" (`:82-84`), complexity is average encoding time per frame. The abstract claim is verbatim: the two fast
algorithms "take about 1% and 8%, respectively, of the number of tests required by an exhaustive search" and
give "a maximum decrease in peak-signal-to-noise ratio of less than 0.71 dB". Figure 4's caption confirms the
sub-plot order (a) ref, (b) partition sizes, (c) subme, (d) trellis, on the ASL-1 set at 30 kb/s.

Figure 4 was rendered and read. Corrections:

- (c) subme: subme=1 sits at MSE about 14.85 and about 0.0167 s, not 0.0155 s; subme=2 at about 13.58, not
  13.5; subme=7 at about 11.37. The first step is therefore about 10*log10(14.85/13.58) = 0.39 dB for about
  +23 percent encoding time, not "0.41 dB for a 29 percent time increase". The sweep's qualitative point (the
  low-subme end is the steep part, and subme 5 to 7 buys almost nothing) is correct and visible in the plot.
- (a) ref: ref=1 at about 12.25 (sweep: 12.22), ref=16 at about 11.37 (sweep: 11.35), times about 0.0205 to
  0.0405 s.
- (b) partitions: 11.535 at 0.0331 s to about 11.375 at 0.0403 s. The sweep's "about 0.06 dB" checks out
  (10*log10(11.54/11.375) = 0.063). Its observation that there is no 16x16-only point on this plot is correct:
  every legend entry contains P8x8 at least.

### S13. x264 `encoder/me.c`. VERIFIED

Downloaded from the same raw URL. Every quote is verbatim at lines 32 to 37 (the `subpel_iterations` preamble,
including the "subme=8,9" sentence), 38 to 51 (the table, `{0,0,0,0}` to `{0,0,4,10}`), 60 to 61 (`BITS_MVD`),
211 (`const uint16_t *p_cost_mvx = m->p_cost_mv - m->mvp[0];`), 231 (`x264_predictor_clip`), 239 to 249 (the
packed `bpred_cost <<= 4` trick) and 266 (`if( bmx|bmy ) COST_MV( 0, 0 );`). The sweep's reading of the ladder,
of the predictor-offset table lookup and of the predictor-first, zero-last ordering is accurate.

### S14. AMD Advanced Media Framework H.264 Video Encoder Programming Guide. VERIFIED with one correction

Fetched from the AMF repository. Copyright line "2025 Advanced Micro Devices, Inc." Confirms:
`AMF_VIDEO_ENCODER_QUALITY_PRESET` with BALANCED, SPEED, QUALITY and HIGH_QUALITY, description "Selects the
quality preset in HW to balance between encoding speed and video quality."; `MOTION_HALF_PIXEL` "Turns on/off
half-pixel motion estimation." (default true) and `MOTION_QUARTERPIXEL` "Turns on/off quarter-pixel motion
estimation." (default false); `B_PIC_PATTERN` values 0, 1, 2, 3; `MAX_NUM_REFRAMES` values 0 to 16;
`PRE_ANALYSIS_ENABLE`; and the document's opening statement that the component "exposes the AMD Video
Compression Engine".

Correction: the rate-control enum lists CONSTANT_QP, CBR, PEAK_CONSTRAINED_VBR, LATENCY_CONSTRAINED_VBR,
QUALITY_VBR, HIGH_QUALITY_VBR and HIGH_QUALITY_CBR, that is seven methods plus UNKNOWN, not six.

Minor terminology point: this guide says VCE, while the block S1 reports as eFused off on the BC-250 is VCN.
The two names belong to different generations of the same fixed-function encoder family. The conclusion (no
fixed-function encode on this part) is unaffected.

---

## 2. The sources the sweep could not open

| Citation | Check |
|---|---|
| Chen and Hang, ICME 2008, pp. 697-700 | Real. Crossref gives DOI 10.1109/ICME.2008.4607530, authors Wei-Nien Chen and Hsueh-Ming Hang, "2008 IEEE International Conference on Multimedia and Expo", pages 697-700, June 2008. The Crossref title record contains the typo "implmentation". Still paywalled, and OpenAlex still shows it with no DOI and no OA location, which is what the sweep reported. The sweep could have cited the DOI |
| Cheung, Fan, Au, Kung, IEEE SPM 27(2), 2010 | Real. DOI 10.1109/MSP.2009.935416, four authors as cited, March 2010, pages 79-89. Paywalled |
| Gao and Zhou, MTAP, 2012 | Real. DOI 10.1007/s11042-012-1074-4, authors Gao and Zhou, title as cited, pages 701-715. Paywalled |
| Rodriguez-Sanchez et al., CCPE, 2011 | Real. DOI 10.1002/cpe.1911, six authors (Rodriguez-Sanchez, Martinez, Fernandez-Escribano, Sanchez, Claver, Diaz), title as cited, November 2011. Paywalled |
| Unpaywall refusing `research@example.com` | Reproduced exactly: HTTP 422, `{"message": "Please use your own email address in API calls. See http://unpaywall.org/products/api"}`. The sweep's account is accurate, and its decision not to substitute a real address was correct |
| `neuron2.net` x264 overview PDF | Not re-checked; irrelevant to any claim |

### CAVLCU: the sweep's claim is REJECTED, and the paper has now been read

The sweep writes that the article page and the content PDF "answer with a 303 to an identity-provider URL and
return HTML, so the text could not be read", and records its headline as unverified.

That is wrong. `https://link.springer.com/content/pdf/10.1007/s11227-021-04183-8.pdf`, the exact URL OpenAlex
gives as the OA location, returns `HTTP 200 application/pdf`, 2,878,507 bytes, a valid PDF 1.6 of the full
article. It converts cleanly. The paper is hybrid OA, "The Author(s) 2021", and is also deposited at ETH Zurich
and at the University of Malaga. Whatever the sweep hit was transient or a client problem; it should have been
retried before the gap was recorded.

Having read it, the facts relevant to lever C3:

- Citation is correct: Fuentes-Alventosa, Gomez-Luna, Gonzalez-Linares, Guil, Medina-Carnicer, *The Journal of
  Supercomputing* 78, pp. 7556-7590, accepted 27 October 2021, published online 29 November 2021.
- The comparison target CAVLC_SU is precisely S10's algorithm: "the only existing state-of-the-art GPGPU
  implementation of CAVLC, which is the solution proposed by Su et al. [38, 39] ... We implemented CAVLC_SU
  from scratch following the description of the algorithm given by their authors [38, 39] and their support
  through private communication with Huayou Su."
- Headline verified: "our approach is between 2.5x and 5.4x faster than the only state-of-the-art GPU-based
  implementation of CAVLC" (abstract), and in section 5 "CAVLCU is between 2.5 and 5.4 faster than CAVLC_SU on
  the first architecture and between 3.0 and 6.7 on the second". Table 10 gives per-clip minimum, maximum and
  average: City 2.7/3.6/3.3 Maxwell, 4.2/6.2/5.2 Turing; Mother and Daughter 2.5/3.5/3.3 and 3.9/5.4/5.1;
  Ducks take off 3.3/5.4/4.1 and 3.0/6.7/4.8.
- Conditions: GeForce GTX 970 (Maxwell, CC 5.2) and GeForce RTX 2080 (Turing, CC 7.5); first 50 frames of City
  (QCIF), Mother and Daughter (CIF) and Ducks take off (720p); GOP 10; 11 QP values from 0 to 50; 128 threads
  per thread-block. Note the largest sequence is 720p, so nothing here is measured at 1080p.
- The four ideas, in the authors' words: one kernel only, "to avoid the long latency global memory accesses
  required to transmit intermediate results among different kernels, and the costly launches and terminations
  of additional kernels"; "an efficient synchronization mechanism for thread-blocks that process adjacent frame
  regions (in horizontal and vertical dimensions) to share results in global memory space"; "vectorized loads
  to move directly the quantized transform coefficients to registers"; and "register tiling to implement the
  zigzag sorting".
- Mechanism, measured: global memory transactions fall by 75.70 percent on Maxwell and 65.86 percent on Turing
  against CAVLC_SU.

Consequence for the sweep: open question 5 is closed, and lever C3 should be designed from CAVLCU rather than
from S10. The two papers also disagree in emphasis in a way that matters to us. S10 reaches its speed with
three kernels and a device-wide prefix scan; CAVLCU's first finding is that the multi-kernel structure is
itself the cost, and that one kernel with its own inter-thread-block synchronisation is faster. On `cs_5_0` we
cannot write CAVLCU's synchronisation safely (no forward-progress guarantee, no device-scope acquire-release),
which turns the toolchain question of open question 4 from an optimisation into a prerequisite for the single
best-evidenced CAVLC design.

---

## 3. Other things found while checking

1. **The object description in section 2 of the sweep is accurate.** Spot-checked against the source:
   `cs_me.hlsl` is `[numthreads(32, 1, 1)]`; the integer search is three stages of 5x5 step 8, 5x5 step 2, 3x3
   step 1, so 59 candidates; the cost line is literally `gCost[tid] = sad + gLambda * uint(abs(ix) + abs(iy));`
   with no predictor anywhere in the file; `gZeroSad` is captured in stage 0 and used only for the post hoc
   snap at `gZeroSad <= gBestSad + gSkipBias`; the four sub-pel windows are `gInt[23*23]`, `gBRaw[23*18]`,
   `gH[18*18]`, `gJ[18*18]`. `cs_mb.hlsl` carries the comments the sweep quotes, dispatches intra per
   anti-diagonal (`gDiagonal`), has `Hadamard4x4` already (lines 383 and 395), and `encoder.cpp` has
   `ChromaQpFromLuma`. `build.ps1` compiles with `fxc` and `/T cs_5_0`. `gpu_pipeline.h` has
   `kShippedPipelineDepth = 2` and `kMaxPipelineDepth = 4`; `mb_layout.h` has `kLevelsWordsPerMb = 204`. The
   only numeric slip is the LDS total noted under S11.
2. **"readback wait about 1 ms" is a residual, not a measurement.** `LAB-B19-RESULT.md`'s readback column reads
   4.25 ms at 720p and 7.57 ms at 1080p. The sweep's "about 1 ms" is 11.51 - 6.53 - 3.42 = 1.56 ms, and 6.46 -
   3.61 - 1.70 = 1.15 ms at 720p. The sweep flags the column inconsistency honestly in its open question 1, but
   then states the derived figure as a fact in its baseline paragraph. Until the columns are understood, no
   lever should be ranked on the size of the readback term.
3. **Lever Q5's mechanism is stated wrongly, although the recommendation survives.** The sweep suggests
   "allow only Intra_16x16 DC in P pictures, which needs no neighbour samples when the neighbours are
   unavailable". In a P slice the neighbours normally *are* available, because they are reconstructed, and that
   is exactly the dependency. What removes it is `constrained_intra_pred_flag = 1` in the PPS, which by
   definition makes inter-coded neighbours unavailable for intra prediction; in a P picture where every other
   macroblock is inter, an intra macroblock then has no available neighbours and DC falls back to
   1 << (BitDepth - 1). So the idea works, but it costs a PPS flag that changes the prediction of every intra
   macroblock in every P picture, and that cost has to be measured, not assumed. The sweep's second route
   (constrained intra prediction) is in fact the same route as its first.
4. **Missed primary source: the standard itself.** Levers Q1 and Q2 need the predictor the bitstream actually
   pays for, which is the median predictor of ITU-T H.264 clause 8.4.1.3 together with the 16x16 special cases,
   and lever Q5 needs 7.4.2.2 for `constrained_intra_pred_flag`. The sweep builds the predictor-relative cost
   argument entirely from encoders, so it never pins down which predictor our rate term must match. If the
   shader's predictor differs from the one `h264_cavlc.cpp` uses when it writes the MVD, the rate term will be
   wrong in exactly the macroblocks where it matters most.
5. **Missed by the brief's own scope: NVIDIA.** The brief asked for "NVENC/AMF/VCN design notes". S3 covers
   Intel and S14 covers AMD; nothing in the sweep covers NVIDIA's Video Codec SDK. Not important for a design
   decision, but the gap is not declared.
6. **S1 shares our cost-model defect**, as noted under S1. Worth stating in the sweep, because S1 is the only
   source measured on our own silicon and a reader may assume it endorses every lever.

---

## 4. What changes in the ranked lever list

Nothing is struck. Four adjustments:

- **Q1 and Q2 keep their rank.** Three independent encoders and one hardware specification agree. Add the
  standard's clause 8.4.1.3 as the definition of the predictor to use, and drop S1 from the evidence for Q1.
- **P1 drops from "the largest single speed lever" to "a lever worth one measurement".** The published saving
  is per partition size, against a 16-point JM search, in an encoder with seven partition sizes; our 18
  candidates in a 16x16-only encoder are a different quantity. The closed-form fit is still nearly free and the
  group shared memory saving is still real, so the work is still worth doing first, but the expected size is
  not established by S6.
- **C3 is re-based on CAVLCU, not on S10.** CAVLCU is 2.5 to 6.7 times faster than the S10 design it
  reimplements, and its first finding is that the multi-kernel structure S10 uses is itself the cost. This also
  promotes the DXC and `cs_6_0` question (open question 4) from an optimisation to a prerequisite, because
  CAVLCU's inter-thread-block synchronisation is not expressible safely in `cs_5_0`.
- **T1's ordering constraint is softer than stated.** With the corrected LDS total of 9068 bytes, a packed
  58x58 integer window at 3.4 KB would take the group to about 12.5 KB and 5 resident groups rather than 7, so
  it can be tried before the sub-pel windows shrink, at a measurable occupancy cost. The sweep's preferred
  order (shrink first) is still the better bet; it is no longer a hard dependency.

Open questions 1, 2, 3 and 4 of the sweep stand. Open question 5 is closed: the paper is readable and has been
read.
