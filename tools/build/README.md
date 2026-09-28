# tools/build

Recipe scripts for the parts of the driver stack that are not built by `driver/kmd/build.ps1`: LLVM, the
Mesa components and DXVK. [docs/build.md](../../docs/build.md) is the narrative (prerequisites, sources, deployment);
these scripts are its executable half.

| File | What it does |
|---|---|
| `build-llvm.ps1` | cmake + ninja for LLVM 23.1.2 with the recorded options; checks the new `CMakeCache.txt` against them |
| `build-mesa.ps1` | meson + ninja for one Mesa component (`-Config radv`, `llvmpipe-umd`, `zink-umd`, `zink-gl`); checks the new meson log's `Build Options:` line |
| `mesa-configs.json` | the four meson option sets and their ninja targets, the only copy the script reads |
| `build-dxvk.ps1` | meson + ninja for DXVK (`-Config per-app`, `ddi-engine`); same gate, also records submodule commits |
| `dxvk-configs.json` | the two DXVK option sets and their ninja targets |
| `common.ps1` | shared helpers: workspace root, Visual Studio environment, tool versions, source identity |

The build scripts load the Visual Studio developer environment themselves, keep TEMP under `<BC250_ROOT>\scratch\tmp`,
restore the caller's environment on exit, and write `recipe.json` into the build directory: source commit and
working-tree fingerprint, exact arguments, tool versions, gate result and, after a build, artifact hashes.
`-ConfigureOnly` stops after the gate. None of them patches a source tree or touches the lab machine.
