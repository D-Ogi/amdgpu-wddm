# M338 - Inference baseline on105

Unit A, same boot03:35:18/full105 session after M337, exact installed SYS and loaded revision verified. STOP clear, overlay notified,1000MHz/VID116 and monitored temperature below85C. Hostsession64154 terminal0, native exits0 and scheduled task removed. No restart.

b9564/3b3da01dc Vulkan,ngl99,t6,pp512/tg128,r3 with shader cache disabled. Actual loader/submit witnesses and exact cache-intent-v2 ICD6B589A8686DF6FFBB2BE447845222A3607E7E162B89923FA5976FCD9FD412754 verified. Every non-result JSON field matches M321, including model geometry and benchmark defaults. Current executable/model SHA256s are recorded in run.log; M321 did not record those hashes in this comparison directory, so byte-for-byte identity of the earlier executable/models is not independently proved by this comparison.

TinyLlama prompt1101.128220+/-4.929448 tokens/s, generation118.277614+/-6.240878. M3211082.322790/115.061026: about+1.74%/+2.80%, not a causal improvement claim. Generation sample ranges overlap. stories15M prompt48980.311409+/-83.507418 and generation533.487457+/-115.862142; previous36012.205752+/-14862.687881 and652.405536+/-36.293503. Large variation prevents assigning its mean change to this driver revision. No regression absence claim beyond these measured cases.

Counters: graphics4134->14499submitted/completed; paging189303->211989submitted/completed; no errors/TDR. Reserved plans50->75,heap0. UMD function elapsed1597743ticks at10MHz across10365calls =159.7743ms total,15.414us/call. This excludes UMD/userspace, scheduler delivery before the function, completion latency and GPU time; it cannot identify the whole bottleneck. Optional probe disabled.

Historical Linux TinyLlama1119.59/154.92@1000MHz is still faster, particularly generation. Baseline configuration caveats from M321 persist; no matched current Linux rerun. Benchmark is throughput evidence, not numerical-model correctness. Same-session paging/shader correctness is separately M336/M337. General concurrency/cache and warm GPU reentry remain open. Lab remains full105.
