# Candidate07100 cache-intent v2 runtime, unit A, 2026-09-23

Same healthy device session as M271/M272, no reinitialization or reboot. KMD0.7.100.1, isolated ICD6B589A8686DF6FFBB2BE447845222A3607E7E162B89923FA5976FCD9FD412754 selected by Limited interactive task with actual loader and submission witnesses. Task completed and was removed. Final boot15:39:09 unchanged,deviceOK,1000MHz/VID116,70.8C. GlobalquietICD unchanged. Text decoded toUTF8; private adapter instance suffixes redacted only.

b9564 (3b3da01dc), Vulkan,ngl99,t6,pp512/tg128,r3. TinyLlama1099.248647 +/-5.434090 prompt tokens/s and116.791281 +/-4.460732 generation tokens/s. stories15M48516.719722 +/-904.834383 and533.422929 +/-126.853078; generation spread remains high. Native exits0, JSON settings/sample counts/backend verified. FinalGFX12923/12923,paging72525/72525,zero timeouts/refusals/noTDR;temperature70.9..72.0C.

Previous Windows M255 (0798/legacy ICD) TinyLlama1092.70/113.88. E14 Linux dpm/bench-1000.txt1119.59/154.92; current Windows remains about1.82%/24.61% slower. Linux reports BLAS,Vulkan, Windows Vulkan; baseline text omits some defaults. This single r3 run is not proof that cache policy caused the small change. Matched1000MHz stories15M Linux baseline is not established. No Windows-over-Linux performance claim.
