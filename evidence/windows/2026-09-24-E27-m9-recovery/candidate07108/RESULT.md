# M344 - RLC busy transition bracketed by first retirement GFXHUB flush

Unit A, candidate0.7.108.1, SYS
F0F2AAE21FFF726E86B4262B552CD39CE9BC254BB039CEF796253D5FC2901775.
Host acceptance: WDK26100 build,319 startup checks,1676 current stop-chain
checks and25 package checks passed. Historical negative control still fails889
of1676 checks as expected. MMIO observation is mocked in the stop fixture;
these tests do not establish hardware register semantics.

Closed-gate install passed in boot04:30:34. Normal shutdown succeeded; after30s
all configured SSH endpoints were unavailable, then one verified OFF/8s/ON
restored AC. Pinned SSH returned in boot2026-09-24T04:46:20. Explicit guard
confirmation preceded opening gates. First full start passed; unchanged64KiB
probe passed3residency cycles/four complete GPU word readbacks, graphics4/4,
paging1024/1024, no timeout/refusal/TDR. No repeated full warm start was attempted.

Stop into display-only passed. Raw record ring-20260924-024937-710.log:

| Boundary | RLC_CNTL | GRBM_STATUS2 |
|---|---|---|
| after-stop / before-gfx-teardown | 0 | 0x00000008 |
| first GTT entry37,4096bytes: before/after unbind | 0 | 0x00000008 |
| after first GFXHUB flush | 0 | 0x01000008 |
| after first MMHUB flush and all later retirements | 0 | 0x01000008 |
| before/after physical memory release, PSP unload, GART restore | 0 | 0x01000008 |

All observation reads returned status0. The first transition is between the
post-unbind and post-GFXHUB-flush observations, before physical memory release.
This supports the planned interval hypothesis, not causation by an individual
MMIO access, outstanding DMA, or a proven reset remedy. Diagnostic timing and
reads remain confounders. PSP unload succeeds;282 GART restore writes;stage79.

A relevant counterexample: successful startup after-PSP observation has
CNTL1/STATUS2 0x01004008, including RLC_BUSY, whereas before-PSP is0/0x8.
Thus busy alone is not a failure discriminator; enable state and lifecycle
matter. M341 failed warm state was CNTL0 with busy set before and after PSP.

Next inspect AMD gmc_v10_0_flush_gpu_tlb's RLC_NO_KIQ access wrappers and its
GFXOFF/KIQ gating, then compare retirement ordering. Do not omit a required
TLB invalidation or free live pages merely to hide the busy bit. Local reference
P:/BC-250/ref/linux-amdgpu/gmc_v10_0.c distinguishes firmware-mediated ready-KIQ
flush from the direct pre-KIQ path; wrapper semantics require expansion before
claiming equivalence to our RREG32/WREG32 shim. No new reset sequence is added.

Power telemetry was read locally during this run (candidate07108-power-start.json).
Scale uses bundled TinyTuya SocketDevice convention; exact-model calibration and
sample freshness are unverified. Power is whole-lab auxiliary data, not a liveness
or GPU-completion witness. Local owner documentation and plug.py now expose the
read-only telemetry command. No credential or identity data was copied.

Current lab: healthy108 display-only, all execution gates closed, same04:46:20
boot, plugON. One controlled AC cycle only. Broader M9 requirements remain open.
Host control log PCI instance strings were redacted; raw KMD and probe records
are unchanged. Source snapshots/manifest identify this dirty worktree build.
