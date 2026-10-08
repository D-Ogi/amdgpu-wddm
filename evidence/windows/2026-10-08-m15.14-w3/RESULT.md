# M15.14 on a game: The Witcher 3 (D3D12) in three display modes (trial 478, 2026-10-08)

Unit A, Windows 11 Pro, 40 CU, desktop on the GPU route. One game session, 2026-10-08 from 02:51Z to 03:06Z, after
a Windows restart at 02:46Z. Every number in this file comes from the files next to it. Fact: M844.

## Question

M15.14 requires that a fullscreen or borderless game scans out its own swap-chain buffers, so that DWM does not
compose its frames. Does The Witcher 3 (next-gen, D3D12) do that on this stack? Increment 2 of M15.14 gave the
D3D12 shell a scan-out primary and gave the desktop router's front the DirectFlip answer. Before this session, the
only measurement was the plan A client (600 of 600 frames by independent flip).

## Stack

| Part | Identity |
|---|---|
| Kernel driver | 0.7.216.20, `bc250kmd.sys` SHA-256 `7580A8F7...`, EnableDisplayModes 2, EnableScanoutPlaneFormats 1, EnableDirectFlipHandshake 1 |
| Desktop route | router `93F707BB...` with DirectFlipFront 1, hosted zink `CA812652...`, ICD `CD360941...` |
| D3D12 shell | `BBB5803E...` (increment 2 of M15.14 inside, from main `3eca5f98`) |
| D3D12 engine | `348117F1...` (vkd3d-proton fork) |
| D3D12 ICD | `822134D0...` (RADV fork) |

`identity-after-478.txt` gives the full hashes. They were the same after the session.

## Method

1. Direct start of the game under the lab debugger, with `AMDGPU_WDDM_D3D12_EXPERIMENT=scanout-flip-1920x1200`.
   In the shell of increment 2 this experiment turns the scan-out primary on and names the geometry it is for.
2. Preset LOW, VSync off, LimitFPS 240, FXAA, no upscaler, no frame generation, no dynamic resolution. The game
   started in exclusive fullscreen at 1920x1200, which is the native mode. Modes (b) and (c) were set in
   Settings > Video > Display in the same session. Geralt stood in the Kaer Morhen room for each mode.
3. The lab overlay was hidden for all three modes (`mon.py action overlay.hide`). Its process and its API stayed
   up. For each mode, `mon.py windows` listed only the game and Program Manager as visible top-level windows. No
   other window covered the game.
4. Per mode: the KMD `log summary` counters before and after (`kmd-counters-478.txt`), and 20 s of ETW: DxgKrnl at
   the full keyword mask, Win32k and Dwm-Core.
5. Each capture was analysed with `tools/win/etw/etw-present-mode.py gpu.etl 6812` at commit `46c7503c`, which
   has the kernel witness of increment 3 (`present-mode-{a,b,c}.txt`). Line 1 of each output names the capture
   instead of the workstation paths. The three `.etl` files are not in the repository. `sha256.txt` gives their
   hashes.

## Result

| Mode | Result | Game presents (20 s) | Kernel per-frame events | KMD scan-out flips (20 s) |
|---|---|---|---|---|
| (a) exclusive fullscreen 1920x1200 | INDEPENDENT FLIP | 2005, 100.1/s | 2003 `IndependentFlip`, 2003 `MMIOFlip` to 3 addresses, the VSync DPC on the same 3, 0 consumed by DWM | +2028 of +2028 requested, 0 refusals |
| (b) exclusive fullscreen 1920x1080 | COMPOSED | 1853, 92.5/s | 0 `IndependentFlip`, 0 `MMIOFlip` of the game, 1200 consumed by DWM | +0, and +1211 hardware flips of DWM |
| (c) borderless window 1920x1200 | INDEPENDENT FLIP | 1682, 83.9/s | 1679 `IndependentFlip`, 1679 `MMIOFlip` to 3 addresses, the VSync DPC on the same 3, 0 consumed by DWM | +1696 of +1696 requested, 0 refusals |

The analyser's `KERNEL-WITNESS` reads `HOLDS` for (a) and (c), and `NOT-HELD` for (b). In (a) and (c), every flip
has interval 0, and every `MMIOFlip` has Flags `0x82` (`FlipImmediate`, with the unconfirmed `MoveFlip` bit). VSync
was off, so the flips tear by design.

The addresses in (a) are `0xF434664000`, `0xF43B930000` and `0xF43C1FA000` (668, 668 and 667 flips). In (c) they
are `0xF43C1FA000`, `0xF459B60000` and `0xF43B930000` (560, 560 and 559). In (b) the VSync DPC scanned the three
buffers of DWM: `0xF40B050000`, `0xF410C10000` and `0xF403AD0000`.

### Why (b) stayed composed

`decision-lines-478.txt` holds the shell's two decision lines. The first chain, at 1920x1200, was admitted. The
1920x1080 chain stood down under `mode-geometry`: the experiment named 1920x1200, and the chain was 1920x1080. The
shell then made a composed primary. No scan-out request reached the kernel driver (scan-out flips +0). The kernel
driver had committed a real 1920x1080 mode, and the fixed geometry of the experiment did not follow it.

The same line gives `source=1920x1200`. Increment 2 read the scan-out caps trailer once, when the adapter opened.
This session therefore does not tell which geometry the kernel driver's trailer gave while the 1080 mode was
committed.

### The compositor's answer

The router front of DWM (pid 1912) logged 256 `check_direct_flip` lines, which is the budget of increment 2. All
256 read `answer=1 rule=supported` for the game's buffer (E26R v3, access 5, 1920x1200, pitch 7680, format 28)
against DWM's own primary (pitch 7680, format 87). The budget ran out before the game reached the world in mode
(a). The log therefore has no answer for modes (b) and (c).

## What the analyser read wrongly, and the change

Before increment 3, `etw-present-mode.py` labelled the presents of (a) and (c) "composed flip (independent
skipped)". Win32k marked each InFrame token with `IndependentFlip` true and `SkipIndependentFlip` true (1970 of
2003 in (a), 1679 of 1679 in (c)). The kernel events of the same frames record flips: an independent flip, an
`MMIOFlip` of the game's own address, a VSync DPC on that address, and no consumption by DWM.

Increment 3 adds the `KERNEL-WITNESS` line. When all four of its clauses pass, a present with the skip pair and no
consumption reads as `hardware flip (kernel witness; Win32k skip)`. The outputs here use that rule: 1970 presents
in (a) and 1679 in (c) moved. In (a), the other 33 presents of the game are "hardware flip (independent)": for them
Win32k did not set the skip flag, and DWM reported direct flip and independent flip true.

## What this result proves

- With the scan-out primary at the native mode, the game scans out its own buffers in exclusive fullscreen and in
  borderless. DWM composes none of its frames. That is criterion M15.14 for a D3D12 game at the native mode.
- The kernel driver admitted every scan-out request of the game (2028 and 1696), with no refusal.
- Exclusive fullscreen at 1920x1080 stayed composed. The cause was the fixed geometry of the experiment. This
  session does not tell if the stack can flip at a committed mode that is not the native one.

## What this result does not prove

- VSync on: not measured in this session.
- The overlay visible over the game: not measured. M15.14 requires that DWM composes again when a window covers
  part of the game.
- D3D11 games: the D3D11 shell of this stack writes the v3 record without the SCANOUT bit, so a D3D11 game cannot
  flip on it.
- A game started from Steam without the experiment: the experiment was set by hand for this session.

## Files

| File | Content |
|---|---|
| `present-mode-a.txt`, `-b.txt`, `-c.txt` | the analyser at `46c7503c`, one per mode |
| `kmd-counters-478.txt` | the KMD scan-out and flip counters before and after each ETW window |
| `decision-lines-478.txt` | the D3D12 shell's decision lines and the front's first answer, with its count per rule |
| `identity-after-478.txt` | the installed files and their SHA-256 after the session |
| `sha256.txt` | the files above, and the three `.etl` captures that stay outside the repository |
