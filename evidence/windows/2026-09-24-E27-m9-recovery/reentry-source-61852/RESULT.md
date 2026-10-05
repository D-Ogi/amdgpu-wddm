# M385 - Upstream6.18.52 and Alpine recipe pinned for reentry work

Source-only review2026-09-24. E28 runs6.18.52-0-lts; previous local upstream
checkout is6.18 commit7d0a66e4bb9081d75c82ec4957c50034cb0ea449. Acquired
upstream gregkh/linux v6.18.52 files, exact URLs/hashes in retrieval.json and
reset-dispatch-retrieval.json. Raw sources preserved, including prior versions.
PROVENANCE: upstream Linux amdgpu driver sources, MIT; original headers retained.
Alpine packaging recipe is from alpinelinux/aports (see recipe source metadata).

Initial byte-preserving diffs included CRLF/LF differences. Use *.normalized.diff
and normalized-comparison.json for text changes; original files were not altered.
The first selected-function extraction used the wrong name soc15_reset_method;
corrected to actual soc15_asic_reset_method before writing the final comparison.

Selected-functions.json compares23 startup/stop/reset function bodies.22are
unchanged, including PSP ring lifecycle, PSP firmware-load ordering, GFX10 RLC
stop/reset/start/resume and CP resume, SDMA5 reset primitive and queue reset,
and SOC15 reset selection. amdgpu_sdma_reset_engine adds an SR-IOV VF rejection;
the normal function path is otherwise unchanged. This is not a new working
warm-reentry sequence to import.

Relevant surrounding differences:
- SDMA5 supported_reset now permits PER_QUEUE when notVF and ring reset is not
  disabled by debug flag; the older code additionally gated IP versions and
  firmware version>=35. GFX10 also respects the debug-disable flag.
- amdgpu_job_timedout checks supported PER_QUEUE and callback before calling
  amdgpu_ring_reset; failure then allows whole-GPU recovery. Module unload/reload
  is a distinct path, not evidence that this queue-reset branch executed.
- amdgpu_device_fini_hw moves power/clock ungating earlier, before disabling
  interrupts; it previously occurred in ip_fini_early. Exact runtime ordering
  matters when comparing retirement, but this diff alone proves no unitA fault.
- PSP command fence polling minimum delay changes10to60us. Protocol/firmware
  ordering in the selected functions is unchanged. Do not tune Windows delays
  based on this alone: earlier commands completed even during warm failures.

Alpine3.24-stable head had already advanced to6.18.53. Pinned the6.18.52-r0
recipe at09a165f4c951edf370eddde03b2d1d5fd71805ed instead. Its7downstream
patches and x86_64config were fetched at that commit and matched recipe SHA512.
No downstream patch touches drivers/gpu/drm/amd/. See alpine-provenance.json.
GitLab recipe fetch returned418; official GitHub mirror succeeded. Recipe
provenance plus version match is not a binary reproducibility proof for the
installed module; retain module/package identity during the next Linux session.

Next Linux comparison must record supported reset masks/debug configuration,
actual timeout->ring reset->SDMA callback entry/return, and original named reset
readbacks while backing storage remains owned. First-load content control and
post-reset real completions are required. A mask, callbackreturn0 or successful
stop readback is not acceptance. Also record ungating versus interrupt teardown
if retirement is tested. Do not repeat known failed full reload unchanged.

No lab action or driver/source behavior change in M385. Current initialized119
session remains retained; local120 candidate remains undeployed. Warm reentry,
resource bounds, CPU cache attributes/PFN lifetime and performance gates remain
open. This removes a reference-version gap, not the hardware blocker.
