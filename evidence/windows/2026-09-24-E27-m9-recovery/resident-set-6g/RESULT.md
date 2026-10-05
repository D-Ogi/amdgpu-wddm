# M428 - Six GiB resident GPU working set with full data validation

Unit A,2026-09-24, KMD0.7.133.1/SYS37A52F95CD90726D909FBF273D55B9336D766E2997668BA713B8ADC45BCF4A87.
Probe v6 SHA256 A8F942D3C9ACC41E4B29A3F4EA3478466F95E2617568BE7FDBFBC4A04F31C530.

The new resident-only mode keeps six1GiB allocations alive and MakeResident
references held together before filling or reading any member. No member is
evicted/freed during validation.103 complete-set queries report all six in
GPU-memory status1:6442450944bytes GPU,0shared,0nonresident,618 individual
allocation statuses. QueryVideoMemoryInfo reports6442450944bytes local usage;
its11889854464byte budget is an accounting limit, not proof of physical VRAM.
Every word of each1GiB member passes GPU DMA readback with distinct global word
indices;6144 real GPU submissions/fences cover6GiB. No reused1GiB working-set
substitute: all six remain resident in every sample through the final read.
The evidence samples placement; it is not continuous physical-residency tracing.

The64MiB complete-set positive control and legacy64MiB three-cycle/four-readback
regression also pass. W4/WX build and help smoke pass. Validator checks every
member at every sample, total bytes, readback coverage/fences and lifetime order.
Changing one intermediate member's status to shared is rejected even when the
aggregate PASS line is unchanged. Exact v6 source is preserved; v5 preliminary
binary remains in private scratch, identified by hash in validation.json.

Preliminary v5 tried a single6GiB allocation and received STATUS_INVALID_PARAMETER
at CreateAllocation2, before any GPU/residency sample. It is preserved as FAIL,
not hidden by the successful multi-allocation result. Parser reads64-bit sizes;
this observation does not establish the precise rejection cause or a general
maximum single-allocation size. A live-output tar pull failed while the test was
running; independent SSH/progress observation confirmed the same process alive.
Completed artifacts were collected afterward, with no repeated workload.

Final GFX6784/6784,paging16614/16614,zero timeouts/refusals,noTDR,zero capture plans.
Boot18:23:50 and DWM2036/start18:24:54 retained through18:52:02; M412DLL hash matches,
66.9C, full table, guard0,Full0 consumed,SDMA controls0,display gates1.
Clocks1000MHz/820mV checked before each workload. No OS/device/DWM/AC restart in
these probe trials. Pagefile32GiB from M427; commit limit42655780864bytes.

The requested12GiB (75% of physical16GiB) remains OPEN.6GiB is37.5% of physical
memory and is only75% of the current8GiB carve-out. The app segment is about7.71GiB.
Neither the32GiB pagefile nor a budget above8GiB enlarges the dedicated segment.
Own-unit BIOS extraction/UMA analysis is queued separately. No firmware change.
OS-boot one-shot persistence and the other full M9 acceptance gates stay open.

Microsoft sources used locally: WDK26100 QUERYALLOCATIONRESIDENCY, residency enum
and QUERYVIDEOMEMORYINFO in ref/ddi-display/d3dkmthk.md; conceptual docs
residency-overview.md and process-residency-budgets.md at110f60ea. Residency list
requirements support retaining all MakeResident references; query samples and
actual readback provide the measured evidence. Raw logs preserve encoding; only
irrelevant PCI instance/interface identifiers were redacted.
