# Flash Attention consumer controls, 2026-10-11

Status: MEASURED. This record extends the allocation-cap controls. It does not change their original output.
The new HIP backend is FC5B2D65. The earlier FB0E315E backend disabled GPU Flash Attention.
Both use the measured allocation cap. The new build enables GPU Flash Attention.

## Artifacts and build contract

The consumer derives from llama.cpp b86d2f07542b29ab099aed34fd6b6d1b2fd4b81c with the reviewed HIP patches.
Reference tools use upstream b11539, commit 10a60cf30. Cross-backend results compare those two stacks.
Exact artifact hashes are in [fa-controls.json](fa-controls.json).
The candidate manifest covers 16 files and 144 HIP translation units without GGML_CUDA_NO_FA.

The builder enables GGML_CUDA_FA and removes inherited quantization overrides.
It rejects compiler flags that disable FA, including flags retained in the output cache.
ValidationTools enables COMMON, TESTS and TOOLS and builds four validation clients.
The source's CMakeLists.txt gates the tests and tools on LLAMA_BUILD_COMMON.
The measured candidate already has that option enabled.
The final builder explicitly enables it when the input cache disables it.
Five offline controls pass in Windows PowerShell 5.1. See [fa-builder-controls.json](fa-builder-controls.json).
These controls invoke the actual script through a recording CMake fixture. They run no compiler or GPU.

## Supervised FA window

[fa-controls.json](fa-controls.json) retains selected records for all 30 collected attempts.
Twenty-seven pass supervision. Three timing attempts fail their preflight or postflight timeout.
The quiet baseline writer passes supervision but lacks the required runtime identity output.
Its numerical verdict remains invalid. A later verbose writer supplies the verified baseline.
Both HIP readers retain their original invalid analyzer verdicts alongside separate reviewed measurements.
Their exact memory-fit advisory is unrelated to incomplete execution. All eight chunks complete.
No original run file was changed.

Eight FA operation cases pass against CPU at NMSE tolerance 5e-4.
They cover six distinct shapes, with head sizes 64, 128, 256 and 512.
The two smaller shapes repeat. The larger shapes cover masked GQA at query counts 1 and 512.
Gemma's 14 diagnostic graphs each assign 48 FA nodes to ROCm0.
These rows include reserve graphs. They do not count executed kernels.

TinyLlama and Gemma return accepted templated answers with FA on and off.
TinyLlama emits `Capital: Paris` in both modes. It passes the Paris-answer rule but fails strict one-word formatting.
Gemma closes its thought section and returns `Paris` in both modes.
These are short-answer controls, not general quality guarantees.

All 16 timing measurements complete. Each cell below is one sample in tokens/s.
Batch and microbatch are 512, with six threads and a full-offload request.
Automatic DPM remains active. The samples do not establish variance or equal clocks.

| Model | Phase | HIP FA on | HIP FA off | Vulkan FA on | Vulkan FA off |
| --- | --- | ---: | ---: | ---: | ---: |
| TinyLlama | pp512 | 2215.877542 | 1862.431768 | 3417.794695 | 3118.084035 |
| TinyLlama | tg128 | 173.531369 | 98.796626 | 305.517843 | 294.126636 |
| Gemma 12B | pp512 | 204.256791 | 210.304078 | 256.380905 | 256.806011 |
| Gemma 12B | tg128 | 31.336985 | 18.470915 | 39.673112 | 38.921984 |

The first Gemma comparisons use the lab Vulkan baseline.
HIP FA on/off record mean KLD 0.376236/0.381159 and top-token agreement 77.206%/76.863%.
The Vulkan self-control records mean KLD 0.000000 and 100% agreement.
That discrepancy alone does not isolate an FA defect.

## Independent whole-model comparisons

The operator supplied follow-up CPU, CUDA and Vulkan comparisons after the supervised window closed.
[fa-kld-crosscheck.json](fa-kld-crosscheck.json) retains exact statistics and hashes for 11 reader logs.
Each has rows 1 through 8 and complete final statistics, with 2040 scored positions.
Each chunk uses context 512. The WikiText-2 prefix and model hashes are recorded there.
Quiet readers lack exact loaded-module witnesses. Their artifact labels follow the operator's report and script.
The earlier window's full preflight, postflight and closure proof does not extend to these later runs.

| Model | Baseline | Reader | Mean KLD | p99 KLD | Same top token |
| --- | --- | --- | ---: | ---: | ---: |
| TinyLlama | RTX 4090 CUDA | Unit A HIP, FA on | 0.003074 | 0.032378 | 97.108% |
| TinyLlama | RTX 4090 CUDA | Unit A HIP, FA off | 0.003252 | 0.026552 | 96.569% |
| TinyLlama | RTX 4090 CUDA | Unit A Vulkan | 0.001973 | 0.009585 | 96.716% |
| TinyLlama | RTX 4090 CUDA | RTX 4090 Vulkan | 0.000648 | 0.005081 | 98.529% |
| Gemma | CPU | RTX 4090 CUDA | 0.152788 | 3.145589 | 88.676% |
| Gemma | CPU | RTX 4090 Vulkan | 0.223034 | 3.703499 | 84.755% |
| Gemma | CPU | Unit A HIP, FA on | 0.262424 | 5.011842 | 81.716% |
| Gemma | CPU | Unit A Vulkan | 0.382391 | 6.664873 | 78.382% |

TinyLlama closes this bounded whole-model check in both HIP modes.
Both meet the selected mean-KLD guide below 0.01 and top-token agreement above 95%.
This acceptance applies to these model bytes, text, context and configuration.
It does not establish universal model correctness or explain the larger HIP tail.

For Gemma, no backend agrees under those guides. It is not a correctness gate for this candidate.
Even RTX 4090 Vulkan against CUDA at the same source revision records mean KLD 0.186044.
The discrepancy is not unique to HIP. Its cause remains unresolved.
These comparisons do not prove the absence of a HIP defect.
The claim that instruction-model sensitivity explains the difference remains a hypothesis.
Gemma reference logs also contain tokenizer warnings. They are not warning-free controls.
Original writer PPL and reconstructed quantized-baseline PPL are distinct statistics.
The source clamps and quantizes the stored distribution, so those values are not interchangeable.

## Closure and remaining scope

The supervised window ended at 05:01:44 UTC with no owned workloads or trial tasks.
Health flags were 15, with unchanged installed artifact hashes and the same boot.
The final sample reports idle 500 MHz and Tctl 70.4 C.
The selected closure fields and source hash are in fa-controls.json.
The queried System events 41, 1001 and 4101 were absent from 04:08:56 UTC through closure.
This record makes no equivalent closure claim for the later operator-provided comparisons.

The full 5325-case FA inventory, 2864 MUL_MAT cases and 214 SOFT_MAX cases remain unrun.
Six passing FA shapes do not certify that inventory.
The integrator owns promotion into the release. This change does not modify installed artifacts.

The exported JSON contains selected metrics and technical hashes.
Account and machine identities, absolute paths and run UUIDs are omitted.
Model weights, baseline binaries and full private logs remain outside the repository.
