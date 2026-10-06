# BD-037 desktop UMD host tests: the control runs and the router gate

Date: 2026-10-02 and 2026-10-03. Machine: the development PC. No lab unit and no GPU take part. The two host
tests drive the shared-surface setup path over fake kernel calls, and the router gate runs the application
router against two built UMD DLLs.

BD-037 is a leak: `Bc250EnsureSurface` repeated a surface setup that had already failed part way, so the
earlier steps ran again and their allocations and maps were lost. The fix resumes the setup at the step that
failed. A test that only shows PASS proves nothing here, because a test that never reaches the fault also
passes. Each lineage therefore has a control run, with the fix absent and the fault injection present, and the
control must fail.

## What ran

| Lineage | Host test | Build and run |
|---|---|---|
| CPU UMD (`bc250d3d.dll`, llvmpipe) | `bc250_surface_format_test.exe` | `scratch/bd037/build-umd.ps1 -Which cpu` |
| hosted UMD (`bc250d3d_zink.dll`, zink) | `bc250_ensure_surface_test.exe` | `scratch/bd037/build-umd.ps1 -Which zink` |

Both recipes are `tools/build/build-mesa.ps1` (`llvmpipe-umd` and `zink-umd`), in the form the deployed builds
use. The host test is not a recipe target, so the wrapper builds it with ninja in the recipe's own environment.

`router-gate.ps1` runs every scenario of the application router's host gate against the two built DLLs. The
script is in this directory because it names the two UMD paths. It changes only the output root of the gate it
calls, and it refuses to run if that gate's own source moved since.

## Result

| Run | Fix | Failures | Log |
|---|---|---|---|
| CPU control | absent | 9 | [cpu-control.txt](cpu-control.txt) |
| CPU with the fix | present | 0 | [cpu-fix.txt](cpu-fix.txt) |
| hosted control | absent | 19 | [hosted-control.txt](hosted-control.txt) |
| hosted with the fix | present | 0 | [hosted-fix.txt](hosted-fix.txt) |

The control failures are the leak itself. Each one names the step that faulted and then shows two allocations,
two maps and two different GPU addresses where there must be one of each, or a second `MakeResident`, `Lock2`
or import of a step that had already succeeded.

The router gate passed: 68 scenarios, 0 failed ([router-gate-summary.txt](router-gate-summary.txt), inputs in
[router-gate-inputs.sha256](router-gate-inputs.sha256)).

## Where the code is

The fixes are on the Mesa fork, not here:

- CPU lineage: `amdgpu-wddm/desktop-umd-bd037` (`45bf1502`, `a95aeb2f`, `98ccc560`, `58049622`), carried into
  release train b19 as `amdgpu-wddm/b19-desktop-umd`.
- hosted lineage: `amdgpu-wddm/hosted-umd-bd037` (`e7d47545`, `5785c3c9`, `d3977297`), carried into b19 as
  `amdgpu-wddm/b19-hosted-umd`.

The build directories and the two source worktrees of the 2026-10-02 runs took 2.7 GB. This run deletes them.
These logs are the record of those runs. The b19 wagons rebuild both DLLs and run both tests again.

Serves BD-037, M13.1 and M13.4.
