# atiadlxx.dll and atiadlxy.dll - the open ADL subset

An AMD Display Library (ADL) for adapters that run our driver. It answers the ADL calls of AMD AGS 6.3 with the
facts that Windows gives for these adapters, so that `agsInitialize` succeeds. The design, the AGS contract and the
packaging proposal are in [docs/design/adl-subset.md](../../../docs/design/adl-subset.md).

PROVENANCE: the structure layouts, constants and function signatures follow GPUOpen-LibrariesAndSDKs/display-library
`include/adl_sdk.h`, `adl_defines.h` and `adl_structures.h` @ 84c881cb2136f04287eb3af12de00a9e2867aa9e (MIT, notice
in each header). The code is our own. The AGS client of the tests uses the AGS SDK 6.3.0 header (MIT).

| File | What it is |
|---|---|
| `adl-abi.h` | the ADL types and constants that the library uses, with size and offset checks |
| `adl-model.h` | the snapshot: adapters, adapter entries and displays |
| `adl-discover.cpp` | the snapshot from SetupAPI, D3DKMT, `EnumDisplayDevices` and `QueryDisplayConfig` |
| `adl-main.cpp` | the exported functions, the contexts and their memory |
| `atiadl.def` | the 31 exports of the shipped library |
| `tests/adl-unit-test.cpp` | the export contract against the test build (`BC250_ADL_TEST_HOOKS`) |
| `tests/test-snapshot.h` | the adapter list that the tests inject |
| `tests/ags-client.cpp` | the AGS 6.3 client of the host tests and of the lab steps |
| `tests/atiadl-test.def`, `tests/atiadl-no-modes.def` | the exports of the test build and of the negative control |

Build: `pwsh tools\build\build-umd-adl.ps1 -Arch x64` (or `-Arch x86`). Host tests with the real AGS library:
`pwsh tools\build\test-umd-adl-ags.ps1 -Arch x64`. Both scripts run in the `umd-adl` gate of
`tools\quality\quick.ps1`.
