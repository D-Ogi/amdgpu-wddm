# tools/build

Recipe scripts for the parts of the driver stack that are not built by `driver/kmd/build.ps1`: LLVM, the
Mesa components, DXVK and vkd3d-proton. [docs/build.md](../../docs/build.md) is the narrative (prerequisites, sources, deployment);
these scripts are its executable half.

| File | What it does |
|---|---|
| `build-llvm.ps1` | cmake + ninja for LLVM 23.1.2 with the recorded options; checks the new `CMakeCache.txt` against them |
| `build-mesa.ps1` | meson + ninja for one Mesa component (`-Config radv`, `radv-mt`, `llvmpipe-umd`, `zink-umd`, `zink-gl`); checks the new meson log's `Build Options:` line |
| `mesa-configs.json` | the four meson option sets and their ninja targets, the only copy the script reads |
| `build-dxvk.ps1` | meson + ninja for DXVK (`-Config per-app`, `ddi-engine`); same gate, also records submodule commits |
| `dxvk-configs.json` | the two DXVK option sets and their ninja targets |
| `build-vkd3d.ps1` | meson + ninja for vkd3d-proton (`-Config per-app`, `ddi-engine`); same gate, records submodule commits and `widl` |
| `vkd3d-configs.json` | the two vkd3d-proton option sets and their ninja targets |
| `build-umd-router.ps1` | cl for the UMD router `bc250d3d_router.dll` (`driver/umd/router`), its UMD doubles and its host-test harness |
| `test-umd-router.ps1` | the router's host gate on that output, with the hosted UMD, the CPU UMD and the DXVK shell named by parameter |
| `pe_compare.py` | compares two PE images apart from the per-build timestamps and PDB GUID of a non-`/Brepro` link |
| `build-radv-queue-tests.ps1`, `radv-queue-tests.py` | host tests of the hosted queue winsys in a built RADV tree ([driver/icd/mesa-wddm2-runtime-queues](../../driver/icd/mesa-wddm2-runtime-queues/README.md)) |
| `build-radv-unorm10-export-test.ps1`, `radv-unorm10-export-test.py` | host test of the 10-bit UNORM colour export rounding (BD-049) in a built RADV tree: the epilog key, the NIR export and the ACO epilog over about 1.3 million inputs, and a negative control without the rounding that must fail |
| `common.ps1` | shared helpers: workspace root, Visual Studio environment, tool versions, source identity |

The build scripts load the Visual Studio developer environment themselves, keep TEMP under `<BC250_ROOT>\scratch\tmp`,
restore the caller's environment on exit, and write `recipe.json` into the build directory: source commit and
working-tree fingerprint, exact arguments, tool versions, gate result and, after a build, artifact hashes.
`-ConfigureOnly` stops after the gate. None of them patches a source tree or touches the lab machine.
The two host-test runners do not build Mesa: rebuild first, then give them a new output directory, where they leave
`record.json` (source commit and status, input and test hashes, results) next to the logs. Run both after every
pull of the Mesa fork ([docs/build.md](../../docs/build.md), "Host gate after every pull of the Mesa fork"). The
last recorded run of the export test is
[evidence/windows/2026-10-03-BD-049-unorm10-export-oracle](../../evidence/windows/2026-10-03-BD-049-unorm10-export-oracle/RESULT.md).
