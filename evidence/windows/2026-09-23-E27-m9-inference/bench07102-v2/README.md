# M321: current07102 inference baseline

Same07102 session after M320, no GPU reinitialization or reboot. Exact isolated cache-intent-v2 ICD6B589A8686DF6FFBB2BE447845222A3607E7E162B89923FA5976FCD9FD412754, native exits0, loader/actual-submit witnesses, full JSON validated. b9564/3b3da01dc, Vulkan,ngl99,t6,pp512/tg128,r3, Q4_0. Global ICD unchanged.

TinyLlama1082.322790+/-31.673333 prompt tokens/s and115.061026+/-6.572937 generation. stories15M36012.205752+/-14862.687881 and652.405536+/-36.293503; prompt variation is large. M274 Windows TinyLlama1099.248647/116.791281; this single measurement does not establish a source-caused regression or improvement. E14 Linux1119.59/154.92 at1000MHz gives current differences-3.33%/-25.73%. Linux reports BLAS,Vulkan; baseline text omits some defaults. Windows-over-Linux target remains unmet.

Finalgraphics16960/16960,paging425844/425844,zero timeouts/refusals/noTDR.20:36:14deviceOK,1000MHz/VID116,71.0C,boot19:27:05unchanged; scheduled task removed. Performance workload is not model-output correctness proof; M318/M320 provide separate bounded correctness evidence. Original JSON/native outputs retained; private profile paths redacted if present.
