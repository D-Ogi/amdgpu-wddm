# M335 - Adapter-lifetime capture counters

Two atomic 64-bit totals in BC250_WDDM count successfully attached new plans using reserved storage or heap allocation. Incrementing is after capture admission and before multipass output. Continuations, terminal repeats and owner release do not add counts. WddmSummaryOf reads each total atomically and prints them on demand and at stop. Totals live until adapter teardown; the pair is not a transactional snapshot under concurrent work. Existing owner counters and early trace lines remain.

The actual-source KmdRouting suite passes 329165 checks, zero failures. Twelve added checks cover once-per-plan counting through multipass/terminal repeats and persistence after context capture storage is drained twice, using the existing reservation-reuse and interleaved heap-fallback fixtures. The WDK26100 build and package signing pass. Development SYS SHA256: 6707DBAF0C6E30FEBC95ED6E8AD036FCE5F043E46467750A9C742ADC17F0C389. This is an undeployed development build with the existing INF version, not a new installed candidate.

Host tests exercise the actual counting branches, not Windows callback concurrency or runtime summary delivery. M329's missing reservation-use evidence remains missing until a candidate containing these counters is deployed and the summary is captured after GPU paging work. No lab action or reboot occurred for this change.

## RLC reference finding

Pinned local AMD reference gfx_v10_0.c defines gfx_v10_0_rlc_reset with GRBM_SOFT_RESET.SOFT_RESET_RLC assertion/deassertion and two 50us delays. It assigns that function to RLC callback tables. The ordinary gfx_v10_0_rlc_resume path calls stop and start, without reset; the inspected generic amdgpu*.c sources contain no RLC reset invocation. Older gfx_v6/v7/v8 sources do invoke their reset callback. This source review does not establish applicability of a new reset on Cyan Skillfish or prove what the running binary does. It therefore does not justify adding this operation as an established Linux reentry fix. E28 remains a failed Linux reload despite a witnessed RLC stop.

Next startup investigation: compare PSP firmware-command results and RLC entry state between first load and reload, tied to the running module. Existing register tracing does not expose PSP shared-memory command payloads. Any reset experiment requires its own explicit hypothesis and reference-derived sequence; it is not a completed repair.
