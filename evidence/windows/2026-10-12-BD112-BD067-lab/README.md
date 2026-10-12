# BD-112 negative controls and BD-067 session 0 vulkaninfo (lab, 2026-10-12)

Date: 2026-10-12, 05:40-05:41Z. Unit A, Windows 11 Pro 22631.6199, release package 0.7.216.100-tester.28, KMD
0.7.216.36 (BD-124 part A candidate, SYS 7290210C), 40 CU. Both steps ran over SSH; the session 1 runs are one-shot
scheduled tasks of the interactive user, removed at the end. Every process was bounded (60 s for vadd, 45 s for
vulkaninfo) and exited by itself.

## BD-112: the event-order negative controls of vadd

`scripts/bd112-vadd.ps1` runs the installed `tools\hip\vadd.exe` (SHA-256 185F7E1D, beside `amdhip64.dll` 13DDF130)
three times: the positive control `--expect-compute`, then `--scale-order before` and `--scale-order omit`. The second
run (`bd112-vadd-run2.txt`) caches the process handle so that PowerShell 5.1 reports the exit codes; the first run
(`bd112-vadd-run1.txt`) has the same program output with empty exit codes.

| Case | Mismatches (pass 1, pass 2) | Program verdict | Exit |
|---|---|---|---|
| positive | 0 of 65536, 0 of 65536 | `vadd: ok` | 0 |
| `--scale-order before` | 65535 of 65536, 65535 of 65536 (index 1 is 3.0, expected 6.0) | `vadd: failed` | 1 |
| `--scale-order omit` | 65535 of 65536, 65535 of 65536 | `vadd: failed` | 1 |

The KMD health word was 15 before and after. Both negative controls fail loudly, as BD-112 requires: a scale that runs
before the addition or not at all leaves the plain sum, and `--expect-compute` reports it. With the positive halves of
2026-10-11 (halfblock 108 host and 117 device checks, vadd twice) the device side of the M16 header corrections is
covered.

## BD-067: vulkaninfo with a VkDevice in session 0

`scripts/bd067-vulkaninfo.ps1` runs the installed `tools\vulkaninfo.exe` (02A70101) with `--text` in session 1 and in
session 0; `scripts/bd067-compare.ps1` compares the two outputs.

| Session | End | Output | Device |
|---|---|---|---|
| 1 (task) | exit 0 after 0.8 s | 1839 lines, stdout and stderr together | `AMD BC-250 (RADV GFX1013)` |
| 0 (SSH) | exit 0 after 0.1 s | 1829 lines on stdout, the 10 loader and winsys lines on stderr | `AMD BC-250 (RADV GFX1013)` |

The only difference between the two outputs is where the ten stderr lines went. The session 0 run that never ended in
BD-067 does not reproduce on this package and KMD. This run does not identify the change that removed it.

## Files

| File | Content |
|---|---|
| `bd112-vadd-run1.txt`, `bd112-vadd-run2.txt` | output of `scripts/bd112-vadd.ps1`, two runs |
| `bd067-vulkaninfo.txt`, `bd067-compare.txt` | output of `scripts/bd067-vulkaninfo.ps1` and `scripts/bd067-compare.ps1` |
| `scripts/` | the three scripts as run (`bd112-vadd.ps1` in its second, handle-caching form) |
