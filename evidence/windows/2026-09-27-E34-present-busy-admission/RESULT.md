# M660 - Bounded busy admission for GPU Present

Source parent fce08a4. M659 found BGP1 refusing ordinary back-to-back work because its outer gate required an idle ring. WddmSubmitPresentHardware now accepts the existing ready-or-busy state, retires observed GPU fences, and retries temporary pressure for at most the500ms submission deadline before preserving existing failure/recovery semantics. Same-root jobs can use the existing bounded pending queue. Root-switch and capacity checks remain in the lower layers; no completion is synthesized.

Host tests extract the actual Wddm submit/completion/watchdog/helper functions. Two jobs without an intervening completion, delayed root-switch admission, full7-slot queue releasing after a real completion, finite no-progress timeout and closed-device refusal pass:1598 checks. Ring capacity/fence ordering passes10025 checks. Restoring idle-only admission in the extracted helper is a negative control:14 assertions fail, including the second burst job. The generator verifies that BGP1 calls the tested helper.

All13 quick quality gates pass through tools/quality/quick.cmd with MSVC environment; real WDK compilation of changed wddm.c passes separately. The initial direct quick.ps1 call lacked cl on PATH and stopped before its compiler controls; the supported wrapper resolved that environment problem. No source gate was relaxed.

The combined older pipeline runner also attempted its independent RADV gather test, whose mock no longer models hosted fence validation/dispatch and fails to compile against the current Mesa source. Added explicit -KmdOnly to isolate these KMD controls without claiming a RADV gather pass. Default still includes that test and reports its failure. This test-model gap is unrelated to the KMD change and remains to be refreshed.

Commands (workspace P:/bc-250):

- pwsh -File bc250-win/driver/kmd/test/run_gfx_pipeline.ps1 -Root P:/bc-250 -Out scratch/g0-hosted/present-busy-test002 -KmdOnly
- Same command with a new output path and -IdleOnlyPresent: expected exit1, observed1.
- bc250-win/tools/quality/quick.cmd P:/bc-250 P:/bc-250/scratch/g0-hosted/present-busy-quality002
- Exact saved WDK compile arguments in private scratch/g0-hosted/present-busy-compile001/arguments.json, output copied here.

Host validation only. No new artifact deployed; unit A remains on exact163 with both experimental gates0 and baseline CPU desktop. Before another trial: build isolated candidate from exact163 plus this correction, harden durable backups and gate-first rollback, review the exact artifact. BGP1 copied content and full G0 still require hardware evidence.
