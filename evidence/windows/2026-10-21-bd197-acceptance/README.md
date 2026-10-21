# BD-197 acceptance: the fixed kernel driver and the DXGI default of the Vulkan ICD (2026-10-21)

Question: with the two parts of the BD-197 fix together, and with no variable set, does a Vulkan game present
through DXGI and flip independently, also at a runtime desktop mode? Do the D3D12 flip clients still flip in
both plane formats? Do the cases that must stay composed stay composed? Does the GDI rollback still work?

## Stack

Unit A, Windows 22631, tester.31v3 installed (D3D12 shell, desktop router and control app of that release),
1920x1200 at 59 Hz, 40 CU, the release DPM settings. The arm swapped the kernel driver file in place and
restarted, ran the trials, then put the release kernel driver back and restarted again.

| part | identity |
|---|---|
| kernel driver under test | `CC29A62692ACAD0341956546ACD52225D0B577A29E75681E25B496CDEE28FD46`, a lab build of `845c7105` (branch `fix/bd197-plane-formats-runtime`). A comparison of its source manifest with that commit: 1299 files, 0 different |
| kernel driver before and after | the release `22BA3CDB898D32D751EF27233AF01C6F81F623A6DF0424AFB7A6AD92C43D1E57` (`07-restore-sys.txt`) |
| Vulkan ICD (r4) | `vulkan_radeon.dll` `2C2BD4788778B7F5EE0733F39E9580CFDA21FA70ABAD7C6FEB90FBBD28D08921`, Mesa fork `daa8f500` (branch `fix/bd197-a-rgba-scanout`), release flavour, the same file as r4 of [the route evidence](../2026-10-21-bd197-vk-dxgi-default/README.md). It ran for one process at a time through `VK_DRIVER_FILES`. Nothing was installed |
| D3D12 flip client | `amdgpu_wddm_d3d12_queue.exe` `7DEBBFE7E6EC182BAEC21AAB58E519DE3CF01E7BDFCD9FE36AD4C69CB8EAA21A` |
| game | Quake II RTX 1.8.1, the `q2demo1` timedemo, ray query, GI and the denoiser off |
| present mode | PresentMon 2.6.0 |

No trial set a route variable, except 06.9. The route line of each game log therefore shows the default of r4.

## Files

| file | what |
|---|---|
| `steps.txt` | the arm's step log, dev PC clock (the lab clock was about 57 s ahead) |
| `05-read-start.txt` | the start record and the loaded image of the candidate kernel driver |
| `06.N-cmd.txt` | the output of trial N: route line, chain lines, PresentMon tallies, kernel driver counter delta, the D3D12 shell's scan-out line (`dbwin`), the window list |
| `06.N-crash.txt` | bug checks, kernel-power events, TDRs and dumps since the trial started |
| `07-restore-sys.txt` | the release kernel driver put back |
| `thermal.txt` | Tctl during the arm |
| `trial-lines.txt` | one line per trial, made from `06.N-cmd.txt` |
| `SHA256SUMS.txt` | the hashes of the files above, without this README |

## Results

"Last rows" are the PresentMon rows after the first 30, which cover the chain's creation. "Scan-out flips" is the
delta of the kernel driver counters `scanout_flips` and `scanout_requests` over the trial: flips that put an
application buffer on the plane. "Caps" is the D3D12 shell's line for the chain: `caps=00000003` is
`DIRECT_FLIP` with `PLANE_FORMATS`. Format 28 is R8G8B8A8, 87 is B8G8R8A8.

| trial | case | route line | frames per second | present mode, last rows | scan-out flips | caps, chain format |
|---|---|---|---|---|---|---|
| 06.1 | hashes | - | - | - | - | ICD MATCH, kernel driver file `CC29A626` |
| 06.2 | Q2RTX borderless 1920x1200 | `route=dxgi source=default` | 101.7 | independent flip 603 of 603 | 639 of 639 | 3, 28 |
| 06.3 | Q2RTX exclusive 1920x1200 | the same | 103.9 | independent flip 603 of 603 | 639 of 639 | 3, 28 |
| 06.4 | Q2RTX borderless, desktop set to 1920x1080 at run time (the BD-197 case) | the same | 113.4 | independent flip 606 of 607 | 639 of 639 | 3, 28, source 1920x1080 |
| 06.5 | Q2RTX borderless 1920x1200 after the mode revert | the same | 103.9 | independent flip 604 of 604 | 639 of 639 | 3, 28 |
| 06.6 | vkcube borderless 1920x1200 | the same | (60 Hz, 16.68 ms) | independent flip 1453 of 1453 | 1477 of 1477 | 3, 87 |
| 06.7 | D3D12 flip client R8G8B8A8, 900 frames | - | - | independent flip 862 of 862 | 900 of 900 | 3, 28 |
| 06.8 | D3D12 flip client B8G8R8A8, 900 frames | - | - | independent flip 865 of 866 | 900 of 900 | 3, 87 |
| 06.9 | Q2RTX borderless, `AMDGPU_WDDM_VK_WSI=gdi` (rollback) | `route=gdi source=env` | 56.2 | (no PresentMon rows) | 0 | - |
| 06.10 | Q2RTX framed window 1280x720 | `route=dxgi source=default` | 208.8 | composed 601 of 602 | 0 | 3, 28 |
| 06.11 | Q2RTX borderless 1920x1200, the lab overlay shown | the same | 102.4 | composed 607 of 608 | 0 | 3, 28 |

No trial had a bug check, a kernel-power event, a TDR, a dump or a restart (`06.N-crash.txt`). Tctl peaked at
68.1 C (`thermal.txt`). The first `Present1` of the game's chain came 440 to 750 ms after the chain was made
(Q2RTX, trials 06.2-06.5, 06.10, 06.11) and 45 ms after it for vkcube (the game log's `first present stage
present1` line), against the BD-105 class of a client that never presents.

06.4 is the case that BD-197 reported: the desktop changed to 1920x1080 while the driver ran. The arm proved the
mode before the game started (`mode proof, attempt 1: enum current 1920x1080 yes; kmd runtime mode 1920x1080 yes`).
The D3D12 shell's line for the chain names `source=1920x1080` with `caps=00000003`, and DWM's router front answered
`check_direct_flip ... answer=1 rule=supported` for the 1080 primary. Before the fix, the kernel driver did not set
`PLANE_FORMATS` on a runtime mode, so an R8G8B8A8 chain stayed composed.

06.11: the lab overlay window (`bc250mon`, topmost, 988,12 to 1908,940) covered part of the game. The kernel driver
put 0 game buffers on the plane, so DWM composed every frame, overlay included. The camera frames of this arm were
taken 40 to 68 s after the step began. The timedemo ended 31 s after it started, so those frames show the desktop
after the game, with the overlay on it. They are not kept.

## Extra trial: the overlay over a B8G8R8A8 chain on the release kernel driver

`x1-release-kmd-overlay-cmd.txt`, 04:44:40Z, after the arm had put the release kernel driver `22BA3CDB` back. A
review of the BD-197 runs found one run of 2026-10-21 (the release kernel driver, a B8G8R8A8 chain, the overlay
shown) that PresentMon read as "Composed: Flip" on every present, while the kernel driver counted 639 scan-out
flips of the game's buffers. If the plane showed the game's buffer, the overlay would be gone from the screen.
This trial repeats that case with the camera on the monitor: Quake II RTX, the loop demo for 45 s, borderless
1920x1200, the r4 ICD with no variable, the overlay shown.

| reading | value |
|---|---|
| route line | `route=dxgi asked=dxgi source=default reason=ok` |
| chain | 1920x1200, format 87 (B8G8R8A8), the D3D12 shell's line `admitted ... caps=00000001` |
| PresentMon | 2000 rows, "Composed: Flip" 1999, median 16.668 ms between presents, 54.6 ms until on the screen |
| scan-out flips | 2001 of 2001 |
| camera, five cropped frames 20-48 s after the start | the game on the screen, with the overlay panel over its right half |

So the overlay stays on the screen while the plane scans out the game's buffers. This agrees with DWM's reverse
composition, where DWM draws the covering window into the game's buffer and flips that buffer. PresentMon reports
it as "Composed: Flip". No reading of the buffer contents proves it. The picture on the screen is right. The cost is the frame rate: the game follows DWM's 60 Hz cadence (16.67 ms) and loses
independent flip while the overlay is up. On the fixed kernel driver the game's chain is R8G8B8A8, and DWM composed
it (06.11, 0 scan-out flips). The camera frames are not kept in this repository.

## What this does not show

- A game of the release build of these parts: the ICD ran for one process, and the kernel driver was a lab build of
  the same source.
- The rollback through the registry value `WsiRoute` and through `MESA_VK_WSI_DEBUG=sw` (both are in
  [the route evidence](../2026-10-21-bd197-vk-dxgi-default/README.md), block P2).
- Other desktop modes than 1920x1080 and 1920x1200, VSync on in the game, and RGB10A2 chains.
