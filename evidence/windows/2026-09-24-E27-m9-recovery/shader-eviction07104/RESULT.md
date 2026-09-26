# M329 - Shader eviction/content acceptance on07104

Unit A,2026-09-24, same boot01:46:31, loaded0.7.104.1/fulltable0x00070068.
Same probe038010EB3E6220A12F66713BEC9B9DCC527646DA1AAE5E7D3880AADA2F467D9A,
ICDEA70D44045BF77867C9BE50EFB311E7CE7C83D01F4B527FB190E5BC8BD3F8306 asM318.
STOP clear, overlay notified,1000MHz/VID116, pretemp70.1C and monitor below85C.
Host session39454 terminal0; task removed. No GPU/PnP/OS restart.

Independent native-file validator passes. Baseline16rounds and eviction16rounds
match CPU word oracle (1048576words/round, three4MiB buffers/type3). Three successful
eviction/residency restoration cycles; stale-input control passes baseline and
fails round1 with nativeexit1, after its own cycle. Loader and actual-submit
witnesses checked. Graphics0->34submitted/completed; paging12053->15920, both
completed,0timeouts/refusals/noTDR. No performance comparison from these timings.

Capture reservation creation remains in the retained first256 log lines. Neither
reserved nor heap capture-use line survives in before/after snapshots; afterlog
21497lines with20473lost to ring wrap. This does not prove zero captures or that
reservation was used. Persistent aggregate path counters are needed for reliable
later acceptance. Do not repeat this same workload solely hoping a lost line
reappears. Content/residency path is accepted here, not all alias graphs, cache
attributes, OS PFN relocation, warm reentry or the full resource/status contract.
