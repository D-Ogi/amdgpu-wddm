# Snapshot provenance clarification

Source inspection at exact KMD166 revision17989843012e8e2c54de75204418b20e25ab0143:
DcnLogVsyncSnapshot (driver/kmd/dcn.c:899) is invoked by WddmSummary
(driver/kmd/wddm.c:1658), through the HardwareAccess summary escape.
CollectDbgInfo only increments its counter and zeros the caller buffer.

Consequently the2.091889s gap in M709 ends at an escape snapshot. It is not an
established OS timeout-detection timestamp or a proven pre-recovery interval.
The phrase "at diagnostic collection" in the original result denotes diagnostic
logging, not attribution to CollectDbgInfo. Root cause remains open.

ISR entry updates InterruptLastTime before calling IH/DCN handlers (pnp.c:249).
DcnVsyncEntryTime updates before the DCN gates (dcn.c:936). Omitting a later DPC
VSync report cannot alone explain both stale entry timestamps.
