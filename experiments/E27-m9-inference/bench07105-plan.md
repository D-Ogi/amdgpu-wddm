# M338 - Inference baseline after capture reservation

Hypothesis: accepted105 retains the measured102 inference throughput within run variation for matched binaries/settings. This is a regression baseline, not an isolated causal A/B experiment or proof of Windows superiority.

Same GPU session as M336/M337. Run M321 llama-bench b9564 on stories15M and TinyLlama using unchanged cache-intent-v2 ICD6B589A8686DF6FFBB2BE447845222A3607E7E162B89923FA5976FCD9FD412754. Settings ngl99,t6,pp512,tg128,r3,JSON verbose, shader cache disabled. Verify native exits, actual loader/submit witnesses, exact benchmark/model hashes and JSON settings against retained M321 evidence. Verify105 loaded/installed identity, clock1000MHz/820mV, STOP and temperature<85C. Keep raw output/process handle, no restart on timeout. Capture before/after summaries.

Compare sample distributions and report workload history/configuration caveats. Benchmark throughput alone does not validate model numerical correctness or close warm reentry. No Linux superiority claim without equivalent Linux run and settings.
