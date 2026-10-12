# BD-021: CacheCoherent requests and the SNOOPED bit per segment (lab, 2026-10-12)

Date: 2026-10-12, 05:45-05:47Z. Unit A, Windows 11 Pro 22631, release package 0.7.216.100-tester.28. Two readings of
the vidmm observation counters that the KMD has carried since candidate138 (`driver/kmd/vidmm.c`,
`VidMmCountEncoding` and `VidMmSummary`). Both are read only.

BD-021 asked whether a local VRAM PTE can get the SNOOPED bit: the encoder sets SNOOPED when the OS sets
`CacheCoherent` in the `DXGK_PTE`, and Linux sets SNOOPED only for system pages. The counters split every validly
encoded entry by page-table level and by kind (system segment, local segment, page-table segment), count the
`CacheCoherent` requests and count each entry whose SNOOPED bit differs from its request.

## Readings

| Reading | Driver run | Source |
|---|---|---|
| 1 | 236 s, KMD of 2026-10-11 16:30Z | the kept stop ring `ring-20261011-163443-057-stop.log` (`scripts/bd021-snoop.ps1`) |
| 2 | 1134 s of desktop use, KMD 0.7.216.36 | the live summary, `bc250kmd_cli log summary` (BC250_ESCAPE_LOG_SUMMARY, `scripts/bd021-summary-now.ps1`) |

Level 0 (PTEs), counts of the last summary in each reading:

| Segment | Kind | Reading 1 coherent / noncoherent | Reading 2 coherent / noncoherent | Snoop mismatches |
|---|---|---|---|---|
| 0 | system | 37518 / 819748 | 53618 / 1268854 | 0, 0 |
| 1 | local VRAM | 0 / 759790 | 0 / 874172 | 0, 0 |
| 3 | page tables | 0 / 512 | 0 / 512 | 0, 0 |

Levels 1 to 3 name only segment 3, with 0 coherent requests and 0 mismatches in both readings.

## Result

The OS did not request `CacheCoherent` for a local PTE in either reading, so no local VRAM PTE got SNOOPED. For system
pages the bit follows the request exactly (0 mismatches). The case of BD-021 does not occur on these workloads. Neither
reading covers a game session. The counters are in every vidmm summary, so a later run that shows a nonzero local
coherent count reopens the item.

The other 13 stop rings of 2026-10-11 and 2026-10-12 have no vidmm summary lines. For that reason reading 2 took the
summary from the running driver. The first lines of `bd021-summary-20261012.txt` are the error of an unknown CLI command
(`version`) in the script and have no other effect.

## Files

| File | Content |
|---|---|
| `bd021-snoop-20261012.txt` | output of `scripts/bd021-snoop.ps1`: the vidmm summary lines of each kept stop ring of the last two days |
| `bd021-summary-20261012.txt` | output of `scripts/bd021-summary-now.ps1`: the vidmm lines of the live summary |
| `scripts/` | the two scripts as run |
