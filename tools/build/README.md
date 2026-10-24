# tools/build

Recipe scripts for the parts of the driver stack that are not built by `driver/kmd/build.ps1`: LLVM, the
Mesa components, DXVK and vkd3d-proton. [docs/build.md](../../docs/build.md) is the narrative (prerequisites, sources, deployment);
these scripts are its executable half.

| File | What it does |
|---|---|
| `build-llvm.ps1` | cmake + ninja for LLVM 23.1.2 with the recorded options; checks the new `CMakeCache.txt` against them |
| `build-mesa.ps1` | meson + ninja for one Mesa component (`-Config radv`, `radv-mt`, `radv-system`, `llvmpipe-umd`, `zink-umd`, `zink-gl`, `zink-gl-icd`). `-Compiler msvc` or `clang-cl` has no default. `-Variant release,lto,isa` adds named options. It checks the new meson log's `Build Options:` line |
| `mesa-configs.json` | the meson option sets and their ninja targets, the only copy the script reads |
| `build-dxvk.ps1` | meson + ninja for DXVK (`-Config per-app`, `ddi-engine`). `-Compiler msvc` or `clang-cl` (x64 only) has no default. Same gate, also records submodule commits |
| `dxvk-configs.json` | the two DXVK option sets, their ninja targets and the arguments of each compiler |
| `build-vkd3d.ps1` | meson + ninja for vkd3d-proton (`-Config per-app`, `ddi-engine`, `ddi-engine-lto`, `-Arch x64` or `x86`). `-Compiler msvc` or `clang-cl` (x64 and x86) has no default. Same gate, records submodule commits and `widl`. It gives meson the `widl` of `toolchain\mingw-w64-tools-14.0.0.r532` (or `-Widl`) in a native file. It stops on a `widl` older than 2.14 or without explicit aggregate returns; for clang-cl x86 it also checks the calling-convention policy in every compile command |
| `vkd3d-configs.json` | the three vkd3d-proton option sets, their ninja targets, the arguments of each compiler and the `compiler_options` of `ddi-engine-lto` |
| `engine_abi.py`, `test_engine_abi.py` | selects the release or the lab engine DLL pin of `driver/umd/d3d12/engine-ddi/engine-abi.json` (`build-engine-ddi.ps1 -Flavour`), with the semantics of `tools/release/release_flavour.py`: neither flavour's DLL can pass the other's run |
| `build-umd-router.ps1` | The UMD router `bc250d3d_router.dll` (`driver/umd/router`), its UMD doubles and its host-test harness. `-Compiler clang-cl` (default, LLVM 23.1.4) or `msvc` with `-MsvcReason`, x64 and x86 |
| `test-umd-router.ps1` | the router's host gate on that output, with the hosted UMD, the CPU UMD and the DXVK shell named by parameter |
| `pe_compare.py` | compares two PE images apart from the per-build timestamps and PDB GUID of a non-`/Brepro` link, and the CheckSum that covers them |
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
