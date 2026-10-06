# Literature sweep: entropy coding and pipelining for the BC-250 H.264 compute encoder

> This sweep has an independent re-check, [`h264-encoder-literature-entropy-pipelining-check.md`](h264-encoder-literature-entropy-pipelining-check.md). The check opened every source again
> on its own and recorded a verdict for each. Some verdicts are VERIFIED WITH CORRECTIONS, and this
> sweep's own text was not rewritten from them. Read the check before you act on a quote here.

Date: 2026-10-06. Scope: entropy coding (CAVLC/CABAC) on GPUs or in parallel, parallel bitstream
assembly, prefix sums for bit offsets, slices as a parallelism unit, overlapping entropy coding with
the next picture, CPU/GPU splits, readback latency hiding, multi-slot pipelining and async compute on
AMD GPUs. Every source below was read at the source (paper PDF, publisher page, or the code file),
not from a summary. Sources that could not be opened are named at the end.

Object of the research: our Media Foundation H.264 encoder MFT,
`driver/umd/mft-h264/` (shaders `cs_me.hlsl`, `cs_mb.hlsl`, `cs_deblock.hlsl`; CPU CAVLC in
`src/h264_cavlc.cpp`; two-slot pipeline in `src/gpu_pipeline.cpp`).

Measured starting point on unit A, 2026-10-06, train b19 (`../LAB-B19-RESULT.md`), 60 pictures,
bit exact against the inbox decoder:

| Case | ours serial | ours pipelined | inbox H.264 (CPU) | GPU busy | readback | CAVLC (CPU) |
|---|---|---|---|---|---|---|
| 720p | 6.46 ms, 154.9/s | 2.75 ms, 363.3/s | 1.51 ms, 662.1/s | 3.61 ms | 4.25 ms | 1.70 ms |
| 1080p | 11.51 ms, 86.9/s | 4.55 ms, 219.8/s | 2.65 ms, 377.0/s | 6.53 ms | 7.57 ms | 3.42 ms |

Quality gap to close separately: Y -1.3 to -2.3 dB, chroma -3.8 to -6.0 dB.

---

## 0. Executive summary of what the literature says

1. **The cheapest large win is not entropy coding at all. It is how the CPU reads the GPU's output.**
   A project that solved the same problem on the same silicon measured a bulk read of GPU staging
   memory at **319.3 ms per frame**, cut to **3.0 ms** by asking for a host-cached memory type, and
   measured its CPU entropy coding at **857.1 ms** cut to **5.6 ms** by copying the staging buffer once
   into ordinary heap memory before the per-macroblock loop (S7). Our own readback is **7.57 ms for
   3.4 MB of staging at 1080p**, about 0.5 GB/s, which is the same signature. See lever L1.
2. **Moving CAVLC to the GPU is the worst-value lever of the four.** The only measured GPU CAVLC inside
   a full H.264 encoder ran **105 fps at 720p (9.5 ms per picture)**, slower than our CPU CAVLC at
   1.70 ms (S4). The best-known GPU CAVLC (CAVLCU, S1) only reports a speedup against that same
   baseline, not an absolute budget. On the GPU the **concatenation, not the symbol coding, is the
   bottleneck**: cuSZ measured Huffman encoding at 380 GB/s and the bit packing that concatenates it at
   60 GB/s (S9). On our exact silicon, a GPU entropy shader was written and then abandoned as dead code
   (S7).
3. **Slices are the proven, cheap way to parallelise entropy coding, and CAVLC pays less for them than
   CABAC does.** x264's own measurements put the worst case (720p, 45 slices, one per macroblock row)
   at **+30 % bitrate at constant PSNR**, and break that cost down: 16 % of it is "reset cabac
   contexts" and 2 % "cabac neighbors", which do not apply to a CAVLC stream at all (S8). At 4 slices
   the cost is roughly a tenth of that density. The same-silicon project ships 4 slices per frame with
   one OpenMP thread per slice as its default at 720p and above (S7).
4. **A third pipeline slot is not the lever. Finer-grained returns are.** Depth 2 already covers one
   picture's entropy coding with the next picture's GPU work, which is the whole serialisation it can
   remove (our own `gpu_pipeline.h` says so). The literature's next step is not more slots but
   *proactive, partial returns*: hand back macroblock rows as they retire instead of at one fence per
   picture (S14, and the overlapped-wavefront idea in the HEVC parallelisation line).
5. **`cs_me` is occupancy-starved by construction, and this is free to fix.** RDNA gives **128 kB of
   LDS per workgroup processor** and up to **16 wave32 per SIMD** (S10). `cs_me` is
   `[numthreads(32,1,1)]` with 9.8 kB of group shared memory, so one wave pays the whole 9.8 kB:
   at most 13 workgroups per WGP, which is 13 waves spread over 4 SIMD32, about **3.2 waves per SIMD,
   near 20 % of what the registers would allow**. A 128- or 256-thread group over the same LDS window
   multiplies resident waves by 4 or 8 and costs no extra LDS. CAVLCU uses exactly that shape:
   128 threads per block, 16 threads per macroblock (S1).

One measurement inconsistency to settle before any of this is tuned: the pipelined 1080p figure
(4.55 ms) is **below** the reported GPU busy time (6.53 ms). With a depth-2 pipeline the steady-state
cost cannot be less than the GPU stage, so either the 6.53 ms figure comes from the instrumented path
(our own `gpu_pipeline.h` warns that per-dispatch timestamp marks "reports a total above the same
picture's uninstrumented cost") or part of the readback runs concurrently with the dispatches. Until
that is resolved, the published breakdown does not say which stage bounds 4.55 ms, and the levers
cannot be ranked by expected ms.

---

## 1. Sources

### S1. CAVLCU: the reference design for CAVLC on a GPU

**Citation.** A. Fuentes-Alventosa, J. Gomez-Luna, J. M. Gonzalez-Linares, N. Guil,
R. Medina-Carnicer. "CAVLCU: an efficient GPU-based implementation of CAVLC". *The Journal of
Supercomputing* 78(6):7556-7590, 2022. DOI `10.1007/s11227-021-04183-8`. Accepted 27 Oct 2021,
published online 29 Nov 2021. Open Access (The Author(s) 2021).
Read in full through the first author's open-access thesis (S3), which reproduces the paper verbatim
with its original pagination; the Springer article page itself refused an unauthenticated fetch.

**What it claims.** CAVLC can be done in a single GPU kernel, with the context dependency (the `nC`
parameter, derived from the left and top neighbour blocks' `TotalCoeff`) carried by wave shuffles
inside a macroblock and by a look-back in global memory between thread blocks, instead of by separate
kernels communicating through global memory.

**Exact quotes.**

Abstract, the four ideas:
> "In this paper, we present CAVLCU, an efficient implementation of CAVLC on GPU, which is based on
> four key ideas. First, we use only one kernel to avoid the long latency global memory accesses
> required to transmit intermediate results among different kernels, and the costly launches and
> terminations of additional kernels. Second, we apply an efficient synchronization mechanism for
> thread-blocks ... that process adjacent frame regions (in horizontal and vertical dimensions) to
> share results in global memory space. Third, we exploit fully the available global memory bandwidth
> by using vectorized loads to move directly the quantized transform coefficients to registers.
> Fourth, we use register tiling to implement the zigzag sorting, thus obtaining high
> instruction-level parallelism."

Abstract, the result:
> "An exhaustive experimental evaluation showed that our approach is between 2.5x and 5.4x faster than
> the only state-of-the-art GPU-based implementation of CAVLC."

Section 4, the thread mapping (p. 7570-7571):
> "The dimensions of the thread-blocks are 4 x 4 x REG_SIZE, where REG_SIZE is the number of MBs of
> each region. ... the i-th MB of the region is assigned to the i-th half-warp of the thread-block, and
> the i-th block of a MB is encoded by the i-th thread of the corresponding half-warp."

Section 4.5, how the `nC` dependency is carried (p. 7576):
> "If the current block is not in the first column of its MB, nA is read from the left thread (x - 1, y,
> z) using the CUDA function __shfl_up ... If the current block is in the first column of the first MB
> of a region, info_A is read from an intermediate array in global memory (d_info_A) of dimensions
> NUM_REG x 4."

Section 4.5, the cross-block synchronisation trick (p. 7579):
> "The elements of d_info_A and d_info_B are initialized to 0 statically. Since all the values written
> are nonzero (due to the fact that the sixth least significant bit is set to 1), the read of each
> element is performed executing the CUDA atomic function atomicExch repeatedly until a nonzero value
> is returned. Additionally, the use of this function restores the stored value to 0, which allows
> subsequent uses of the intermediate arrays in global memory, and avoids getting old cached values."

Section 4.6, the per-block bit assembly (p. 7580):
> "As the VLCs assigned to the CAVLC symbols are obtained, their bits are concatenated in a 32-bit
> variable (word_val) and their lengths added in a second 32-bit variable (word_len) while the
> bit-length of the resulting encoding is less than or equal to 32."

**Measured numbers and conditions.** GeForce GTX 970 (Maxwell, cc 5.2) and GeForce RTX 2080 (Turing,
cc 7.5). First 50 frames of *City* (QCIF), *Mother and Daughter* (CIF), *Ducks take off* (720p), GOP
10, 11 values of QP from 0 to 50, 128 threads per thread block so `REG_SIZE` = 8. Table 10 speedups
over CAVLC_SU: Maxwell min 2.5x, max 5.4x; Turing min 3.0x, max 6.7x (per-clip averages 3.3 to 4.1 on
Maxwell, 4.8 to 5.2 on Turing). Table 11: global load transactions improved 4.26-4.39x (Maxwell),
2.55-2.70x (Turing); executed instructions improved 2.13-2.45x. Crucially, **the paper reports no
absolute ms-per-frame budget in its text** (the runtimes live only in Figures 15-17), so it cannot by
itself tell us whether a CAVLCU-style kernel would beat 3.42 ms on 40 gfx1013 CUs.

**How it applies to us.** It is the design document for a hypothetical `cs_cavlc.hlsl`. Our
`h264_cavlc.cpp` carries exactly the dependency CAVLCU solves: `SliceWriter::NcLuma(gx, gy)` reads
`m_nnzY` of the left and top 4x4 block, and `MvPred` reads neighbour motion vectors. The HLSL
equivalents of the three CUDA primitives it leans on exist: `__shfl_up` is `WaveReadLaneAt` /
`QuadReadAcrossX`, `__popc` is `countbits`, `__ffs` is `firstbitlow`, and `atomicExch` is
`InterlockedExchange`. Expected gain: removes 3.42 ms of CPU work per 1080p picture, but adds an
unknown GPU cost plus a concatenation pass (see S2/S9). Cost: a second full CAVLC implementation.
**Risk to bit exactness: severe.** Our CPU CAVLC is the thing the 82/82 bit-exact verdict is anchored
to. Any GPU CAVLC must be validated against it bit for bit, not against a decoder.

### S2. GVLE: the parallel bitstream concatenation problem, solved

**Citation.** A. Fuentes-Alventosa, J. Gomez-Luna, R. Medina-Carnicer. "GVLE: a highly optimized
GPU-based implementation of variable-length encoding". *The Journal of Supercomputing*, accepted
3 Dec 2022. DOI `10.1007/s11227-022-04994-3`. Source code published by the authors at
`github.com/z12fuala/GVLE`. Read in full through S3.

**What it claims.** Variable-length *encoding* on a GPU is bounded by four things, in this order:
shared-memory bank conflicts in the codeword table, strided global memory access, instruction count
when building the per-thread code, and the inter-block scan that turns per-block bit lengths into
absolute bit positions in the output stream.

**Exact quotes.**

Abstract:
> "Fourth, a novel inter-block scan method, which outperforms those of state-of-the-art solutions, is
> used to calculate the bit-positions of the thread-blocks encodings in the output bit-stream. Our
> proposed mechanism is based on a regular segmented scan performed efficiently on sequences of
> bit-lengths of 32 consecutive thread-blocks encodings by using global atomic additions."

Abstract, results:
> "An exhaustive experimental evaluation shows that our solution is on average 2.6x faster than the
> best state-of-the-art implementation."

Section 2.3.2, how a block's bit position is obtained in the prior art it improves on:
> "Given an element d_scan[i], if the flag A is set, then it stores the bit-length of block-code i;
> otherwise, if the flag P is set, it holds the sum of bit-lengths of block-codes 0 to i, which is the
> bit-position of the block-code i + 1 in the output vector."

Section 2.3.3, the race at the seams between two neighbouring codes:
> "To avoid race conditions with the previous and next thread-codes, the first and last writes are
> performed by using atomic OR operations."

Section 2.3.2.1, why the look-back width matters:
> "in the Yan et al.'s algorithm, each thread-block looks back the result written in global memory by
> only one thread-block, while, in the Yamamoto et al.'s approach, each thread-block looks back 32
> previous results simultaneously. This optimization is the unique reason of the significant speedup."

**Measured numbers and conditions.** Intel Core i7-7800X at 3.50 GHz, 32 GB RAM, GeForce RTX 2080
(Turing cc 7.5), CUDA 11.1, driver 512.15, -O3. Canterbury and Large Canterbury corpora, each file
replicated up to at least 100 MB, 50 timed iterations after one warm-up, thread block 128.

- Table 6, GVLE against YAVLE: runtime 0.50 ms vs 1.30 ms (2.57x); shared load bank conflicts
  3,170,669 vs 13,839,817 (4.36x); global load transactions 6,721,148 vs 60,261,119 (8.97x); global
  reduction transactions 205,459 vs 4,307,361 (20.96x); executed instructions 517,852 vs 1,319,851
  (2.55x).
- Table 7, the whole ladder on 100 MB inputs: GVLE 0.50 ms average (0.42 min, 0.53 max); YAVLE 1.30 ms;
  CUVLE 6.85 ms; serial CPU VLE 191.75 ms. GVLE is 13.63x CUVLE and 377.15x the serial CPU.
- Table 8, the scan alone: GVLE_scan 1.22 ms vs YAVLE_scan 1.97 ms (1.62x) vs CUVLE_scan 47.13 ms
  (38.32x).
- The per-optimisation ladder, so the levers can be ranked: codeword table layout 1.21x, global memory
  access pattern 1.53x, register-space code building 1.23x, inter-block scan 1.14x.

**How it applies to us.** This is the piece a GPU CAVLC would need *after* CAVLCU, and the ladder says
where the time goes. The headline number is also a sanity check: 100 MB in 0.50 ms on an RTX 2080 is
about 200 GB/s, and our 1080p coefficient payload is about 3.4 MB of staging, so the concatenation of a
picture is nowhere near the dominant cost at that efficiency. The warning is the opposite one: the
*least* efficient known inter-block scan (CUVLE_scan, 47.13 ms) is 38x worse than the best, so a
first naive implementation of the bit-position scan can easily cost more than the 3.42 ms it was meant
to save. Expected gain: none on its own. Cost: it is the mandatory second half of lever L4.
Risk to bit exactness: the atomic-OR seams are where an off-by-one bit would show up.

### S3. The open-access thesis that carries S1 and S2 in full

**Citation.** A. Fuentes-Alventosa. *Optimizacion de algoritmos de vision por computador y compresion
de datos en GPU* (doctoral thesis, Programa de doctorado: Computacion Avanzada, Energia y Plasmas),
Universidad de Cordoba, 2023. Handle `10396/25270`, file `2023000002649.pdf`, 9,021,171 bytes.
Download note: `helvia.uco.es` gates the PDF behind a JavaScript cookie step
(`/helvia/helvia_set_cookie.php?dest=...`); fetching that URL first with a cookie jar then the PDF
works.

**Why it is listed separately.** It is the only route we have to the full text of S1 and S2, and it
adds the author's own Spanish-language summary of each contribution, including this statement of
CAVLCU's result that the English abstract words differently:
> "La evaluacion experimental mostro que CAVLCU es entre 2.5x y 5.4x mas rapido que la ..."
and this, on where the GVLE scan matters beyond VLE:
> "La operacion scan es 1.62x mas rapida si se usa el metodo scan inter-bloque propuesto ... Por tanto,
> ofrece posibilidades prometedoras para acelerar algoritmos que lo requieran, como la propia operacion
> scan y el algoritmo de compactacion."

### S4. The only measured GPU CAVLC inside a complete H.264 encoder

**Citation.** H. Su, M. Wen, N. Wu, J. Ren, C. Zhang. "Efficient Parallel Video Processing Techniques
on GPU: From Framework to Implementation". *The Scientific World Journal* 2014:716020, 2014.
DOI `10.1155/2014/716020`. Open access, read at PMC3976889.

**What it claims.** A full H.264 encoder can be offloaded to a GPU, including the "control intensive"
stages, by reorganising CAVLC into four independent component paths and partitioning the encoder's
loop by frame.

**Exact quotes.**
> "To the best of our knowledge, there is no GPU-based CAVLC implementation before our work."
> "Through profiling the instructions of CAVLC, we found three major factors that restrict its
> parallelism, that is, the context-based data dependence, the memory accessing dependence, and the
> control dependence."
> "We partitioned the CAVLC into four paths according to the four components of a frame: Luma_AC,
> Luma_DC, Chroma_AC, and Chroma_DC."
> "We introduced the loop partition technology to divide the whole pipeline into four steps (ME,
> intracoding, CAVLC, and deblocking filter) in terms of frame."
> "Because we do not propose a new CAVLC algorithm, but just reorder the execution sequence, there is
> no impact to the RD performance."
> "More than 96% of workload of H.264 encoder is offloaded to GPU. The CPU is only responsible for some
> simple transactions, such as I/O process."

The two warnings that matter most to us:
> "An interesting observation is that the proportion of the CAVLC rose after parallelization. In
> addition, the number increased with the computation power of the GPU, from 23% on GTX260 to 34% on
> C2050."
> "the parallel degree of the most time consuming kernel (bit_pact) of CAVLC is relatively small and
> decreases with the kernel execution. In addition, the computation-accessing-ratio of CAVLC is
> relatively low; the performance of the proposed CAVLC is majorly determined by the bandwidth of the
> GPU"
> "It should be noticed that the CAVLC achieves a very high performance on the CPU used in this paper
> due to its high frequency and big cache size."
> "For the parallel implementation, though almost all the workloads are offloaded to GPU, the memory
> copy time consists of about 25% even."

**Measured numbers and conditions.** Table 5, reference x264, 720p: their component-based CAVLC on a
GTX260 reaches **105 fps for CAVLC alone**, a speedup of **8x** over x264's CPU CAVLC. Whole encoder on
a Tesla C2050: 13-17x, 32.3 fps at 720p. Also in Table 5 for comparison: AsAP fine-grained multicore
CAVLC, 4.86x, 36-41.3 fps. Platforms: Tesla C2050, GTX460, GTX260; CPU Core i7-2600; D1, 720p, 1080p.

**How it applies to us.** It is the strongest argument *against* lever L4 taken first. 105 fps at 720p
is 9.5 ms per picture for CAVLC alone on a 2008-class GPU. Our CPU CAVLC is 1.70 ms at 720p and
3.42 ms at 1080p. gfx1013 at 40 CUs is far stronger than a GTX260, but the paper's own diagnosis says
the limit is **bandwidth and the small parallel degree of the bit-packing kernel**, not arithmetic,
and bandwidth is exactly what a 128-bit-bus APU has least of. Its useful positive contribution is the
*component split* (Luma_AC / Luma_DC / Chroma_AC / Chroma_DC), which removes control divergence, and
its honest note that reordering alone costs no RD performance.

### S5. The inter-block scan that GVLE improves on, and the gap-array idea

**Citation.** N. Yamamoto, K. Nakano, Y. Ito, D. Takafuji, A. Kasagi, T. Tabaru. "Huffman Coding with
Gap Arrays for GPU Acceleration". *Proc. 49th International Conference on Parallel Processing (ICPP
'20)*, 17 Aug 2020. DOI `10.1145/3404397.3404429`. Best Paper Award.
**Partially opened.** The ACM full-text HTML returned HTTP 403; the authors' ICPP'20 slide deck
(`jnamaral.github.io/icpp20/slides/Yamamoto_Huffman.pdf`) was retrieved but its text layer did not
extract cleanly. The algorithm is therefore quoted here through S2's detailed description of it
(S2 section 2.3), which reimplemented it from the authors' published code
(`github.com/daisuke-takafuji/Huffman_coding_Gap_arrays`), plus the publisher-page abstract.

**What it claims (from the abstract).** Three techniques accelerate GPU Huffman encoding and decoding:
Single Kernel Soft Synchronization (SKSS), wordwise global memory access, and compact codebooks; a
"gap array" attached to the codeword sequence further accelerates decoding.

**Measured numbers and conditions.** NVIDIA Tesla V100, 10 files: encoding 2.87x to 7.70x and decoding
1.26x to 2.63x faster than the previous GPU implementations; decoding a further 1.67x to 6450x with a
gap array. Independently, S2 measured this encoder (as YAVLE) at 1.30 ms average on 100 MB inputs on
an RTX 2080, and its scan component at 1.97 ms.

**How it applies to us.** SKSS is the single-kernel-plus-look-back pattern CAVLCU also uses, and the
32-wide look-back is the one detail that produced the large speedup over the one-predecessor look-back.
If we ever write a GPU CAVLC, the bit-position pass should look back a wave's worth of block lengths
at once, not one. The gap array itself is a *decoder* aid and does not apply to an encoder.

### S6. What parallel entropy coding costs in bitrate: the N-fold CABAC measurement

**Citation.** V. Sze, M. Budagavi, A. P. Chandrakasan, M. Zhou. "Parallel CABAC for Low Power Video
Coding". *IEEE International Conference on Image Processing (ICIP)*, 2008. Read from the authors'
copy at `eems.mit.edu/wp-content/uploads/2023/08/vsze_icip2008_paper.pdf`.

**What it claims.** CABAC can be made to code N bins per cycle, and at N = 2 the coding-efficiency cost
is small.

**Exact quotes.**
> "Experiments show that this new scheme (with N=2) can deliver ~2x throughput improvement at a cost of
> 0.76% average increase in bit-rate or equivalently a decrease in average PSNR of 0.025dB on five 720p
> resolution video clips when compared with H.264/AVC."
> "Arithmetic coding is inherently serial due to strong data dependencies, and typically only a single
> symbol is coded at a time. Consequently, the AC engine is often the bottleneck in the codec"
> "the H.264/AVC CABAC operates serially, with a 1 bin/cycle throughput"
And the reason a CABAC branch would be attractive at all, for the quality target rather than this one:
> "the Context-Based Adaptive Binary Arithmetic Coding (CABAC) provides a 9-14% improvement over the
> Huffman-based Context-Adaptive Variable Length Coding"

**Measured numbers and conditions.** N = 2, five 720p clips, JM reference software, compared against
H.264/AVC single-slice CABAC: +0.76 % bitrate, -0.025 dB PSNR, ~2x throughput. The authors' later work
on massively parallel CABAC reports 2.7x to 32.8x bins per cycle at 0.25 % to 6.84 % coding loss.

**How it applies to us.** Two things. First, it is the price list for breaking an entropy coder's serial
chain: a 2x throughput gain for well under 1 % bitrate is cheap, a 30x gain for up to 6.84 % is not.
Second, the 9-14 % CABAC-over-CAVLC figure is the honest scale of what a CABAC branch would buy on the
quality side. Our chroma gap of 3.8 to 6.0 dB is far larger than 9-14 % of bitrate can explain, so
CABAC is **not** the fix for T3; the partition/intra/chroma-mode-decision diagnosis already in
`LAB-B19-RESULT.md` is.

### S7. The same problem, on the same silicon: the BC-250 VA-API driver

**Citation.** `MTSistemi/bc250-vaapi`, also published as `simpmix/bc250-encoding-decoding-fix`
v0.4.0. "AMD BC-250 VA-API Driver & Video Acceleration Suite". Read at
`https://github.com/MTSistemi/bc250-vaapi` (README) and the raw files
`approach1-compute-encoder/src/gpu_compute.c`, `approach1-compute-encoder/src/encoder_h264.c`,
`approach1-compute-encoder/shaders/entropy_encode.comp`.

> **LICENCE TRAP: GPL-3.0 (driver and shader code), GPL-2.0 for the audio kernel module.** Our MFT is
> MIT. This source is a **read-only reference for facts and ideas**. Do not copy code, tables, shader
> text or comments from it into our tree. If a directory is ever mirrored locally, name it
> `bc250-vaapi__WARN-GPL3-no-code-in-our-driver` per the workspace rule.

**What it claims.** The BC-250's hardware video engine is fused off, so H.264 and HEVC encoding run as
Vulkan compute shaders on the 40 CUs with entropy coding on the Zen 2 CPU.

**Exact quotes.**

README, the hardware fact:
> "The BC-250 is a repurposed PS5 APU (Zen 2 8-core/16-thread CPU, up to 40 unlocked Compute Units;
> Oberon / Cyan Skillfish semi-custom RDNA 1.5 architecture) whose physical VCN (Video Core Next)
> hardware engine was permanently unprovisioned and eFused off at the factory."

README, the architecture:
> "Real-time H.264 and H.265/HEVC encoding executed across the APU's 40 Compute Units using custom
> Vulkan compute shaders with asynchronous pipelining and AVX2 CPU SIMD offloading."

README, the measured encode rates ("Measured on BC-250 Silicon"): H.264 Vulkan compute 640x480
**267 fps**, 720p **179 fps**, 1080p **100-134 fps**, 1440p **67-80 fps**; HEVC 1080p **111+ fps**
"with default `BC250_HEVC_SLICES=4`, SIMD 4x4 transforms, and `MOVNTDQA` streaming readback". Game
streaming overhead "Only ~4.5% total GPU impact during active 60 FPS gaming".

README, the entropy and slice knobs:
> "`BC250_SLICES_PER_FRAME` | `4` | Number of slices per H.264 frame. Use `2` for multi-stream VR to
> prevent CPU thread congestion."
> "`BC250_USE_CABAC` | `1` (Main/High) | Toggles CABAC (10-13% smaller bitrate) vs CAVLC for H.264
> encode."
> "`OMP_WAIT_POLICY` | `PASSIVE` | Critical: enforces passive wait in `libgomp`, cutting CPU usage from
> 1300% to ~350%."

README, the chroma note (adjacent to this sweep but directly on our T3 gap):
> "Chroma Fidelity: Bit-exact non-linear Table 8-10 QP mapping eliminates the standard chroma PSNR
> deficit."

`gpu_compute.c`, `find_memory_type_preferred()` doc comment, the readback pathology measured on real
hardware:
> "On real BC-250 hardware this driver was measured (BC250_PERF_STATS=1, real board run, 1280x720)
> paying ~81ms/frame - the entire real-time-throughput gap between ~1ms of actual GPU compute + ~1ms of
> CPU CAVLC and the ~83ms real wall-clock time per frame - inside shadow_copy()'s bulk memcpy() itself,
> i.e. the CPU *reading* ~10.6MB/frame back out of that same memory. Uncached/write-combined memory has
> notoriously poor CPU read bandwidth (routinely an order of magnitude or more below normal cached RAM)
> even for a single fully sequential streaming pass"

`encoder_h264.c`, `shadow_copy()` doc comment, the second half of the same problem, with a 2x2 measured
at 1440p (mean P-frame ms):
> ```
>            cached        uncached
>   shadow    13.76         329.92     <- memcpy itself: 3.0ms vs 319.3ms
>   no shadow 14.57         862.26     <- CAVLC: 5.6ms vs 857.1ms
> ```
> "HOST_CACHED is what makes the bulk read affordable (319ms -> 3.0ms); this function is what keeps the
> per-MB scattered reads off that memory at all (a HOST_CACHED mapping still costs CAVLC 5.6 -> 13.1ms
> when read directly, so the flag alone does not make the staging buffer behave like ordinary cacheable
> RAM). Removing either one regresses; removing both is the original ~860ms/frame pathology."

`encoder_h264.c`, the diagnosis of where their per-macroblock CPU cost really was (a warning against
micro-optimising the bit writer):
> "the ~44-63us/macroblock CPU-side CAVLC cost this project has been chasing is NOT dominated by
> entropy-coding bit writes at all (batching cavlc.c's unary zero-bit writes into single bs_write_u()
> calls - this branch's other commit - moved the board-measured fps by under 0.6%). Per-MB timing
> brackets isolated the real cost: the unconditional-per-MB skip decision (mb_has_any_luma_nonzero() +
> mv_predictor(), called for EVERY macroblock before any entropy coding happens at all) alone averaged
> ~36-38us/MB at every tested resolution"

`encoder_h264.c`, the streaming-load detail:
> "The source is mapped write-combining staging memory: an ordinary load fetches part of a line at a
> time. Measured on a BC-250, this memcpy was 14% of the H.264 encoder thread. MOVNTDQA reads a whole
> line into a fill buffer, and degrades to an ordinary load if the memory turns out cached."

`encoder_h264.c`, how they cut the payload that crosses the bus at all:
> "every CPU read of the pre-quant coefficient buffer was position 0 of some block - 24 of the 384 ints
> per macroblock ... so the GPU now writes those values to a compact num_mbs*24 buffer and only that
> crosses to the host. The full coefficient buffer stays device-local ... At 1440p this drops 44.2 MB of
> host-visible staging per encoder context, 22.1 MB of per-frame GPU->host copy, and 22.1 MB of
> per-frame shadow_copy()."

`encoder_h264.c`, their CPU entropy parallelism (one slice per OpenMP iteration):
> "`#pragma omp parallel for schedule(static) num_threads(threads) if(threads > 1 && num_slices > 1)`"
with the default chosen as:
> "When backed by real GPU context at HD/FHD resolutions (>= 720p), default to 4 slices for
> multi-threaded parallel OpenMP entropy coding."

`gpu_compute.c`, on where GPU-side contention time actually goes, and what fixes it:
> "under real GPU contention this encoder's frame is ~675ms of which its shaders only EXECUTE ~2.3ms -
> the rest is the submission waiting behind the other process's work (DEVLOG 24.4). At 60fps that is
> only ~14% of the GPU being asked for, yet contention drops the encoder to 1.48fps. Queue *priority*,
> not more queues and not CPU/GPU overlap, is the mechanism aimed at that wait."

`gpu_compute.c`, the state of their GPU entropy path:
> "staging_buffers[]/entropy_buffer above are a separate, currently-dead GPU-entropy-coding path
> (nothing reads gpu_compute_get_staging_data()) and are deliberately left on plain
> create_buffer_with_memory()."

And the shader that path would have used is a symbol pre-pass only, not a bitstream writer: 72 lines,
`layout(local_size_x = 32) in; // Wave32 aligned`, 24 blocks per macroblock, output one packed 32-bit
word per 4x4 block holding `total_coeff`, `trailing_ones`, packed signs and four packed magnitudes, so
the actual variable-length codes and all concatenation stay on the CPU.

**Measured numbers and conditions.** All of the above are the project's own on-board measurements:
1280x720 and 2560x1440 for the readback 2x2, 1080p/1440p for the fps table, default 4 slices,
`BC250_PERF_STATS=1` brackets. Hardware is the same part as unit A.

**How it applies to us.** This is the most directly usable source in the sweep, on four counts.
1. **Readback memory type and read pattern.** They measured a 100x penalty for CPU reads of
   write-combined staging memory on this exact silicon, and a further 2.3x for scattered per-macroblock
   reads even after the memory type is fixed. Our readback is 7.57 ms for the 3.4 MB of levels and
   macroblock-info staging a 1080p picture carries (per `gpu_pipeline.h`: "every slot costs one levels
   and one macroblock-info staging buffer (3.4 MB together at 1080p)"), with about 1 ms of that being
   the wait. That leaves about 6.5 ms of transfer and memcpy for 3.4 MB, roughly **0.5 GB/s**, which is
   write-combined-read territory, not cached-RAM territory. See lever L1.
2. **Their H.264 1080p compute-encode rate is 100-134 fps against our 219.8/s pipelined.** We are
   already ahead of the only comparable implementation on this hardware. The inbox CPU encoder at
   377 fps is the thing to beat, not them.
3. **They parallelise the CPU entropy stage with 4 slices and OpenMP by default at 720p and above**,
   and had to force 1 slice for Steam Link clients whose decoders fail on multi-slice H.264. That is a
   compatibility warning for any slice work we do.
4. **They wrote a GPU entropy shader and abandoned it.** On this hardware, with a CPU this fast, the
   GPU entropy path did not pay. Treat lever L4 accordingly.

### S8. x264's own measurement of what slices cost

**Citation.** x264, `doc/threads.txt` (upstream file, read from the GitHub mirror
`mirror/x264` at `master`). Benchmarks in it are dated to x264 r1732, commit `b20059aa`.

**What it claims.** Slice-based threading adds bitrate for several separable reasons, and the
contribution of each was measured.

**Exact quotes.**
> "Penalties for slice-based threading:
> Each slice adds some bitrate (or equivalently reduces quality), for a variety of reasons: the slice
> header costs some bits, cabac contexts are reset, mvs and intra samples can't be predicted across the
> slice boundary."

> "Some numbers on penalties for slicing:
> Tested at 720p with 45 slices (one per mb row) to maximize the total cost for easy measurement.
> Averaged over 4 movies at crf20 and crf30. Total cost: +30% bitrate at constant psnr.
> I enabled the various components of slicing one at a time, and measured the portion of that cost they
> contribute:
>     * 34% intra prediction
>     * 25% redundant slice headers, nal headers, and rounding to whole bytes
>     * 16% mv prediction
>     * 16% reset cabac contexts
>     * 6% deblocking between slices ...
>     * 2% cabac neighbors (cbp, skip, etc)"

> "But none of the proportions should depend strongly on the number of slices: some are triggered per
> slice while some are triggered per macroblock-that's-on-the-edge-of-a-slice, but as long as there's no
> more than 1 slice per row, the relative frequency of those two conditions is determined solely by the
> image width."

> "Some parts of the encoder are serial, so it doesn't scale well with lots of cpus."

**Measured numbers and conditions.** 8-core Nehalem (2 x E5520) 2.27 GHz, hyperthreading off, Linux
2.6.34.7 64-bit, `park_joy_1080p.y4m`, `--tune psnr --crf 30`. Slice threading vs frame threading,
speedup and PSNR delta:

| threads | slice speedup | frame speedup | slice PSNR | frame PSNR |
|---|---|---|---|---|
| 2 | 1.41x | 2.29x | -0.005 | -0.002 |
| 4 | 1.96x | 3.97x | -0.029 | -0.001 |
| 8 | 2.43x | 3.98x | -0.067 | -0.001 |

(`--preset veryfast`; at `--preset medium` the slice column reaches 3.79x at 8 threads with -0.015 dB,
at `--preset slower` 4.13x with -0.026 dB.)

**How it applies to us.** It prices lever L3 in advance, and the price is lower for us than for x264:
- 18 % of x264's measured slice cost is CABAC-specific ("reset cabac contexts" 16 % plus "cabac
  neighbors" 2 %) and **does not exist in a CAVLC stream**.
- The 45-slices-per-720p-frame test is a deliberate worst case at one slice per macroblock row. At 4
  slices in a 1080p picture (68 macroblock rows) the per-slice-boundary effects are about one eleventh
  as frequent, and the per-slice-header effects about one eleventh as many.
- x264's own slice-threading speedup is sub-linear (1.96x at 4 threads) because its slice threads do
  analysis as well as entropy coding. Our slice threads would do entropy coding only, from an already
  complete coefficient buffer, so they should scale closer to linearly.
Expected gain: CAVLC 3.42 ms to roughly 1 ms at 1080p with 4 threads. Cost: per-slice bit writers and a
concatenation step, a `first_mb_in_slice` header per slice, and a reset of `m_skipRun` and of the
neighbour availability rules at every slice start in `SliceWriter`. Risk to bit exactness: this
**changes the bitstream** (it is a different, equally legal encode), so the bit-exact verdict has to be
re-baselined per slice count, and the 82/82 case list needs a 1-slice case kept as the anchor.

### S9. cuSZ: the measured proof that concatenation, not coding, bounds GPU entropy coding

**Citation.** J. Tian, S. Di, K. Zhao, C. Rivera, M. H. Fulp, R. Underwood, S. Jin, X. Liang,
J. Calhoun, D. Tao, F. Cappello. "cuSZ: An Efficient GPU-Based Error-Bounded Lossy Compression
Framework for Scientific Data". *PACT 2020*. arXiv `2007.09625`.

**What it claims.** On a GPU, Huffman symbol encoding is nearly free compared with "deflating", the
pass that concatenates variable-length codes and removes the padding bits.

**Exact quotes.**
> "To generate the dense bitstream of Huffman codes within each data block, we conduct deflating in
> order to concatenate the Huffman codes and remove the unnecessary zero bits according to the saved
> bitwidths."
> "Table 4 illustrates that our encoding achieves about 250 GB/s for uint64_t and about 380 GB/s for
> uint32_t, based on the test with all 111 fields under the error bound of 1e-4. ... Because of the
> coarse-grained chunk-wise parallelization, the performance of deflating is about 60 GB/s, which is
> lower than the encoding throughput of 380 GB/s. Consequently, the Huffman coding performance is
> bounded mainly by the deflating throughput."
> "We observe that using a total of around 2e4 concurrent threads consistently achieves the optimal
> throughput."
> "we reuse the memory space of Huffman codes for the deflated bitstream because the latter uses
> significantly less memory space and does not have any conflict when writing the deflated bitstream to
> the designated location."

**Measured numbers and conditions.** NVIDIA V100, five HPC datasets (HACC, CESM-ATM, Hurricane ISABEL,
Nyx, QMCPACK), 111 fields, error bound 1e-4. Encoding 380 GB/s (uint32 codeword representation),
250 GB/s (uint64), deflating about 60 GB/s. Codebook construction 0.68 to 50.71 ms depending on the
number of quantisation bins.

**How it applies to us.** A 6.3x gap between coding and concatenation is the design constraint for any
GPU CAVLC we write: budget for the concatenation first, and pick a 32-bit code container rather than
64-bit. The "2e4 concurrent threads" optimum is also a useful target shape: a 1080p picture has 8160
macroblocks and 195,840 4x4 blocks, so one thread per block is an order of magnitude past that optimum
and one thread per macroblock is just under it.

### S10. AMD RDNA Architecture: the occupancy arithmetic for cs_me

**Citation.** AMD, "RDNA Architecture" public presentation, GPUOpen
(`gpuopen.com/download/RDNA_Architecture_public.pdf`).

**Exact quotes.**

Slide "LDS per workgroup processor":
> "128 kB per workgroup processor"
> "Up to 64 kB per workgroup"
> "Read / write / atomic throughput of up to 32 dwords per cycle (doubled relative to GCN)"
> "32 banks"
> "Mind the bank conflicts!"

Slide on register-based occupancy:
> "Each SIMD32 has 1024 physical registers"
> "16x Wave32 with 64 VGPRs"
> "Occupancy in '# of threads per SIMD lane' is unchanged from GCN ... RDNA equivalent: 16x Wave32 or
> 8x Wave64"

Slide on workgroups:
> "Workgroup size: keep it a multiple of 64"

**How it applies to us.** `cs_me.hlsl` declares `[numthreads(32,1,1)]` and holds 9.8 kB of
`groupshared` (the window arrays `gInt[23*23]`, `gBRaw[23*18]`, `gH[18*18]`, `gJ[18*18]`, `gPart[9*32]`
plus `gSrcMb[256]` and the reduction scratch). One wave per workgroup means one wave carries the full
9.8 kB allocation. With 128 kB per WGP that is at most 13 resident workgroups, so 13 waves across the
WGP's 4 SIMD32 units: about **3.2 waves per SIMD**, against the 16 that 64 VGPRs would allow. The fix
is not less LDS; it is more threads sharing the same LDS: a 128-thread group (4 waves, still one
macroblock's window) gives about 13 waves per SIMD at the same LDS footprint, and also satisfies
"keep it a multiple of 64". This is the one lever on the list with no bitstream risk at all if the
search order is preserved, and it is the lever `LAB-B19-RESULT.md` already names ("the cs_me occupancy
limit (9.8 KB group shared memory)"). CAVLCU's 128-thread block (S1) is the precedent.

### S11. AMD RDNA Performance Guide

**Citation.** AMD GPUOpen, "RDNA Performance Guide" (`gpuopen.com/learn/rdna-performance-guide/`).

**Exact quotes.**
> "GCN runs shader threads in groups of 64 known as wave64." / "RDNA runs shader threads in groups of
> 32 known as wave32."
> "Make the workgroup size a multiple of 64 to obtain best performance across all GPU generations."
> "Prefer a struct of arrays or add padding to reduce access strides and bank conflicts."
> "Async compute queues can be used to issue compute commands to the GPU parallel to the graphics
> queue."
> "Smaller workgroups (64 threads) usually perform better than larger workgroups when run async."
> "Minimize the number of barriers used per frame." / "Batch groups of barriers into a single call to
> reduce overhead."

**How it applies to us.** Two concrete items. The bank-conflict advice applies to `cs_mb.hlsl`'s
`gCoef[24][16]` and `gLev[24][16]`: a stride of 16 dwords over 32 banks puts blocks 0 and 2 in the same
bank set, and padding to 17 would break that; GVLE measured a 4.36x reduction in shared-load bank
conflicts worth 1.21x runtime for exactly this class of fix (S2). The barrier advice matters because a
1080p I picture holds about 440 dispatches per `gpu_pipeline.h`.

### S12. Asynchronous Compute Deep Dive (GDC 2017)

**Citation.** A. Dunn (NVIDIA), S. Hodes (AMD). "Asynchronous Compute Deep Dive", GDC 2017, 29 slides
(`gpuopen.com/download/GDC2017-Asynchronous-Compute-Deep-Dive.pdf`).

**Exact quotes.**
Slide "Hardware Details":
> "4 SIMD per CU" / "Up to 10 Wavefronts scheduled per SIMD" / "Accomplish latency hiding" / "Graphics
> and Compute can execute simultanesouly on same CU"
Slide "Resource Contention":
> "Problem: Per SIMD resources are shared between Wavefronts" / "Occupancy limited by # of registers,
> Amount of LDS, Other limits may apply..." / "Wavefronts contest for caches" / "Beware of cache
> thrashing!" / "Try limiting occupancy by allocating dummy LDS"
Slide "Async. Tax":
> "Additional CPU work organizing/scheduling async tasks" / "Synchronization/ExecuteCommandLists
> overhead" / "Synchronization overhead" / "Additional barriers (cross-queue synchronization)"
Slide "Async. Tax - Advice":
> "First: determine if CPU or GPU is the bottleneck (GPUView)"
Slide "GPU View #3 - Events" names the exact kernel events to look for:
> "ID3D12Fence::Signal - DxKrnl - SignalSynchronizationObjectFromCpu" / "ID3D12CommandQueue::Wait -
> DxKrnl - WaitForSynchronizationObjectFromGpu"

**How it applies to us.** It is the authority for the claim that LDS is one of the two occupancy
limiters (lever L5), and it is the caution against reaching for a second queue: the "async tax" is
real, and S7 measured on this very board that *queue priority*, not more queues, was what addressed
submission wait. It also names the ETW events to fold if we ever want to see our own fence waits in a
trace, which is directly reusable given the project's existing ETW tooling.

### S13. GDeflate: the sub-stream layout for parallel variable-length codes

**Citation.** E. Uralsky (NVIDIA). "GDEFLATE bitstream specification", IETF Internet-Draft
`draft-uralsky-gdeflate-00`.

**Exact quotes.**
> "To enable parallel parsing of the bit stream, GDeflate splits the ... sub-streams. Each sub-stream is
> assigned to a fixed SIMD lane, so all ..."
> "GDeflate targets SIMD width of 32, which aligns well with most common ..."
> "The SIMD group is assumed to comprise 32 parallel 'lanes', even though the physical SIMD width of ..."
> (section headings) "5. Bit stream rearrangement for SIMD parallelism", "5.1. Sub-streams and ordering
> of symbols", "5.2. Packing of variable-length codes within sub-streams"

**How it applies to us.** It is the clean statement of the alternative to a prefix-sum concatenation:
instead of computing every code's absolute bit offset, define the container format so that 32 lanes
each own a fixed sub-stream and no offset arithmetic is needed. **We cannot use it.** The H.264 slice
RBSP is a single bit stream with a normative order, so we have no freedom to rearrange it; a decoder
would reject it. It is listed because it explains *why* H.264 entropy coding is harder to parallelise
than general-purpose compression, and it names the one escape hatch H.264 does give us for the same
purpose: slices (lever L3), which are the standard's own sanctioned sub-streams.

### S14. Fine-grained CPU-GPU synchronisation: the case for partial, early returns

**Citation.** D. Lustig, M. Martonosi. "Reducing GPU Offload Latency via Fine-Grained CPU-GPU
Synchronization". *19th IEEE International Symposium on High Performance Computer Architecture (HPCA)*,
2013. Princeton University. Read from
`mrmgroup.cs.princeton.edu/papers/dlustigHPCA13.pdf`.

**Exact quotes.**
> "For many workloads, however, the performance benefits of offloading are hindered by the large and
> unpredictable overheads of launching GPU kernels and of transferring data between CPU and GPU."
> "We then propose a set scheme of full-empty bits to track when regions of data have been transferred.
> This dependency tracking is fast, efficient, and fine-grained, mitigating much of the latency
> uncertainty and cost of offloading in current systems. On top of these full-empty bits, we build APIs
> that allow for early kernel launch and proactive data returns."
> "across a set of seven diverse benchmarks that make use of our support, the mean improvement in
> runtime is 26%."
> "these techniques deliver performance improvements of 26%"
Also, on where the cost actually sits:
> "coarse-grained synchronization each add latency overheads"
and, in the caption to Figure 2, an inconvenient fact for integrated parts:
> "Memory copy latency between CPU and GPU for NVIDIA GTX580 (discrete) and AMD A8-3870K (integrated)
> GPUs. For small transfer sizes, the discrete GPU actually has lower latency than the integrated
> case."

**Measured numbers and conditions.** Simulated hardware extension (full/empty bits in the GPU memory
controller) plus real-system characterisation on a GTX 580 and an AMD A8-3870K APU; seven benchmarks;
mean runtime improvement 26 %, and "the overlap scenario provides an average of 26% speedup".

**How it applies to us.** The hardware proposal is not available to us, but the *software shape* is: do
not wait for one fence per picture before starting entropy coding. Copy macroblock rows (or slices) to
staging as they retire, signal a query per group of rows, and let the CPU start CAVLC on row 0 while the
GPU is still on row 40. That is "proactive data returns" without needing full/empty bits, and it is the
natural extension of our existing two-slot pipeline: slots hide a whole picture's latency, per-row
returns hide a whole picture's *readback*. Expected gain: up to the smaller of the CAVLC stage and the
readback stage per picture, so a few ms at 1080p. Cost: more staging copies and more query slots
(`gpu_pipeline.h` already notes "the timestamp queries need one copy per slot"); more D3D11 Map calls,
each of which is a kernel transition. Risk to bit exactness: none, if the row order is preserved.

### S15. BC-250 hardware documentation, for the VCN question

**Citation.** `elektricM/amd-bc250-docs`, "AMD BC250 Documentation", `docs/hardware/specifications.md`
and `docs/getting-started/introduction.md` (`elektricm.github.io/amd-bc250-docs/`).

**Status: contradicted, flagged rather than relied on.** A web search surfaced two incompatible claims
about this part: S7's README states the VCN block "was permanently unprovisioned and eFused off at the
factory", while a search summary of this documentation site asserted that "As of Mesa 25.1 and kernel
6.11, the VCN block is enabled and provides full hardware H.264/H.265 decode and encode, plus VP9
decode". A third repository, `m2jgh8tg7r-bot/bc250-vcn-linux-research`, describes itself as
"Reverse-engineering and Linux enablement research for AMD BC-250 / Cyan Skillfish VCN 2.0.3, including
NBIO doorbell, power, firmware, and ring bring-up", which is consistent with the block being present
but not brought up rather than absent.

**How it applies to us.** It does not change this sweep's recommendations, but it is a loose end worth
one bounded check against `bc250-win\docs\facts.md` and a Windows-side enumeration, because a working
VCN encoder would make the whole compute encoder an engineering exercise rather than the only route.
Do not act on the search summary; read the documentation site and the research repository at the source
first.

---

## 2. The levers, ranked

Ranked by expected ms per 1080p picture divided by risk. Numbers marked "estimate" are derived from the
sources' measurements on comparable or identical hardware, not measured on our encoder.

### L1. Fix the readback path: memory type plus one streaming copy (highest value)

- **Where.** `src/gpu_pipeline.cpp` (the staging buffer creation and the `Collect` / Map path),
  `src/h264_cavlc.cpp` (which currently reads the collected vectors).
- **Evidence.** S7's two measured fixes on this exact silicon: host-cached memory type took a bulk read
  of 22 MB from 319.3 ms to 3.0 ms, and copying once into ordinary heap memory before the per-macroblock
  loop took CAVLC from 857.1 ms to 5.6 ms; both are needed, removing either regresses. Their
  `MOVNTDQA` note (a non-temporal load reads a whole line into a fill buffer and degrades gracefully if
  the memory turns out cached) is the mechanism.
- **Our own evidence that we have the same problem.** 3.4 MB of staging per 1080p picture
  (`gpu_pipeline.h`), readback 7.57 ms of which about 1 ms is the wait, so about **0.5 GB/s** for the
  transfer and copy. On the development PC the same run shows readback waits of 4.78 to 5.11 ms against
  0.87 ms of CAVLC at 720p (`mft-h264/README.md`), which is the same shape.
- **First action, no code change.** Establish which D3D11 usage and CPU access flags our staging
  buffers actually get, and whether `Collect` copies out of the mapped pointer once or lets the CAVLC
  loop read it. If `Collect` already `memcpy`s into `std::vector`, half the fix is in place and the
  remaining question is only the memory type and the use of streaming loads.
- **Expected gain (estimate).** Several ms per 1080p picture off whatever stage bounds the pipeline. If
  the readback transfer went from 0.5 GB/s to the 7 GB/s S7 achieved, 6.5 ms becomes under 0.5 ms.
- **Risk to bit exactness: none.** A copy is verbatim and a memory type cannot change a bit. S7 says
  the same: "this changes WHERE the CPU reads a byte from, never WHAT byte it reads".

### L2. Per-row or per-slice proactive returns, instead of one fence per picture

- **Where.** `src/gpu_pipeline.cpp` (`Submit` / `Collect`), `src/h264_cavlc.cpp` (so `SliceWriter` can
  consume rows as they arrive).
- **Evidence.** S14 (proactive data returns, 26 % mean runtime improvement from overlap), and our own
  `gpu_pipeline.h`, which states that depth 2 "is enough to cover the entropy coding of one picture with
  the GPU work of the next, which is the whole of the serialisation this removes; a deeper pipeline only
  adds latency". That sentence is the argument against a third slot and for finer granularity instead.
- **Expected gain (estimate).** Up to the smaller of the CAVLC stage and the readback stage, so a few ms
  at 1080p; the exact figure depends on which stage the inconsistency in section 0 resolves to.
- **Cost.** More staging buffers or sub-range copies, more queries, more Map calls. The CAVLC
  neighbour dependency is left-and-up only, so row-by-row consumption is legal with a one-row lag.
- **Risk to bit exactness: none** if row order is preserved.

### L3. CAVLC on 4 CPU slices with 4 threads

- **Where.** `src/h264_cavlc.cpp` (`SliceWriter`: per-slice state, `m_skipRun` reset, neighbour
  availability at slice edges), `src/h264_syntax.cpp` (`first_mb_in_slice`), `src/encoder.cpp` (the NAL
  assembly).
- **Evidence.** S8 for the price (and for the 18 % of x264's measured slice cost that is CABAC-only and
  does not apply to us), S6 for the general shape of the throughput/bitrate trade, S7 for the fact that
  4 slices with one OpenMP thread each is the shipped default on this CPU.
- **Expected gain (estimate).** CAVLC 3.42 ms to about 1 ms at 1080p. On the serial path that is a 2.4 ms
  saving; on the pipelined path it only helps if CAVLC is the bounding stage.
- **Cost.** Real but small bitrate cost at 4 slices, to be measured, not assumed. Compatibility: S7 had
  to force 1 slice for Steam Link decoders.
- **Risk to bit exactness: this changes the bitstream.** Keep a 1-slice case as the bit-exact anchor and
  re-baseline the 82-case sweep per slice count.

### L4. CAVLC on the GPU (lowest value, highest risk)

- **Where.** A new `shaders/cs_cavlc.hlsl` plus a bit-position pass, and a validation harness against
  `h264_cavlc.cpp`.
- **Evidence for.** S1 gives the design (one kernel, wave shuffles for the in-macroblock `nC`, a global
  look-back between regions, register-space zigzag, vectorized loads) and S2/S5 give the concatenation
  (segmented scan with global atomic adds, a 32-wide look-back, atomic-OR at the seams). S4 gives the
  component split that removes control divergence.
- **Evidence against.** S4's measured GPU CAVLC is 9.5 ms per 720p picture, slower than our 1.70 ms CPU
  CAVLC, and its own diagnosis is bandwidth and the small parallel degree of the bit-packing kernel. S9
  measures concatenation 6.3x slower than coding. S7, on our exact silicon, wrote the shader and left
  the path dead. S1 publishes no absolute ms budget.
- **Expected gain.** Removes 3.42 ms of CPU work; adds an unknown GPU cost on a GPU stage that is
  already the larger half of the picture.
- **Risk to bit exactness: severe.** It replaces the component the 82/82 verdict rests on.
- **Recommendation.** Do not start this before L1, L2, L3 and L5 are measured. If it is started, write
  the bit-position scan first and measure it alone against 3.42 ms; if the scan alone costs more, stop.

### L5. Raise the cs_me workgroup from 32 to 128 threads

- **Where.** `shaders/cs_me.hlsl`, `[numthreads(32,1,1)]` and the reduction over `gCost`/`gSad`/
  `gCandX`/`gCandY` (currently sized 32).
- **Evidence.** S10 (128 kB LDS per WGP, up to 64 kB per workgroup, 16 wave32 per SIMD at 64 VGPRs),
  S11 ("Make the workgroup size a multiple of 64"), S12 (LDS as an occupancy limiter), S1 (128-thread
  blocks with 16 threads per macroblock as the working precedent).
- **Arithmetic.** 9.8 kB per 1-wave group gives at most 13 waves per WGP, about 3.2 per SIMD32. The same
  9.8 kB per 4-wave group gives about 13 waves per SIMD32.
- **Expected gain (estimate).** This is the only lever aimed at the GPU half of the picture (6.53 ms
  reported, under 4.55 ms implied). Latency hiding on a memory-bound search should improve materially,
  but no source gives a number for this shader, so it must be measured.
- **Risk to bit exactness: none if the candidate search order and the tie-breaking are preserved.** A
  wider reduction changes which lane wins a tie unless the comparison keeps the same ordering, and that
  would change motion vectors and therefore the bitstream. Write the reduction so that equal costs
  resolve to the same candidate as today, and verify against the 82-case sweep.

### L6. Async compute queue or queue priority (not now)

- **Evidence.** S11 ("Async compute queues can be used to issue compute commands to the GPU parallel to
  the graphics queue"), S12 (the async tax: extra CPU scheduling, cross-queue barriers, ExecuteCommandLists
  overhead), S7 (on this board, under contention, "Queue *priority*, not more queues and not CPU/GPU
  overlap, is the mechanism aimed at that wait", with 675 ms frames holding only 2.3 ms of shader
  execution).
- **Recommendation.** Not a throughput lever for an isolated encode trial. It becomes relevant only when
  the encoder has to share the GPU with a game, which is a separate goal (S7 reports ~4.5 % GPU impact
  during 60 fps gaming as their target).

---

## 3. One finding outside the sweep's topic, recorded because it is large

Our chroma deficit is 3.8 to 6.0 dB, which is far larger than any entropy or partition decision can
explain by itself. S7's README attributes the elimination of exactly this class of defect to the chroma
quantisation-parameter mapping:

> "Chroma Fidelity: Bit-exact non-linear Table 8-10 QP mapping eliminates the standard chroma PSNR
> deficit."

(The table number quoted is the HEVC one; the H.264 equivalent is the non-linear `qPi` to `QPc`
derivation, plus `chroma_qp_index_offset` in the PPS.) The claim is testable without any lab time:
check how `cs_mb.hlsl` and `h264_syntax.cpp` derive the chroma QP, and whether a linear mapping or a
missing offset is being used where the standard requires the table. This belongs to the quality branch
named in `LAB-B19-RESULT.md`, not to this sweep, and is recorded here only so it is not lost.

---

## 4. Sources that could not be opened

- **ACM Digital Library full text** of Yamamoto et al., ICPP '20 (`dl.acm.org/doi/fullHtml/10.1145/
  3404397.3404429`): HTTP 403. Worked around through S2's detailed reimplementation account, the
  authors' published code repository name, and the publisher abstract. The authors' own ICPP'20 slide
  deck downloaded but its text layer did not extract.
- **Springer article pages** for S1 (`10.1007/s11227-021-04183-8`) and S2 (`10.1007/s11227-022-04994-3`):
  both redirect to `idp.springer.com` for authentication. Both papers were read in full through the
  first author's open-access thesis (S3) instead, which reproduces them verbatim with their journal
  pagination.
- **ResearchGate** copies of several papers: not fetchable.
- **Chi, Alvarez-Mesa, Juurlink et al., "Parallel Scalability and Efficiency of HEVC Parallelization
  Approaches", IEEE TCSVT 22(12), 2012**, and its Overlapped Wavefront (OWF) idea, which is the closest
  published analogue of "start the next picture before the current one finishes": the TU Berlin
  open-access PDF failed with a self-signed certificate in the chain, and the ACM/IEEE pages are
  paywalled. Only the reported figures reached me second hand (average speedups of 8.7, 9.3 and 10.7 for
  WPP, Tiles and OWF on 4K sequences), so they are **not** cited as evidence above. Worth one more
  attempt: it is a decoder paper, but OWF is the one named prior art for lever L2.
- **`vip.ac.uma.es` publication list** (the authors' group page, a possible source of author PDFs): TLS
  certificate does not match the host name.
- **Sitaridi, Mueller, Kaldewey, Lohman, Ross, "Massively-Parallel Lossless Data Decompression",
  arXiv 1606.00519**: opened and read, then judged not applicable. It is a decompression paper; its
  intra-block and inter-block parallelism framing duplicates what S2 says better for the encode side.
- **`elektricM/amd-bc250-docs`**: the search summary of this site contradicts S7 on whether the VCN
  block is usable. Not read at the source yet, so it is recorded as an open question in S15 rather than
  as a finding.
