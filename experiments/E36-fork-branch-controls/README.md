# E36: controls for the consolidated Mesa fork branches

Hypothesis: the two consolidated branches of the Mesa fork (one base, 05e6c962;
RADV/ICD on `amdgpu-wddm/radv-wddm2-consolidated`, D3D10 UMD on
`amdgpu-wddm/d3d10umd-consolidated`), built clean with the recorded recipes,
behave like the lab candidates they were assembled from: the E14 compute smoke
and the M546 flip-model regression pass with the fresh artifacts.

Scope: development PC builds (`tools/build/build-mesa.ps1 -Config radv|zink-umd`),
two bounded lab controls with the M538 and M546 procedures, baselines restored.
No DWM probe, no promotion, no KMD change, no rename of `bc250_` identifiers.

Procedure:
1. Worktrees of both branch heads, clean configure and build, recipe.json kept.
2. Lab state check (STOP flag, running tasks, baseline hashes) before the window;
   the other agent notified of the window and its end.
3. E14 smoke: candidate ICD in the registered path, `run-smoke.ps1` with the
   smoke007 configuration and the candidate hash, baseline restored in `finally`.
4. Flip control: `run031.ps1` with the artifact paths changed, launched as an
   interactive scheduled task, three overlay captures, ROI analysis as in M546.
5. Evidence pulled as one archive; images kept out of Git, hashes and ROI counts in.

State: run on 2026-09-27 (fact M553). Both controls pass; the branches were
pushed to the fork afterwards with tags `...-consolidated-2026-09-27b`.

Evidence: `evidence/windows/2026-09-27-E36-fork-branch-controls/`.
