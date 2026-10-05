# M663 - Exact164 deployed, native GPU copy regression passes

Unit A2026-09-27. Exact164 source933f383/SYS9B9B99D3 from M661. Transition source2df6726. Worker8448/start11:50:51.4026584Z, installer10048/start11:50:51.9980162Z; worker terminal exit0 at11:51:13.4836756Z. CPU DWM2016 and OS boot retained. Monitor independently confirms health15/guard0; no manual new-generation confirmation.

Collector12560/start11:50:51.9612729Z reaches terminal11:53:52.6024938Z,160 samples, no reader timeout. Original processes absent, transition task removed at11:55:00Z. Full private archive scratch/g0-hosted/kmd164-transition/transition-receipts.zip. No reinstallation after observation timeout.

Unchanged native executable0821C9BDA9A2D72AEF7B8C3185D0ACB729F258A570BA26D3B3031E8DBA604963, source4633abe, run010 dirty-list mode:5 GPU-copy cases pass with zero pixel mismatches,30 residency controls pass, fence34. Exit0, process absent, native task removed. Native closure11:54:22Z: CPU2016, same boot, health15,1000MHz/VID116,66.625C. Source/destination/IB residency and actual copied content are covered by this BC2S control; BGP1 Present wrapper/ACQUIRE_MEM and desktop composition are not covered.

Experimental GPU Present/CDD interop gates remain0; baseline UMD8279AC7F/registered ICD93B1D1FD retained. Exact163 rollback is staged at C:/BC250/m12/candidate07164/rollback163; older162 retained. Diagnostic deployment only, no G0 acceptance. Next is reviewed DWM029180-second trial with genuine BGP1 admission/retirement and separate content proof.

native-results.txt selects unchanged result/residency lines from the private complete stdout; other tool output is omitted, not rewritten. No private device identifiers are included.
