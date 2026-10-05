# M383 - Same-allocation buffer alias visibility passes on119

Measured unit A, initialized0.7.119.1, same boot2026-09-24T08:30:00,
1000MHz/820mV. No driver transition or reboot. Native probe SHA256
6D69F6D4D77E702B6ABF0FA24B57F799A0CA88C26B3C290BC3A3C54B986FF76D;
cache-intent-v2 ICD6B589A8686DF6FFBB2BE447845222A3607E7E162B89923FA5976FCD9FD412754.
Actual loader path and GPU-submit witnesses present in each stderr.

Baseline: three independent4MiB coherent type3 buffers, two hash dispatches with
a global shader-write/read memory barrier,16changing input rounds, all CPU/GPU
results equal. Alias: first dispatch writes one intermediate VkBuffer; second
reads through a distinct VkBuffer bound to the same VkDeviceMemory at offset0.
Identical buffer requirements checked;16rounds all CPU equal and equal to baseline.
The alias borrows memory ownership and is destroyed before the owning buffer.

Negative control: reader bound to separately allocated, deliberately poisoned
intermediate memory. Round0 GPU45d18ff3e74b52df differs from CPU6747a9c9b5e067b5;
nativeexit1,completed0/mismatches1 as required. Positives each nativeexit0.
The host wrapper and final independent CLI info/confirm exit0; hidden Limited
task removed. All raw stdout/stderr/native exit files and before/after logs saved.

After workload: GFX13298/13298,paging183579/183579,zero timeouts/refusals,noTDR,
114reserved captures/0heap. Exact before/after values in counters-validation.json.
Initial independent parser expected uppercase YES while native output uses yes;
corrected read-only validation passed without rerunning the hardware workload.
run.log/final.log redact irrelevant hardware/interface identity lines and PCI
instance identifiers; native probe logs are unmodified.

Scope: shader visibility through two Vulkan views of the same allocation with
proper synchronization. This does not prove distinct GPU virtual addresses,
distinct CPU mappings/cache attributes, general physical alias dependencies in
paging transfers, eviction of the aliased allocation, or warm reentry. These M9
acceptance gates remain open. Installed119 initialized session is retained;
local120 capture-pressure candidate remains undeployed.
