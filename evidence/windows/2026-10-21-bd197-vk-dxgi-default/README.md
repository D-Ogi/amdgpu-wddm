# BD-197: the Vulkan DXGI present route on unit A, and DXGI as its default (2026-10-21)

Question: does the DXGI present route of the system Vulkan ICD present on unit A, does a borderless or
exclusive game then flip independently, and does the route hold as the default with no variable set? A side
question (block E): what does the CPU copy of the GDI route cost, before and after the SSSE3 copy.

Unit A, Windows 22631, tester.31v3 installed, release KMD `22BA3CDB898D32D751EF27233AF01C6F81F623A6DF0424AFB7A6AD92C43D1E57`
(the arms refuse to start on another one), 1920x1200 at 59 Hz. Each candidate ICD ran for one process only
(`VK_DRIVER_FILES` to a manifest next to it). Nothing was installed. Quake II RTX 1.8.1 played the `q2demo1`
timedemo with ray query on, GI and the denoiser off, 45 s at most per run. PresentMon 2.6.0 read the present
mode. The KMD log summary before and after each run gave the scan-out flip counters. The ICD's game log (the
control app's game-log switch) gave the route lines.

## Candidates

All builds are of the Mesa fork, branch `fix/bd197-a-rgba-scanout`, on the tester.31v3 system ICD source
`042227dc`, MSVC 19.44.

| name | source | flavour | `vulkan_radeon.dll` sha256 |
|---|---|---|---|
| inst | the installed tester.31v3 system ICD | release | 700A03C5735866CA80AB5EA007D175E713FDF57F42090C009A4ED9D21482A7E0 |
| r2 | `d6d5a896` | release | 5961EE1E6CD70D28E02C5495A550F0C24D9F3468176D35D7BA5C53F851DCAD9E |
| r2lab | `d6d5a896` | lab (present log) | 3BCBBF2062ED5D438E33C11E7B316323643B96A68AF6242E8C8755E6E345AF34 |
| r3lab | `9c3b6660`, the SSSE3 copy | lab (present log) | 9245917A3D7B409A7DF52332F68A9944AF0830975F719009820349E963E386E4 |
| r4 | `daa8f500`, DXGI as the default | release | 2C2BD4788778B7F5EE0733F39E9580CFDA21FA70ABAD7C6FEB90FBBD28D08921 |

vkpresent (`tools/win/vkpresent`): block E ran `2A74E2D7...` (the witness that `vkpresent` reported wrongly, see
below), P2 ran `CA8D96A3B06A6C3DF5390A506FB6749DA13CF1FA2365D0E5A2825EF991952E83`. vkcube
`87A96AF3A4677C4AE0BC293625B9F18BFE80A41362992BC03CA2AA3CADE37721`.

## Files

| file | what |
|---|---|
| `d1-*` | block D1, 01:58-02:07Z: the installed ICD and r2, `dxgi` and `gdi` by the variable, borderless and exclusive, three rounds |
| `d2-*` | block D2, 02:20-02:27Z: r2 with z-order readings (`wz`), the overlay shown (`ov`), a window, vkcube, the default and the registry |
| `e-*` | block E, 03:19-03:27Z: r2lab against r3lab on the GDI route, interleaved. `e-plog-split.md` pools the present logs |
| `e-07-red-screen-half.jpg` | the half-size screen picture of run 07 (r3lab, RGBA8, every frame cleared to FF0000) |
| `p2-*` | block P2, 03:46-03:53Z: r4 with no route variable, and the rollbacks |
| `*-arm.txt` | the arm's own log, one RESULT line per run |
| `*-results.csv` | one row per run |
| `*-route-lines.txt` | the route line and the first-present stages of each run's game log |
| `SHA256SUMS.txt` | the hashes of the files above, without this README |

A run name is `<icd>-<route>-<window>[-flags]`. The route `def` sets no variable. `rg` sets `WsiRoute=gdi` in
`HKLM\SOFTWARE\amdgpu-wddm\Vulkan` for that run only. `sw` sets `MESA_VK_WSI_DEBUG=sw`. The window is `bl`
(borderless desktop fullscreen), `ex` (mode-set fullscreen), `win` (1280x720 at 0,0), `cube` (vkcube 1920x1200
borderless), or `vkp` and `vkpb` (vkpresent 1920x1200 borderless, RGBA8 and BGRA8, IMMEDIATE, 2400 frames).

## Results

P2, r4, nothing set unless the run name says so (`p2-arm.txt`, `p2-route-lines.txt`):

| run | route line | frames per second | present mode (PresentMon) | first Present1 after the chain was made |
|---|---|---|---|---|
| 01 def bl | `route=dxgi asked=dxgi source=default reason=ok` | 101.8 | independent flip 631 of 633 | 660 ms |
| 02 def ex | the same | 103.7 | independent flip 631 of 633 | 543 ms |
| 11 def bl | the same | 104.5 | independent flip 628 of 631 | 493 ms |
| 12 def ex | the same | 103.6 | independent flip 631 of 633 | 497 ms |
| 03 def cube | the same | - | independent flip 1470 of 1480 | 38 ms |
| 04 def win | the same | 209.4 | composed, 631 of 631 | 409 ms |
| 05 def vkpb | the same, and vkpresent `dxgi/dxgi source=default`, 2400 of 2400 read back as asked | 2114.5 | independent flip 2377 of 2394 | 51 ms |
| 06 def bl rg | `route=gdi asked=gdi source=registry reason=asked` | 53.6 | - (GDI) | - |
| 07 gdi bl | `route=gdi asked=gdi source=env reason=asked` | 51.8 | - | - |
| 08 gdi vkpb | the same, and vkpresent `gdi/gdi source=env`, 2400 of 2400 | 329.7 | - | - |
| 09 def bl sw | `route=gdi asked=dxgi source=default reason=dxgi-runtime` | 52.8 | - | - |
| 10 def vkpb sw | the same, and vkpresent `gdi/gdi`, expected gdi, 2400 of 2400 | 329.6 | - | - |

In every DXGI run with the game at the output's size the KMD counted scan-out flips for every flip of the chain
(639 of 639 for Quake II RTX). The route proved itself ("route presented ... deadlines off") in every DXGI
run, and no run wrote `route-deadline`. In Quake II RTX the acquire that ends that proof waited until
3199-3620 ms into the chain, because the game loads the demo between its first present and that acquire. The
arm had 0 display, dxgkrnl, driver or WHEA events and 0 live kernel reports. The WsiRoute value was absent
before the arm and absent after it.

D2, r2 (`d2-arm.txt`): the same picture with the route asked by the variable or by the registry: 103.0-104.1
frames per second with independent flip on 629-631 presents, vkcube independent flip on 1472 of 1482, the
window composed at 208.6, the GDI runs at 52.1-55.3. With the overlay shown over the game, the chain was
composed at 59.3 frames per second, as a window over the game requires.

D1 (`d1-arm.txt`): in round 1 both r2 DXGI runs flipped independently (101.8 and 103.9 frames per second). In
rounds 2 and 3 the four r2 DXGI runs were composed at 59.3-59.4 frames per second, while the KMD still
counted scan-out flips for them. The installed ICD composed at 98.1-102.9 in all three rounds. Why D1's later
rounds composed is not established. No run of D2 or P2 at the output's size composed, except the one with the
overlay shown.

E, the GDI copy (`e-plog-split.md`, medians in microseconds, period between two presents):

| client | before (r2lab) copy / period | after (r3lab) copy / period |
|---|---|---|
| vkpresent RGBA8 (the copy swaps red and blue) | 2898 / 4737, 3 runs | 828 / 2634, 4 runs |
| vkpresent BGRA8 (memcpy) | 1087 / 2889, 1 run | 1113 / 2962, 1 run |
| Quake II RTX borderless, B8G8R8A8_SRGB (memcpy) | 1333 / 19335, 3 runs | 1324 / 18216, 3 runs |

Every vkpresent frame of block E read back as asked (0 of 2400 mismatched in each run). The two pictures of run
07 have a mean colour of 254,1,4: red stays red.

**vkpresent reported FAIL in every run of block E, and the fault was in vkpresent.** Its witness took
`dxgi.dll` and `DComp.dll` in the process for a DXGI route. The ICD's instance set-up loads both on every route,
so the runs read "asked gdi, took dxgi-composition" while their present logs said path `gdi`. The `vkpresent`
of P2 reads `d3d12.dll` instead, which only a DXGI route loads, and P2 reads `gdi/gdi` and `dxgi/dxgi` where the
route lines say the same.
