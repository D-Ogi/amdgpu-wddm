# M413 - Current desktop inference baseline before the Vulkan migration

Unit A, 2026-09-24. KMD0.7.127.1, unchanged boot11:44:14, M412 CPU-rendered
Mesa main/LLVM23.1.2 desktop and overlay active throughout. Vulkan remains the
old cache-intent-v2 ICD, SHA256
6B589A8686DF6FFBB2BE447845222A3607E7E162B89923FA5976FCD9FD412754.
Exact driver, application and model hashes are in the run log.
llama-bench b9564/3b3da01dc, ngl99, t6, pp512, tg128, r3,
1000MHz/820mV, shader cache disabled, same workload settings as M380.

| Model | Prompt tokens/s | Generation tokens/s |
|---|---:|---:|
| stories15M Q4_0 | 26521.39 +/- 4057.13 | 403.67 +/- 4.37 |
| TinyLlama Q4_0 | 1072.32 +/- 0.16 | 110.33 +/- 0.42 |

Both native programs exit0 and their loader traces identify the intended ICD
and BC250 submissions. JSON retains all three samples for each workload.
Final GFX10369/10369 and SDMA22293/22293, zero timeouts/refusals, no TDR.
Capture allocations33 reserved/0heap, peak1plan/5505328bytes. DWM4448 retains
start13:58:18 and responds at14:11:18; boot unchanged and guard count0.
No device reload, OS restart or power operation was performed.

The initial scheduled-task observer returned early: it saw Ready before the
TinyLlama exit marker existed. Task LastTaskResult was3221225786 (0xC000013A),
so the wrapper did not pass. Subsequent bounded inspection found TinyLlama
had continued and finished with native exit0; the finish script checked both
markers, JSON was collected, and the task was removed. No workload rerun.
The wrapper termination cause is unknown. Future observers must use native
completion markers, not task state alone. Keep this anomaly with the results.

M380 used KMD119 and a different desktop configuration. Its TinyLlama
1101.77/118.75 and stories15M48335.30/518.46 are historical comparisons,
not isolated evidence about KMD, LLVM or Mesa performance. This is the current
Windows comparison point for a per-process Vulkan ICD migration. It does not
prove Windows superiority over Linux or complete M9 acceptance.

Raw text is decoded to UTF-8 with original newlines preserved; only PCI and
interface identities are redacted. SHA256SUMS covers all files except itself.
