# BC-250 LLM reference numbers from public Linux sources

Research note for the M16 HIP work. It records the public LLM throughput numbers for this
board, and the exact page and revision behind each one. Nothing here was measured on our
Windows stack. These are targets to compare against, not our own results.

An earlier draft of this note mixed the two decode columns of the `akandr/bc250-rocm` table.
It also stated three numbers without naming a page. This note replaces that draft.

## 0. Source provenance

| Source | File | Revision | Fetch date |
| --- | --- | --- | --- |
| `akandr/bc250-rocm` | `README.md` | `f26dd12218157d893080029f1d4a66d6639ea26d`, 2026-10-03 | 2026-10-10 |
| `MTSistemi/SkillFishOS` | `docs/AI.md` | `24fa7421928d10cf1e26119ea792dc14fd12fd88`, 2026-09-11 | 2026-10-10 |
| `MTSistemi/SkillFishOS` | `website/src/content/docs/en/ai-locale.md` | `2c20e1b66c32dac35b119fddc6fffc655f44edf3`, 2026-09-13 | 2026-10-10 |
| `TechMakesArt/llama.cpp-bc250` | `README.md`, branch `vulkan-fused-gate-up` | `a15e16fc16523cef41d14ca36489e429583b5d79`, 2026-04-18 | 2026-10-10 |
| `kalpakprod/awesome-bc250` | `docs/en/12-ai-llm.md` | `91877cfe5e57d4e021980d84b6f55579527a245b`, 2026-06-18 | 2026-10-10 |

Licences of the imported text, for the record: `akandr/bc250-rocm` MIT, `MTSistemi/SkillFishOS`
GPL-3.0-only, `TechMakesArt/llama.cpp-bc250` MIT, `kalpakprod/awesome-bc250` CC-BY-4.0.
Only facts and figures are quoted here.

## 1. akandr/bc250-rocm: ROCm against Vulkan under llama.cpp

All values are tokens per second, at an empty context, flash attention on, Fedora 44, GPU
clock pinned at 1500 MHz. ROCm and Vulkan come from the same llama.cpp source tree. Each
figure is the median of eighteen samples over two pooled runs of the same campaign.

| model | size | ROCm pp512 | Vulkan pp512 | pp ratio | ROCm tg64, graph opt on | ROCm tg64, graph opt off | Vulkan tg64 | tg ratio, opt on |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| qwen2.5-1.5B Q4_K_M | 1.04 GiB | 1798.6 | 1850.0 | 0.97 | 213.1 | 196.7 | 212.3 | 1.00 |
| qwen3-8B Q8_0 | 8.24 GiB | 409.4 | 394.7 | 1.04 | 39.5 | 38.5 | 39.0 | 1.01 |
| deepseek-r1-14B Q4_K_M | 8.37 GiB | 195.8 | 199.8 | 0.98 | 33.6 | 32.6 | 35.1 | 0.96 |
| qwen3-14B Q4_K_M | 8.63 GiB | 197.6 | 204.8 | 0.96 | 33.7 | 32.7 | 34.7 | 0.97 |
| qwen3.6-35B-A3B MoE IQ2_M | 10.72 GiB | 588.8 | 457.0 | 1.29 | 70.8 | 71.2 | 86.9 | 0.81 |
| qwen3.8-27B UD-IQ3_XXS | 11.09 GiB | 102.9 | 105.0 | 0.98 | 15.1 | 15.2 | 17.6 | 0.86 |

The page's own headline decode column is the one with `GGML_CUDA_GRAPH_OPT=1`. Its words:
"**The decode column is measured with `GGML_CUDA_GRAPH_OPT=1`**". The off column is the
build as shipped, and the page lists it as 196.7, 38.5, 32.6, 32.7, 71.2 and 15.2.

Read the two columns together, not across each other. Three facts follow from them.

1. The option is worth 1.083 on the 1.5B, 1.026 on the 8B, and 1.031 and 1.032 on the two
   14B models. It is worth nothing on the MoE and the 27B, where it launches no streams.
2. Prefill is unchanged by the option to within 0.1 percent on every model.
3. The decode ratio against Vulkan is 1.00 and 1.01 on the 1.5B and the 8B. It is 0.96 and
   0.97 on the two 14B models, 0.81 on the MoE and 0.86 on the 27B.

Per-model verdicts, as the page states them:

- The 8B is the only model ahead of Vulkan on both halves, 1.04 prefill and 1.01 decode.
- The 1.5B comes closest to joining it, 0.97 prefill and level decode, 213.1 against 212.3.
- The MoE prefills at 1.29, the largest margin in the table.
- The MoE and the 27B are the two models the decode option cannot help.

Both halves improve as the context fills. Prefill at 4096 and 8192 tokens of depth reaches
1.38 and 1.49 times Vulkan. The 1.5B decode holds 1.04 to 1.07 times Vulkan from 4096 out
to 30720 tokens.

The build behind these figures is not a stock one. It carries thirteen llama.cpp patches, a
native gfx1013 rocBLAS build, and a patched `amdgpu` with TLB flush fixes and a 40 CU
unlock. `GGML_HIP_NO_VMM=ON` is required at build time. `HSA_ENABLE_SDMA=0` is set for
inference.

## 2. The `GGML_CUDA_GRAPH_OPT` trap, and the fix that exists

The fastest decode numbers above need a runtime option that computes wrong tokens as
shipped. Both halves of that sentence matter, and an earlier draft of this note carried only
the first half.

The defect, in the page's words: the Q branch overwrites `attn_norm` while the K and V
projections on the other streams still read it. The graph allocator plans memory for
sequential execution. On three streams the Q branch can write its rotated output into a
buffer the K and V projections still read.

What that does per model: qwen3-8B and qwen3-14B decode word salad, a different one each
run. deepseek-r1-14B gives a coherent reply that is not the greedy one. The 1.5B came out
byte-identical in ordinary runs and produced garbage on another build. Prefill is untouched.
Both perplexity gates evaluate prompts only, so neither gate can catch this.

The page also publishes the fix. With `alloc-deps.diff` applied and
`GGML_CUDA_GRAPH_OPT_ALLOC_DEPS=1` set beside it, the replies are byte-identical to the
default on all four affected models. None of the speed is lost, to within 0.1 percent. The
diff keeps every tensor of a region allocated until that region's join, through the
allocation-dependency hook of llama.cpp PR 27301.

The repository's own advice stays "leave `GGML_CUDA_GRAPH_OPT=1` off unless you also apply
its fix". The region code is the same in upstream llama.cpp master at the time of writing.
Whether it corrupts output on other GPUs depends on how they schedule the streams, and
nothing on that page measures it.

Two further notes from the same page. `GGML_CUDA_DISABLE_GRAPHS=1` is the workaround for HIP
graph instantiation failures at deep context on 14B models, and it also disables the
multi-stream option. `GPU_MAX_HW_QUEUES=1`, `HIP_FORCE_DEV_KERNARG=1` and
`HSA_ENABLE_INTERRUPT=0` are worth nothing on the thirteen-patch build.

## 3. SkillFishOS: what the project publishes, and what its tracker holds

### 3.1 Their own measurement

`docs/AI.md` carries one measurement, on Qwen3-1.7B Q4_K_M:

| | CPU only | GPU over Vulkan | factor |
| --- | --- | --- | --- |
| Generation | 41.5 tok/s | 210.7 tok/s | 5.1x |
| Prompt processing | 9.2 tok/s | 157.2 tok/s | 17x |

The same page states their engine choice. They run Unsloth Studio with llama.cpp on the RADV
Vulkan backend, not ROCm, because ROCm does not support gfx1013. They raise the TTM page
limit with `ttm.pages_limit=1572864 ttm.page_pool_size=1572864`, which gives Vulkan about
13 GiB of the 16 GiB. The 5.1x figure is the one their `README.md` repeats.

### 3.2 Their cluster numbers, and where they live

The cluster figures are not in `docs/AI.md`. They are on the website AI page, the file
`website/src/content/docs/en/ai-locale.md`, published at
`https://skillfishos.com/en/docs/ai-locale`. The URL `https://skillfishos.com/en/docs/cluster`
returns HTTP 404, and the repository has no cluster page under `docs/`.

What that page states, at its lines 59 to 61:

- Several boards share a model too large for one board. A 22 GB 27B model does not load on a
  single board. On two boards it runs at 7.57 tokens per second.
- The cluster is not faster. A model that did fit on one board loses about a third of its
  speed when split, because every boundary between layers travels over the network.
- The cluster exists for the models that do not run at all on one board.

The feature is real in the tree, not only in the documents. The repository holds
`apps/control-center/sfcc/cluster.py`, `system/usr/local/bin/skillfish-cluster` and
`system/etc/systemd/system/skillfish-cluster.service`.

### 3.3 Community numbers in their tracker

GitHub issue search over `repo:MTSistemi/SkillFishOS`, run on 2026-10-10. The search matches
whole words, so a word fragment returns nothing. Record the query with the count.

| query | `total_count` | items that carry an LLM number |
| --- | --- | --- |
| `tokens` | 2 | issue 14 |
| `llama` | 0 | none |
| `tok` | 0 | none |
| `ollama` | 5 | issue 14 |
| `benchmark` | 6 | none |
| `cluster` | 1 | none |
| `LLM` | 1 | none |
| `performance` | 7 | none |

The zero counts for `llama` and `tok` are an artefact of whole-word matching. They are not
evidence that the tracker holds no LLM numbers. The query that finds the number is `tokens`.

Issue 14, "AMD BC-250: Ollama falls back to CPU unless OLLAMA_IGPU_ENABLE=1 is added to
compose.yaml", closed, opened 2026-06-30. The reporter ran SkillFishOS 26.06 on a BC-250.
Ollama reported `PROCESSOR 100% CPU` and the GPU stayed near 350 MHz. After adding
`OLLAMA_VULKAN=1` and `OLLAMA_IGPU_ENABLE=1` to the Ollama service, `ollama ps` reported
`PROCESSOR 100% GPU`. The GPU clock reached about 2000 MHz. The reporter's words on speed:
"around 75-100 tokens/sec with Qwen3 4B".

That is the only LLM throughput number in their tracker. The eight queries above are the
evidence for that statement.

## 4. TechMakesArt/llama.cpp-bc250: a Vulkan fork with its own numbers

Read the right branch. The default branch is `vulkan-fused-gate-up`, and the BC-250 README
is there. The `master` branch carries the upstream llama.cpp README, whose only sample
numbers are a Metal run of qwen2 1.5B at 5765.41 pp512 and 197.71 tg128. Those Metal numbers
are not BC-250 numbers.

On branch `vulkan-fused-gate-up`, measured on Qwen 3.5-9B Q4_K_M, Ubuntu 24.04, kernel
6.8.0, Mesa 26.0.3, RADV, TTM page limit raised to 16 GiB:

| metric | stock llama.cpp, Vulkan | this fork, Vulkan | delta |
| --- | --- | --- | --- |
| single-stream tg | 37.00 tok/s | 54.99 tok/s | +48.6 % |
| single-stream pp | 199.66 tok/s | about 306 tok/s | +53 % |
| batched-32 aggregate tg | about 100 tok/s | 151 tok/s | +51 % |
| batched-64 aggregate tg | not stated | 175 tok/s | not stated |

Their step ladder for the decode figure, which matters more than the headline delta:

| step | tg64 | gain |
| --- | --- | --- |
| baseline, stock llama.cpp | 37.00 | reference |
| plus fused gate and up kernel | 39.15 | +5.8 % |
| plus Q4_K smin restructure | 41.08 | +5.2 % |
| plus RDNA1 `rm_kq=4` | 42.33 | +3.0 % |
| plus SMU governor | 54.99 | +29.7 % |

The largest single step is the SMU governor, and the page states plainly that the governor is
somebody else's project. The fork's own kernel work carries 37.00 to 42.33, about 14 percent.
The rest is a clock policy. Our KMD already owns clocks and voltage, so that 29.7 percent is
not a software win we could repeat on top of ours.

Their claimed perplexity control: Qwen3-4B Q4_K held at 1.0455 to 1.0457 with an error bar of
0.0246 across every configuration. `test-backend-ops MUL_MAT` passes 938 of 938 on Vulkan.
The fork is a snapshot of upstream commit `f772f6e43` and is not maintained.

## 5. Community numbers of a weaker grade

`kalpakprod/awesome-bc250`, `docs/en/12-ai-llm.md`, is a community survey. Its own sourcing is
Discord, Telegram and Hackaday comments. Treat every figure in this section as hearsay until
somebody reproduces it. The page itself marks some rows that way.

What it states for the Vulkan route: roughly 30 to 40 tok/s on an MoE model that fits in the
16 GiB, for example gpt-oss-20b or Qwen3.5-35B-A3B. Its CU unlock table, credited to akandr,
reads 66.1 to 87.5 tok/s on gpt-oss-20b and 59.5 to 78.7 tok/s on Qwen3.5-35B-A3B, a median
of plus 32 percent generation and plus 50 percent prefill over eleven models. It also records
Qwen3.5-35B-A3B at 25.1 tok/s through Ollama against 59.5 tok/s through llama.cpp.

The rows of interest to M16 are its HIP ones, all sourced to Discord messages. A member
reported ROCm, HIP and PyTorch running after a MEC firmware change, BIOS changes and a
rebuild of the stack. On that path llama.cpp built against HIP measured 709 tok/s on
TinyLlama-1.1B pp512 and 115 tok/s on Llama-3.1-8B pp512 at stock clocks. Prefill scaled with
the CU unlock, about 230 tok/s at 24 CU and 94 to 95 W against 371.6 tok/s at 40 CU and
125 W, both at 1500 MHz.

## 6. What this means for M16

- The decode target on this board is Vulkan, not ROCm. On the only published head to head,
  ROCm decode reaches 0.81 to 1.01 of Vulkan, and it needs thirteen patches to do it.
- Prefill is where a HIP path pays. ROCm prefill reaches 1.29 at an empty context on the MoE,
  and 1.38 and 1.49 times Vulkan at 4096 and 8192 tokens of depth.
- Any decode figure we quote from `bc250-rocm` must name the graph option state. The headline
  column and the shipped build differ by up to 8.3 percent on the 1.5B.
- A multi-stream graph optimisation on this part has a known aliasing defect, and a published
  fix. Our own graph or stream work must keep a region's tensors alive until the join.
- Nothing in this note is a Windows number. We hold no LLM measurement of our own stack yet.

## 7. How to check these numbers again

```sh
# the ROCm against Vulkan table and the graph option paragraphs
curl -sS https://raw.githubusercontent.com/akandr/bc250-rocm/main/README.md \
  | grep -nE 'tg64|GRAPH_OPT|alloc-deps'

# the SkillFishOS measurement, and their cluster figures
curl -sS https://raw.githubusercontent.com/MTSistemi/SkillFishOS/main/docs/AI.md \
  | grep -nE 'tok/s'
curl -sS https://raw.githubusercontent.com/MTSistemi/SkillFishOS/main/website/src/content/docs/en/ai-locale.md \
  | grep -nE '7.57|22 GB|third'

# their tracker, one query at a time, with the count
gh api -X GET search/issues -f q='repo:MTSistemi/SkillFishOS tokens' --jq .total_count

# the Vulkan fork, on its default branch and not on master
curl -sS https://raw.githubusercontent.com/TechMakesArt/llama.cpp-bc250/vulkan-fused-gate-up/README.md \
  | grep -nE '37.00|54.99'
```
