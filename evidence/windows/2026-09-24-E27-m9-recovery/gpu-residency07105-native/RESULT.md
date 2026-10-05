# M337 - Full1GiB GPU readback on reserved capture storage

Unit A,2026-09-24, same boot03:35:18/full0.7.105.1 as M336. Both scripts verify exact installed SYS40D7E3790C8168A08B5BF21379EDC49D3B6288B477DBBEFFCED422E64C187008, loaded0x00070069, healthy device, unchanged M320 probeE9566E495A4FB5BB0F37B8A2583AB10F573D0F9ECEE2B84D94E264242529E9C3,1000MHz/VID116 and temperature<85C. STOP clear, overlay notified. No restart or AC cycle. Host sessions75194/control and91079/large both terminal0.

64KiB control passes four full reads and three residency2->1 cycles.1GiB passes four full word-oracle GPU DMA_DATA reads with fences1024/2048/3072/4096 and three residency3->1 cycles. Sentinel initialization precedes each GPU copy. Restore times6297/6250/6235ms. Original300s watchdog unchanged; large summary timestamps294.044->511.454seconds. Independent native-file validator passes.

Large reserved capture totals26->50,heap0->0:24new reserved plans. Graphics38->4134submitted/completed; paging12899->180716submitted/completed; no timeout/refusal/TDR. Control totals20->26,heap0. Totals are adapter-wide, so background OS work can contribute. Extract the LAST summary in each snapshot; an initial extraction assertion requiring only one summary was corrected because the control after-log retains both earlier and final summaries. Raw evidence is unchanged.

This supplies runtime reservation use at1GiB scale. Explicit eviction/competing demand is not automatic oversubscription or arbitrary PFN relocation proof; DMA_DATA readback is not shader-cache proof. General concurrent OS delivery, busy/oversize fallback guarantees and warm GPU reentry remain open. No inference-performance superiority claim. Lab remains full105 in the same boot.
