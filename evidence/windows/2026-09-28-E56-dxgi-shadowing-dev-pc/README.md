# E56: DLL shadowing and the DXGI composition present route (development PC, not unit A)

Date: 2026-09-28. Machine: the development PC, Windows 10.0.26200, NVIDIA GeForce RTX 4090. **Nothing in
this directory ran on unit A.** The lab was not touched. Both harnesses are console processes with no
window and nothing resident.

The question is from the M16 design work for the Vulkan WSI. Mesa's DXGI WSI loads `DXGI.DLL`, `DComp.DLL`
and `D3D11.DLL` by full System32 path. A game that ships DXVK or vkd3d-proton has its own `dxgi.dll` next
to the executable, and the Windows loader resolves a module's static imports by module name against the
modules that are already loaded. So a System32 module that we load by full path can still bind to the
game's `dxgi.dll`. This run measures how far that reaches and whether the composition present route
survives it.

The route under test is the one the WSI would use: a System32 DXGI factory, an explicit adapter by LUID, a
D3D11 device, an NT shared texture, a cross-device shared fence, DirectComposition with
`CreateSwapChainForComposition`, `CopyResource`, `Present1`, `ResizeBuffers` and `ALLOW_TEARING`.

## Harnesses

Two harnesses produced these logs. They live outside the repository, in the design directory of this work.
Their SHA-256 sums are in the two `manifest.json` files, together with the compiler, the System32 module
versions and the hash of every app-local DLL that the runner put next to the test executable.

| Harness | Source SHA-256 prefix | Binary SHA-256 prefix | Logs |
|---|---|---|---|
| `shadowtest` | `400B7C0C` | `7BEC4531` | `shadowtest/` |
| `presenttest` | `5525AEE8` | `DE758CC0` | `presenttest/` |

The app-local DLLs are exact copies of the builds the lab uses: DXVK 3.1.1 `dxgi.dll` (`2E674A56`) and
vkd3d-proton `d3d12.dll` (`7B77ED5C`) plus `d3d12core.dll` (`90B1DAD6`) from the Witcher 3 DX12 package of
E40, and the M14 per-app DXVK `d3d11.dll` (`41CE2364`) plus `dxgi.dll` (`F593D6C2`).

## Files

| File | What it holds |
|---|---|
| `shadowtest/manifest.json` | the run: UTC start, hashes, System32 module versions, exit code and wall time of each mode |
| `shadowtest/control.txt` | no app-local `dxgi.dll`, the baseline of the loader behaviour |
| `shadowtest/shadow.txt` | an app-local DXVK `dxgi.dll` loaded first, the game-like case |
| `shadowtest/shadow-rebind.txt` | the same, with the import tables of System32 `d3d11` and `dcomp` rewritten to System32 |
| `shadowtest/shadow-actctx.txt` | the same, with an activation context that holds a `loadFrom` to System32 |
| `shadowtest/shadow-search.txt` | the same, with `LOAD_LIBRARY_SEARCH_SYSTEM32` on the load of `d3d11` |
| `shadowtest/sysfirst.txt` | System32 `dxgi` loaded first, then the same name again |
| `shadowtest/shadow/shadowtest_dxgi.log` | the DXVK log that the `shadow` mode produced |
| `shadowtest/shadow-search/shadowtest_dxgi.log` | the DXVK log that the `shadow-search` mode produced |
| `presenttest/manifest.json` | the second run, in the same shape |
| `presenttest/control-plain.txt`, `control-seal.txt` | the full route in a clean directory, without and with the seal |
| `presenttest/dxvk11-plain.txt`, `dxvk11-seal.txt` | the full route beside app-local DXVK `d3d11` and `dxgi` |
| `presenttest/vkd3d-plain.txt`, `vkd3d-seal.txt` | the full route beside DXVK `dxgi` and vkd3d-proton `d3d12` |
| `presenttest/dxvk11-none.txt`, `vkd3d-none.txt` | the route refused by the gate, for comparison |

`plain` means no loader intervention. `seal` is a policy that rewrites the import tables of the System32
graphics modules to System32 modules that are already loaded, and that redirects bare-name loader calls
made from those modules. The seal never loads a module itself. `none` skips the route when the gate reports
shadowing.

## What the shadowtest modes show

| Mode | Device on an explicit adapter | Device on a NULL adapter | DXVK entered |
|---|---|---|---|
| `control` | ok | ok | no |
| `shadow` | ok, every object in System32 | `0x8000FFFF`, exception `0xC0000005` | yes |
| `shadow-rebind` | ok | ok | no |
| `shadow-actctx` | ok | ok | no |
| `shadow-search` | ok | fails as in `shadow` | yes |
| `sysfirst` | ok | ok | no |

Shadowing is real. In the `shadow` mode, the harness loads System32 `d3d11.dll` by full path, and its only
static `dxgi` import, `CreateDXGIFactory2`, points at the app-local DXVK DLL (`shadowtest/shadow.txt:11`).
`GetModuleHandleW("dxgi.dll")` also answers with the app-local DLL. Everything the explicit-adapter device
touches stays in System32: the factory, the adapter, the device, its `IDXGIDevice`, the NT and KMT shared
textures and the shared `ID3D11Fence` (`shadowtest/shadow.txt:12-33`).

Only the NULL-adapter device fails. It enters DXVK's factory, DXVK builds a Vulkan instance and reports the
GPU as `1002:73df`, DXVK refuses d3d11's private interface query, and the call ends in `0x8000FFFF` with an
access violation (`shadowtest/shadow.txt:36`, `shadowtest/shadow/shadowtest_dxgi.log`).

`LOAD_LIBRARY_SEARCH_SYSTEM32` on the load of `d3d11` changes nothing. The NULL-adapter device fails
exactly as in the `shadow` mode, and the mode produced a DXVK log of its own
(`shadowtest/shadow-search.txt`). `sysfirst` shows the other direction: the first load of a name wins, so
System32 first makes the later bare-name load answer System32. A game loads its own DLLs before our code
runs, so that order is not ours to pick.

## What the presenttest modes show

| Mode | Gate | Route | Contacts with the app-local DLL |
|---|---|---|---|
| `control-plain` | clean | pass | 0 |
| `control-seal` | clean | pass | 0 |
| `dxvk11-plain` | shadowed | pass | 5 |
| `dxvk11-seal` | shadowed | pass | 2 |
| `vkd3d-plain` | shadowed | pass | 3 |
| `vkd3d-seal` | shadowed | pass | 0 |

The route passes in all six modes. Each run creates the swap chain, presents four frames, resizes the
buffers and presents once more with `ALLOW_TEARING`. Frame 0 reads back BGRA 191,127,64,255 against the
expected 191,128,64,255, which is inside the tolerance of one step. Every mode exits 0 in about 0.3 s. No
mode produced a DXVK log, so DXVK was never entered. The late loads of stage S12 show that the view the
game has of its own modules does not change.

System32 `dxgi` implements `CreateSwapChainForComposition` and `Present1` in every shadowed mode
(`presenttest/dxvk11-plain.txt`, stage S07).

Of the five contacts in `dxvk11-plain`, two belong to the game's own DXVK `d3d11`, and the test expects
them. Two are the latent binding of System32 `d3d11!CreateDXGIFactory2`, which the route never calls. One
is a real call,
`GetModuleHandleA("DXGI")` from System32 `d3d11` at device creation
(`presenttest/dxvk11-plain.txt:89-93`). The seal redirects that call and removes the latent bindings. In
`vkd3d-seal` nothing touches the app-local DLL (`presenttest/vkd3d-seal.txt:87`).

`Present1(1,0)` takes 5.8 ms to 6.4 ms on this machine. That is the vsync pace of this monitor and says
nothing about unit A.

## Limits

- The user-mode driver here is the NVIDIA one. This run measures nothing about our driver, our fences or
  our allocation placement.
- No window. The harnesses do not call `CreateTargetForHwnd`, because nothing with a window may start on
  this machine.
- No game. The three scenarios are the same DLLs in a synthetic application directory.
- One Windows build. The `d3d11` call sites can move with a Windows update, so the gate has to detect that
  at run time.
