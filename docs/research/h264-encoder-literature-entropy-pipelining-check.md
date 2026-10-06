# Verification of h264-encoder-literature-entropy-pipelining.md

Date: 2026-10-06. Checker: skeptical re-read of every source in
`research/h264-encoder-literature-entropy-pipelining.md` at the source, plus the project's own primary evidence for every claim the
sweep made about our encoder.

Scope of the check, per the brief: (1) is the citation real and correct, (2) does each quoted sentence
appear verbatim at the stated place, (3) do the numbers and their conditions match, (4) is the claim
attributed to the source what the source actually says.

Verdict counts: **11 VERIFIED, 4 CORRECTED, 0 REJECTED** among S1-S15. Two of the sweep's own
derivations are **REJECTED**: lever L1's premise, and the chroma-QP hypothesis in its section 3.

---

## 0. The headline: the sweep's central measurement reading is wrong, and it inverts the ranking

The sweep built lever L1 ("Fix the readback path", ranked highest value) on this inference:

> "Our readback is **7.57 ms for 3.4 MB of staging at 1080p**, about 0.5 GB/s, which is the same
> signature."
> "readback 7.57 ms of which about 1 ms is the wait, so about **6.5 ms of transfer and copy** for
> 3.4 MB, roughly **0.5 GB/s**, which is write-combined-read territory, not cached-RAM territory."

**This is backwards, and the lab console the sweep cited says so in one line it did not read.**

`lab-e52/out-20261006T081819Z/20261006T081819Z/t1080-60.txt`, line 12:

```
  per picture: command recording 0.25 ms, map wait 6.89 ms, readback transfer 0.68 ms
```

and `t720-60.txt`, same line:

```
  per picture: command recording 0.14 ms, map wait 3.93 ms, readback transfer 0.32 ms
```

The writer's own source confirms the semantics. `perf-wt/driver/umd/mft-h264/src/gpu_pipeline.cpp`
around line 990:

> "The copies were recorded by Submit; the first Map blocks until they have executed on the GPU, and
> the two memcpy calls move the result into the caller's storage. So this block holds the whole wait
> for the GPU plus the cost of the transfer"

and `gpu_pipeline.h`:

> "Wall-clock cost of moving the last picture's levels and macroblock info to the CPU: the two
> CopyResource calls, the two blocking Map calls and the two memcpy calls. This is where the CPU
> waits for the GPU, so it is the one number that says whether the pipeline is GPU bound."

So the 7.57 ms "readback" column is **6.89 ms of waiting for the GPU plus 0.68 ms of transfer and
memcpy**. 3.4 MB in 0.68 ms is about **5.0 GB/s**, not 0.5 GB/s. At 720p, 0.32 ms for about 1.5 MB is
about 4.7 GB/s. That is ordinary cached-RAM behaviour, an order of magnitude away from S7's
write-combined pathology. The 0.5 GB/s figure is wrong by a factor of about ten.

LAB-B19-RESULT.md itself says the same thing in words the sweep read and then reversed: "GPU work
(3.6 / 6.5 ms) plus **about 1 ms of submission and readback wait** plus CAVLC on the CPU". The 1 ms is
the readback's own cost, not the wait.

Second, the sweep's "one measurement inconsistency to settle" (pipelined 4.55 ms below GPU busy
6.53 ms) is settled by the same files, and the answer matters more than the inconsistency:

```
  pipelined pass: depth 2, the same 60 pictures
    4.59 ms per picture, 6.52 ms of it on the GPU, 218.1 pictures per second
    per picture: record 0.37 ms, map wait 0.00 ms, readback 0.51 ms, cpu 3.39 ms
    60 pictures retired, 50 of them timed; the test spent 5.55 ms a picture making them,
    which the clock above leaves out
```

(720p: 2.46 ms per picture, GPU 3.61 ms, test spent 2.14 ms a picture making sources.)

The pipelined figure **excludes 5.55 ms a picture of test-side source generation**, during which the
GPU was running. Real wall time is about 10.1 ms a picture at 1080p and about 4.6 ms at 720p, both of
which comfortably contain the GPU stage. So:

- the GPU stage, not CAVLC and not the readback, is the **steady-state bound**: about 6.5 ms at 1080p
  (about 153/s ceiling) and about 3.6 ms at 720p (about 277/s ceiling);
- `map wait 0.00 ms` in the pipelined pass means **the CPU never waited for the GPU**. The entropy
  coding is already fully hidden. Hiding it harder buys nothing;
- the published 219.8/s and 363.3/s pipelined rates are not sustainable throughput for a real source.

**Consequence for the sweep's ranking.** Of its five active levers, four target work that is already
off the critical path:

| Lever | Sweep rank | Verified expected gain in the shipped (depth 2) shape |
|---|---|---|
| L1 readback memory type and streaming copy | 1st, "several ms" | **about 0.5 ms at best, and the premise is false**: the copy already runs at ~5 GB/s and `Collect` already memcpys once into `std::vector` (`gpu_pipeline.cpp` lines 1010 and 1016), which is S7's shadow-copy half of the fix |
| L2 per-row proactive returns | 2nd, "a few ms" | **about 0** while `map wait` is already 0.00 ms |
| L3 CAVLC on 4 CPU slices | 3rd, "2.4 ms serial" | **0** in the pipelined shape; 2.4 ms on the serial path only, and it costs bitstream compatibility and the bit-exact anchor |
| L4 CAVLC on the GPU | 4th, "do not start" | confirmed: do not start |
| L5 cs_me workgroup 32 -> 128 | 5th | **the only lever on the bound**. Should be first |

L5 is first, L1 and L2 are small clean-ups worth doing only because they are free of bitstream risk,
and L3 and L4 should not be started at all until the GPU half is under 3 ms at 1080p. The sweep's own
fifth executive-summary point reached L5; its ranking then buried it.

One further caution on L5 that the sweep did not raise: at 128 threads a group, 7 groups per 64 kB CU
give 28 waves per CU, 14 per SIMD32, which needs at most 1024/14 = 73 VGPRs a wave. `cs_me`'s register
count is unmeasured, so the 4x gain is an upper bound set by LDS and may be cut by registers. Measure
the VGPR count from the compiled shader before predicting a factor.

---

## 1. Source-by-source verdicts

### S1. CAVLCU (Fuentes-Alventosa et al., J. Supercomput. 78(6):7556-7590, 2022) - **CORRECTED**

Citation **exact**. Crossref record for `10.1007/s11227-021-04183-8` returns the title, all five
authors, The Journal of Supercomputing, volume 78, issue 6, pages 7556-7590, 2022, online 29 Nov 2021.

The sweep's route to the full text is sound and reproducible. `helvia.uco.es` needs the cookie step
first, then the PDF; the file is 9,021,171 bytes as stated, 112 pages.

Quotes checked in the thesis reproduction:

- the four key ideas (one kernel / synchronization mechanism / vectorized loads / register tiling):
  **verbatim**.
- "The dimensions of the thread-blocks are 4 x 4 x REG_SIZE, where REG_SIZE is the number of MBs of
  each region": **verbatim** (the source renders the multiplication signs as the symbol, not "x").
- "the i-th MB of the region is assigned to the i-th half-warp of the thread-block, and the i-th block
  of a MB is encoded by the i-th thread of the corresponding half-warp": **verbatim**.
- the `__shfl_up` / `d_info_A` passage: **verbatim**.
- the `atomicExch` look-back passage: **verbatim**.
- the `word_val` / `word_len` passage: **verbatim**.

**Correction 1 (the abstract quote).** The sweep prints as one abstract sentence: "our approach is
between 2.5x and 5.4x faster than the only state-of-the-art GPU-based implementation of CAVLC". The
source has two separate sentences: the contributions list says "comparison of our implementation with
the only existing state-of-the-art GPGPU implementation [38, 39]. An exhaustive experimental
evaluation showed that our solution is between 2.5x and 5.4x faster than the state-of-the-art
implementation [38, 39]", and section 3 opens "The only state-of-the-art GPU-based implementation of
CAVLC is the solution presented by Su et al.". The sweep's sentence is a stitch of the two. The
substance is correct.

**Correction 2 (a condition the sweep omitted, and it matters).** The baseline CAVLC_SU is **not Su et
al.'s own binary**: "We implemented CAVLC_SU from scratch following the description of the algorithm
given by their authors [38, 39] and their support through private communication with Huayou Su."
So CAVLCU's 2.5-5.4x is against a reimplementation. This **strengthens** the sweep's own warning that
S1 gives no absolute ms budget: there is no chain at all from S4's measured 105 fps to a CAVLCU
runtime on our part.

Numbers and conditions, all **confirmed**: GTX 970 (Maxwell cc 5.2) and RTX 2080 (Turing cc 7.5);
first 50 frames of City (QCIF), Mother and Daughter (CIF), Ducks take off (720p); GOP 10; 11 QP values
0 to 50; "The number of threads per thread-block was 128 in all cases; hence, the value of the
parameter REG_SIZE of CAVLCU was 8". Table 10 verbatim: Maxwell min 2.5 max 5.4, averages 3.3 / 3.3 /
4.1; Turing min 3.0 max 6.7, averages 5.2 / 4.8 / 5.1 - the sweep's "per-clip averages 3.3 to 4.1 on
Maxwell, 4.8 to 5.2 on Turing" is right. Table 11 improvements 4.32 / 4.26 / 4.39 and 2.56 / 2.55 /
2.70 and 2.30 / 2.45 / 2.25 / 2.17 / 2.30 / 2.13 match the sweep's ranges.

The sweep's claim "the paper reports no absolute ms-per-frame budget in its text (the runtimes live
only in Figures 15-17)" is **correct**.

### S2. GVLE (Fuentes-Alventosa et al., J. Supercomput., 2022) - **VERIFIED**

Citation real; DOI `10.1007/s11227-022-04994-3`; code repository `github.com/z12fuala/GVLE` exists.

All four quotes **verbatim**, including "a novel inter-block scan method, which outperforms those of
state-of-the-art solutions"; "Given an element d_scan[i], if the flag A is set..."; "To avoid race
conditions with the previous and next thread-codes, the first and last writes are performed by using
atomic OR operations"; and the 32-versus-1 look-back comparison ending "This optimization is the
unique reason of the significant speedup."

The abstract sentence the sweep gives as "on average 2.6x faster than the best state-of-the-art
implementation" is **verbatim** (the source writes "2.6 x" with the multiplication symbol).

Every number checked against the tables and **all match**, which is unusual and worth saying plainly:

- Table 6 (GVLE over YAVLE): bank conflicts 13,839,817 -> 3,170,669 (4.36x); global loads
  60,261,119 -> 6,721,148 (8.97x); global reduction transactions 4,307,361 -> 205,459 (20.96x);
  executed instructions 1,319,851.51 -> 517,852.75 (2.55x); runtime 1.30 -> 0.50 ms (2.57x).
- Table 7: GVLE 0.50 (0.42 / 0.53), YAVLE 1.30, CUVLE 6.85, CPU_VLE 191.75; 13.63x and 377.15x.
- Table 8: GVLE_scan 1.22, YAVLE_scan 1.97 (1.62x), CUVLE_scan 47.13 (38.32x).
- The ladder: 1.21x (Table 2, bank conflicts in the codeword table), 1.53x (Table 3, global memory),
  1.23x (Table 4, register space), 1.14x (Table 5, inter-block scan).
- Platform: 3.50 GHz Core i7-7800X, 32 GB, RTX 2080, CUDA 11.1, driver 512.15, -O3, one warm-up plus
  fifty timed iterations. Corpus: 11 Standard plus 3 Large Canterbury files, each replicated to "the
  final size greater than or equal to 100 megabytes".

The sweep's derived sanity check "100 MB in 0.50 ms on an RTX 2080 is about 200 GB/s" is arithmetically
right. Note for anyone reusing it: that counts the input pass only, so the real traffic is higher.

### S3. The thesis (Fuentes-Alventosa, Univ. Cordoba, 2023, handle 10396/25270) - **VERIFIED**

Handle, filename `2023000002649.pdf` and byte count 9,021,171 all exact. The cookie workaround the
sweep documented works as written. Both Spanish quotes appear verbatim ("es entre 2.5x y 5.4x mas
rapido que la mejor implementacion anterior en GPU de CAVLC"; the scan paragraph ending "como la
propia operacion scan y el algoritmo de compactacion"). The thesis does reproduce both papers with
their journal pagination.

### S4. Su et al., Sci. World J. 2014:716020 - **VERIFIED**

Citation, authors, DOI `10.1155/2014/716020` and PMC3976889 all correct.

Every quoted sentence appears **verbatim**, including the two the sweep flagged as warnings:

> "because the parallel degree of the most time consuming kernel (bit_pact) of CAVLC is relatively
> small and decreases with the kernel execution"
> "the computation-accessing-ratio of CAVLC is relatively low; the performance of the proposed CAVLC is
> majorly determined by the bandwidth of the GPU"
> "It should be noticed that the CAVLC achieves a very high performance on the CPU used in this paper
> due to its high frequency and big cache size."
> "the memory copy time consists of about 25% even."

Table 5 confirmed: "Component-based CAVLC | x264 | 720p | CAVLC | 8 | 105 (for CAVLC)" on GTX260, and
"The proposed H.264 | x264 | 720p | Application | 13~17 | 32.3" on C2050. Platform: Alienware
Aurora-R3, Core i7-2600 quad-core 3.4 GHz, CUDA 4.2, three GPUs. The sweep's arithmetic (105 fps =
9.5 ms a picture) is right, and the GTX260 is indeed a 2008 part.

The sweep's AsAP comparison row ("4.86x, 36-41.3 fps") was not located in the extracted text; treat it
as unconfirmed. It carries no weight in any conclusion.

One useful extra condition the sweep did not quote, which makes its "against L4" case stronger: "When
compared with the performance on another CPU, Intel E8200, the speedup ratio of CAVLC can be 46, 4
times higher than the speedup on Intel CPU i7-2600." The GPU-over-CPU ratio for CAVLC is almost
entirely a statement about which CPU you compare to. Our comparison CPU is a Zen 2.

### S5. Yamamoto et al., ICPP '20 - **CORRECTED (partially verified)**

Citation **exact**: Crossref `10.1145/3404397.3404429` returns "Huffman Coding with Gap Arrays for GPU
Acceleration", authors Naoya Yamamoto, Koji Nakano, Yasuaki Ito, Daisuke Takafuji, Akihiko Kasagi,
Tsuguchika Tabaru, ICPP '20: 49th International Conference on Parallel Processing, 2020-08-17, pages
1-11. The code repository `github.com/daisuke-takafuji/Huffman_coding_Gap_arrays` exists.

ACM full text is 403 for us as well, so the sweep's workaround is accepted. Abstract content confirmed
through two independent indexes: SKSS, wordwise global memory access, the gap array, and "GPU Huffman
encoding and decoding run 2.87x-7.70x times and 1.26x-2.63x times faster than previously presented GPU
Huffman encoding and decoding" on an NVIDIA Tesla V100.

**Corrections.** Two items in the sweep's S5 entry are **not verified at any source**: the "Best Paper
Award", and the "further 1.67x to 6450x with a gap array" decoding figure. Drop both or cite them. The
third abstract item, "compact codebooks", is also unconfirmed. The S2-derived numbers for this
implementation (YAVLE 1.30 ms, YAVLE_scan 1.97 ms) are confirmed in S2 and are the ones to rely on.

### S6. Sze et al., ICIP 2008 - **VERIFIED**

Title, four authors, affiliations (MIT and Texas Instruments) all correct at
`eems.mit.edu/wp-content/uploads/2023/08/vsze_icip2008_paper.pdf`.

Every quote **verbatim**, including the one the sweep did not flag as uncertain and which does appear:
"Arithmetic coding is inherently serial due to strong data dependencies, and typically only a single
symbol is coded at a time. Consequently, the AC engine is often the bottleneck in the codec". The
abstract's "~2x throughput improvement at a cost of 0.76% average increase in bit-rate or equivalently
a decrease in average PSNR of 0.025dB on five 720p resolution video clips" is verbatim, and Table 6
backs it per clip (BigShips 0.67 %, City 0.73 %, Crew 0.80 %, Night 0.80 %, ShuttleStart 0.81 %,
average 0.76 %, -0.025 dB). The 9-14 % CABAC-over-CAVLC figure is verbatim.

**Flag, not a correction to the source.** The sweep adds: "The authors' later work on massively
parallel CABAC reports 2.7x to 32.8x bins per cycle at 0.25 % to 6.84 % coding loss." Neither 32.8 nor
6.84 appears in this paper, and no citation is given for the later work. It is an uncited claim and
should either be sourced (Sze and Budagavi's massively-parallel CABAC line) or struck. It does not
affect the conclusion.

The sweep's use of S6 is sound and its strongest move in the whole document: CABAC's 9-14 % bitrate
advantage cannot explain a 3.8-6.0 dB chroma deficit, so CABAC is not the T3 fix.

### S7. bc250-vaapi / bc250-encoding-decoding-fix - **CORRECTED**

Repository exists at `github.com/MTSistemi/bc250-vaapi` and at
`github.com/simpmix/bc250-encoding-decoding-fix`.

**Licence trap confirmed and tightened.** `LICENSE` opens
`SPDX-License-Identifier: GPL-3.0-only`, with an explicit "there is no 'or any later version' grant".
The README confirms GPL-2.0 for the audio kernel module. The sweep's handling (read-only reference,
facts and ideas only, `__WARN-GPL3-no-code-in-our-driver` naming if mirrored) is correct and should be
kept exactly as written. GitHub's API reports the licence as NOASSERTION, so do not rely on the API.

Every quote **verbatim** at the stated file: the VCN sentence, the architecture sentence, the encode
rates, the three environment-variable rows, the Chroma Fidelity line, the
`find_memory_type_preferred()` 81 ms / 10.6 MB passage, the `shadow_copy()` 2x2 table with its
319.3 / 3.0 and 857.1 / 5.6 ms figures and the "Removing either one regresses" sentence, the
44-63 us and 36-38 us per-macroblock diagnosis, the `MOVNTDQA` comment, the compact DC buffer passage
with its 44.2 / 22.1 / 22.1 MB figures, the OpenMP pragma (lines 3032 and 3609), the ">= 720p default
to 4 slices" comment (line 1860), the queue-priority passage, and the dead GPU-entropy path comment
(line 406). The 72-line `entropy_encode.comp` is as described: `local_size_x = 32`, a symbol pre-pass
only, no bitstream writer.

Two sweep sentences are near-misses on wording, both harmless: "the original ~860ms/frame pathology"
is verbatim but split across lines, and "this changes WHERE the CPU reads a byte from, never WHAT byte
it reads" is verbatim in the `shadow_copy()` comment.

**Correction 1, and it is the most important fact in this source.** The sweep omits the README's own
default:

> "`BC250_H264_BACKEND` | `x264` | `x264` uses Zen 2 CPU offload (fastest, leaves GPU free for games).
> Set to `compute` for GPU compute ME + Dynamic Governor."

and the codec matrix, which lists H.264 encode acceleration as "libx264 (compute encoder as
fallback)" for Baseline, Main and High. **The only other team working this exact silicon ships the Zen
2 CPU encoder as its H.264 default and treats its GPU compute encoder as a fallback.** That is
independent confirmation of LAB-B19's finding that the inbox CPU encoder beats our compute path, and it
belongs in any document that ranks our levers. Their GPU compute path is the one they kept for HEVC,
where there is no libx265-class default in their matrix.

**Correction 2.** The sweep writes: "Their H.264 1080p compute-encode rate is 100-134 fps against our
219.8/s pipelined. We are already ahead of the only comparable implementation on this hardware." The
comparison is invalid in both directions. Their number is a whole-driver rate on real frames; our
219.8/s excludes 5.55 ms a picture of test-side work (section 0 above). Our honest comparables are
86.9/s serial and an approximate 153/s GPU-stage ceiling, so we are **behind to level with** their
100-134 fps, not ahead.

**Correction 3.** The sweep's lever L1 treats our readback as "the same signature" as theirs. It is
not: their pathology is 10.6 MB a frame read at roughly 0.13 GB/s; ours is 3.4 MB at about 5.0 GB/s.
Their shadow-copy half of the fix is **already implemented in our encoder** (`gpu_pipeline.cpp` 1010
and 1016 memcpy out of the mapped staging into `std::vector` before any per-macroblock read). Their
memory-type half has no D3D11 application-level equivalent: `CreateStaging` sets only
`D3D11_USAGE_STAGING` with `D3D11_CPU_ACCESS_READ` and the driver picks the memory type. On unit A that
driver is ours, so if this is ever worth pursuing it is a KMD or UMD allocation-policy question, not an
MFT change. At 5 GB/s there is nothing to pursue.

**Correction 4, minor.** The sweep's "4 slices per frame with one OpenMP thread per slice as its
default at 720p and above" is right in substance, but the gate in the code is `total_mbs >= 1000`
(about 720p), not a resolution test, and the Steam Link override forces 1 slice unless
`BC250_FORCE_SLICES=1`.

### S8. x264 `doc/threads.txt` - **VERIFIED**

Read the raw upstream file (5,387 bytes). Every quote **verbatim**, in order, including the whole
six-item percentage breakdown with its parenthetical on deblocking, and the "none of the proportions
should depend strongly on the number of slices" paragraph. Conditions verbatim: "8core Nehalem (2x
E5520) 2.27GHz, hyperthreading disabled", "linux 2.6.34.7, 64-bit", "x264: r1732 b20059aa",
`park_joy_1080p.y4m`. The sweep's table is correct to the digit (2: 1.41x / 2.29x, -0.005 / -0.002;
4: 1.96x / 3.97x, -0.029 / -0.001; 8: 2.43x / 3.98x, -0.067 / -0.001; medium at 8: 3.79x, -0.015;
slower at 8: 4.13x, -0.026).

Two separation points the sweep states correctly and that a reader could miss: the +30 % figure is a
different experiment from the benchmark table (720p, 45 slices, 4 movies, crf20 and crf30), and the
table is `--tune psnr --crf 30` at three presets.

**One inference overreaches.** The sweep writes: "16 % of it is 'reset cabac contexts', and 2 % 'cabac
neighbors', which do not apply to a CAVLC stream at all." The two items are indeed CABAC-specific, so
subtracting them is right. But the sentence then implies a CAVLC encode pays nothing in their place,
and that is false: a slice boundary also resets CAVLC's own neighbour context, the `nC` of
`coeff_token`, which is derived from the left and top blocks' `TotalCoeff`, and resets `mb_skip_run`.
Our `SliceWriter::NcLuma` (line 96 of `h264_cavlc.cpp`) reads exactly those neighbours and would
return 0 at a slice edge. x264's list does not price that cost because it measured a CABAC encode. So
the correct statement is: CAVLC avoids 18 % of x264's measured cost and pays an unmeasured `nC`-reset
cost in its place. Also note the 34 % "intra prediction" item is largely inapplicable to our P
pictures, which have no intra macroblocks at all - which makes our slice cost lower again, for a
reason that is itself a T3 defect.

### S9. cuSZ, PACT 2020, arXiv 2007.09625 - **VERIFIED**

Title, eleven authors, venue and arXiv id correct; the PDF carries `doi.org/10.1145/3410463.3414624`
and the PACT '20 virtual-event header.

Every quote **verbatim**, including the deflating definition, the "about 250 GB/s for uint64_t and
about 380 GB/s for uint32_t ... the performance of deflating is about 60 GB/s, which is lower than the
encoding throughput of 380 GB/s. Consequently, the Huffman coding performance is bounded mainly by the
deflating throughput", the "around 2e4 concurrent threads" observation, and the memory-reuse sentence.
Conditions confirmed: V100; HACC, CESM-ATM, Hurricane, Nyx, QMCPACK; 111 fields; error bound 1e-4;
Table 3 codebook construction total 0.68 ms at 128 quantisation bins rising to 50.71 ms at 8192, on
the Hurricane Isabel dataset.

The sweep's derived figures are right: 380/60 = 6.3x; 1080p is 8160 macroblocks (confirmed by
`t1080-60.txt`: "120x68 macroblocks") and 8160 x 24 = 195,840 4x4 blocks, so one thread a block is an
order of magnitude past the 2e4 optimum and one thread a macroblock is just under it.

### S10. AMD "RDNA Architecture" public deck - **CORRECTED**

Document real at `gpuopen.com/download/RDNA_Architecture_public.pdf`. Every quoted line **verbatim**:
the LDS slide ("128 kB per workgroup processor", "Up to 64 kB per workgroup", "up to 32 dwords per
cycle (doubled relative to GCN)", "32 banks", "Mind the bank conflicts!"), the register slide ("Each
SIMD32 has 1024 physical registers", "16x Wave32 with 64 VGPRs", the threads-per-SIMD-lane
equivalence), and "Workgroup size: keep it a multiple of 64".

**Correction.** The sweep's executive summary says RDNA allows "up to **16 wave32 per SIMD**". The
deck's "16x Wave32 with 64 VGPRs" is one entry in a list of **register-budget examples** (alongside
"4x Wave32 with 256 VGPRs"), not a hardware wave-slot limit. On RDNA 1, which is what gfx1013 is, the
hardware limit is **20 wave32 slots per SIMD32** (RDNA 2 and later reduced it to 16). Against 20 the
sweep's "near 20 %" becomes about 16 %, which only sharpens its point. For the real authority use the
RDNA 1.0 Instruction Set Architecture document, not the marketing deck.

**Correction to the arithmetic, and to the file it is about.** Two things:

1. The arrays the sweep lists (`gInt[23*23]`, `gBRaw[23*18]`, `gH[18*18]`, `gJ[18*18]`, `gPart[9*32]`)
   are **not** in `bc250-win/driver/umd/mft-h264/shaders/cs_me.hlsl`, the path the sweep cites. That
   file holds only `gSrcMb[256]`, `gCost/gSad/gCandX/gCandY[32]` and four scalars: **1552 bytes**. The
   arrays are in the unmerged perf worktree,
   `scratch/m15/video-encode/perf-wt/driver/umd/mft-h264/shaders/cs_me.hlsl`, which is the e52 build
   that produced the b19 numbers. Any brief that acts on L5 must name that path or it will patch the
   wrong shader.
2. Counted exactly from that file, the group shared total is 2,267 dwords = **9,068 bytes**, not
   9.8 kB. The 9.8 kB figure comes from the project's own README ("raised cs_me's group shared memory
   from 1552 bytes to about 9.8 KB per 32-thread group") and is a rounding. The sweep also divides by
   the 128 kB WGP figure while the deck's own "Up to 64 kB per workgroup" and the per-CU split mean a
   group's LDS lives in one 64 kB half: floor(65536/9068) = 7 groups a CU, 14 a WGP, about 3.5 waves a
   SIMD32. The project README reached 6 a CU with the 9.8 kB figure. All three roads give 15 to 22 % of
   the wave slots, so the conclusion stands; the stated numbers do not.

### S11. AMD RDNA Performance Guide - **VERIFIED**

All eight quoted lines present at `gpuopen.com/learn/rdna-performance-guide/` with the stated wording.
One truncation: the barrier line is "Batch groups of barriers into a single call to reduce overhead of
barriers"; the sweep ends it at "overhead."

### S12. Dunn and Hodes, "Asynchronous Compute Deep Dive", GDC 2017 - **VERIFIED**

Deck real, 29 slides. Every quoted line **verbatim**, including the deck's own typo "simultanesouly",
the resource-contention list, "Try limiting occupancy by allocating dummy LDS", the four async-tax
items, "First: determine if CPU or GPU is the bottleneck (GPUView)", and both DxKrnl event names.

**Condition the sweep should have stated.** The hardware slide is **GCN**, not RDNA: "4 SIMD per CU",
"Up to 10 Wavefronts scheduled per SIMD", and the VGPR-to-max-waves table is labelled GCN. RDNA's
shape is different (2 SIMD32 a CU, 4 a WGP, 20 wave32 slots a SIMD32 on RDNA 1). The sweep uses S12
only as authority for "LDS is one of the two occupancy limiters" and for the async tax, and does not
mix the numbers into its arithmetic, so the use is sound - but the deck must not be cited for
occupancy counts on our part.

### S13. GDeflate Internet-Draft - **CORRECTED**

Draft real: `draft-uralsky-gdeflate-00`, "GDEFLATE bitstream specification", NVIDIA Corporation,
expired individual submission, "Expires 8 January 2025". Version -00 is the only version.

**Correction.** The author is **Y. Uralsky** (Yury Uralsky), not "E. Uralsky".

Quotes **verbatim** at the stated sections, with the sweep's ellipses matching real line breaks:
section 5.1 "To enable parallel parsing of the bit stream, GDeflate splits the original sequence of
variable-length codes into 32 independent sub-streams. Each sub-stream is assigned to a fixed SIMD
lane, so all lanes in the SIMD group can collectively parse all 32 sub-streams in parallel."; "GDeflate
targets SIMD width of 32, which aligns well with most common CPU and GPU architectures today and in
foreseeable future."; "The SIMD group is assumed to comprise 32 parallel 'lanes', even though the
physical SIMD width of the underlying implementation may be wider or narrower."; section headings 5,
5.1 and 5.2 as given.

The sweep's conclusion - that we cannot use this because the H.264 slice RBSP has a normative bit
order, and that slices are the standard's own sanctioned equivalent - is correct and is the right
reason to keep the source in the list.

### S14. Lustig and Martonosi, HPCA 2013 - **CORRECTED**

Paper real at `mrmgroup.cs.princeton.edu/papers/dlustigHPCA13.pdf`; title, both authors and Princeton
affiliation correct. The venue and year are not printed on this author copy; they are correct per the
published record.

Quotes **verbatim** (the earlier misses were ligature extraction, not absence): the "performance
benefits of offloading are hindered" sentence, "early kernel launch and proactive data returns", "the
mean improvement in runtime is 26%", "Of the different strategies, the full-overlap scenario provides
an average of 26% speedup", "Driver delays, uncertainty about data arrival times, and coarse-grained
synchronization each add latency overheads regardless of the placement of the GPU relative to the
CPU", and the Figure 2 caption on the GTX 580 and A8-3870K.

**Correction.** The sweep writes "We then propose a **set** scheme of full-empty bits". The paper says
"We then propose a scheme of full-empty bits to track when regions of data have been transferred." Drop
the stray "set".

The sweep's framing is honest: this is a simulated hardware proposal (full/empty bits in the GPU memory
controller) plus real-system characterisation, and only the software shape transfers. Its 26 % is for
the full-overlap strategy specifically.

### S15. elektricM/amd-bc250-docs - **CORRECTED: the open question is closed, against the search summary**

The sweep left this as a contradiction to be checked later. Checked. The documentation site says:

> "VCN (Video Core Next): Firmware blocked by Sony - hardware exists but cannot be used"
> "Hardware video encode/decode will not work because the required VCN firmware is missing."
> "Software decoding via CPU is the only option"

So there is **no contradiction with S7** on the outcome: both say the block is unusable. They differ
only on mechanism (S7: eFused off at the factory; S15: firmware blocked, hardware present), and
`m2jgh8tg7r-bot/bc250-vcn-linux-research` (GPL-3.0, exists) is consistent with S15's version. The claim
that "As of Mesa 25.1 and kernel 6.11 the VCN block is enabled" came from a search summary and is **not
in the primary source**. The sweep was right to refuse to act on it, and the bounded check it proposed
can be closed: no Windows-side VCN enumeration is needed on this evidence.

---

## 2. The sweep's own derivations, checked against our code and evidence

### REJECTED: lever L1's premise

Covered in section 0. The copy runs at about 5.0 GB/s, `Collect` already shadow-copies, and D3D11 gives
the application no memory-type choice. Expected gain is about 0.5 ms on the serial path and
approximately 0 in the shipped shape.

### REJECTED: the chroma-QP hypothesis in the sweep's section 3

The sweep proposes, from S7's "Bit-exact non-linear Table 8-10 QP mapping eliminates the standard
chroma PSNR deficit", that we may be using a linear chroma-QP mapping or missing an offset, and calls
it "testable without any lab time".

Tested. **We already implement it.** `src/encoder.cpp` line 38, `Encoder::ChromaQpFromLuma`:
`qPi = qpY + chromaQpIndexOffset`, clamped to 0..51, identity below 30, then
`kChromaQpFromQpi30[qPi - 30]`; `src/h264_tables.cpp` line 164 is commented "Table 8-15, QPc for qPi
30..51" and `encoder.h` cites clause 7.4.2.2. That is the H.264 non-linear derivation with the PPS
offset, which is the correct analogue of the HEVC table S7 names. The chroma deficit has another cause.

One condition the sweep should have carried into any quality discussion: the compare runs are not at
equal bits. `compare-1080-60.txt` shows 3,089,727 bytes for us against 3,320,028 for the inbox (+7.5 %)
and 1,542,203 against 1,607,210 at 720p (+4.2 %). Part of the dB gap is a bitrate gap. Also, the
per-picture breakdown shows our chroma is **better** on picture 0 (Cb +3.08, Cr +2.81 dB at 1080p) and
only loses from picture 1 on, which points at the P-picture path, not at a quantiser table applied to
every picture.

### Verified: the internal-code quotes, with a path correction

Every `gpu_pipeline.h` and `gpu_pipeline.cpp` sentence the sweep quotes is **verbatim** - but in
`scratch/m15/video-encode/perf-wt/driver/umd/mft-h264/`, not in `driver/umd/mft-h264/` as cited. The
main-tree `gpu_pipeline.h` has no pipeline slots at all. Locations in the perf worktree:
`gpu_pipeline.h` line 126 (the 440 dispatches and the instrumented-total warning), line 141 (depth 2
covers the entropy coding of one picture with the next picture's GPU work), line 143 (3.4 MB a slot),
line 162 (one query copy a slot); `gpu_pipeline.cpp` line 521 (3.4 MB at 1080p). The CAVLC neighbour
claim is right: `SliceWriter::NcLuma` at line 96 of `h264_cavlc.cpp` reads `m_nnzY` of the left and top
4x4 block, and `MvPred` at line 136 reads neighbour motion vectors.

---

## 3. Primary sources the sweep missed

1. **`lab-e52/out-20261006T081819Z/20261006T081819Z/t1080-60.txt` and `t720-60.txt`, line 12 and the
   pipelined block.** The single most important source for this question, inside the archive the sweep
   already cited. It gives `map wait`, `readback transfer` and the test's own source-generation cost,
   and it refutes L1 and resolves the inconsistency the sweep left open. A log counter's semantics come
   from the writer's source; this is that lesson again.
2. **`perf-wt/driver/umd/mft-h264/README.md`, "What is not done yet".** The project had already written
   down the LDS occupancy finding ("about 9.8 KB per 32-thread group, which on a 64 KB compute unit
   caps six groups"), the 4090-to-unit-A extrapolation, and the quality causes. The sweep rederived the
   first from a marketing deck and got a different number.
3. **S7's README default `BC250_H264_BACKEND=x264`** and the codec matrix's "libx264 (compute encoder
   as fallback)". The sweep read this file and omitted its most relevant line.
4. **`src/encoder.cpp` `ChromaQpFromLuma` and `h264_tables.cpp` Table 8-15.** Closes the sweep's
   section 3 without a lab run.
5. **Sze and Budagavi's massively-parallel CABAC work** - the uncited "later work" behind the
   2.7-32.8x and 0.25-6.84 % figures. Cite it or strike the sentence.
6. **The AMD RDNA 1.0 Instruction Set Architecture document** - the authority for wave slots per SIMD32
   and LDS allocation granularity on gfx1013, in place of the RDNA public deck and the GCN-era GDC 2017
   deck.
7. **Chi, Alvarez-Mesa, Juurlink et al., Overlapped Wavefront (IEEE TCSVT 22(12), 2012)** - still
   unread, still the one named prior art for L2. Since L2's measured headroom is now about zero, it has
   dropped from "worth one more attempt" to "not worth the fetch".

---

## 4. What to do with the sweep

Keep it. Its source work is unusually solid: 11 of 15 sources clean, every long quotation verbatim,
every table number in S1, S2, S4, S6, S8, S9 correct to the digit, and the licence trap on S7 handled
properly. It was also right on its two hardest judgement calls, S6 (CABAC is not the T3 fix) and S13
(sub-stream rearrangement is illegal for us).

What it got wrong is one measurement reading, and that reading set its whole ranking. Fix these four
things before anyone acts on it:

1. Replace the readback paragraph and lever L1 with the measured `map wait` / `readback transfer`
   split, and move L5 (`cs_me` occupancy, in the **perf worktree** path) to the top of the ranking.
2. State that at depth 2 the entropy coding is already fully hidden (`map wait 0.00 ms`), so L2, L3 and
   L4 have about zero expected gain in the shipped shape, and that the pipelined rates exclude 5.55 ms
   (1080p) and 2.14 ms (720p) a picture of test-side work.
3. Correct the S7 comparison: their default H.264 backend is the Zen 2 CPU, and our comparable rate is
   86.9/s serial or about a 153/s GPU-stage ceiling, not 219.8/s.
4. Make the small source corrections: Y. not E. Uralsky; "a scheme" not "a set scheme"; 16 wave32 is a
   register example and RDNA 1's limit is 20; the GDC 2017 numbers are GCN; 9,068 bytes not 9.8 kB;
   drop or cite the ICPP Best Paper claim, the 6450x gap-array figure, the AsAP row, and the
   2.7-32.8x CABAC figures; close S15 as answered.
