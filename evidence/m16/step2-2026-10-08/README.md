# M16 step 2 on unit A, 2026-10-08: a HIP program on our own runtime

Session 2026-10-08T12:35:04Z to 12:35:07Z, release 0.7.216.100-tester.21 (KMD 0.7.216.22), 40 CU, GPU at its
500 MHz idle state before and after, Tctl 60.4 C before and after (`clock read`, temperature_mc 60375).

| Artifact | Bytes | SHA-256 |
|---|---|---|
| `amdhip64.dll` (product build of `m16/hip` 813c59d3, `/MT`, links `bc250hsa.lib`) | 242688 | `D67F5D788AD799107F18963118D2972F5166D0A1E7AA7CC778DFAB0350D0B9F2` |
| `vadd.exe` (clang 22.1.8, gfx1013, the HIP sample of `compute/hip`) | 163840 | `B03B564DBDD524F20EB8E9421D6A8A1EE667049EC59C089FD7C953A35E0C6EB1` |

Command: `vadd.exe --expect-compute --wait-total 10000` in `C:\BC250\tmp\m16-hip`, run by `step2.ps1` (copy in this
directory). Output: `vadd.out.txt`, `vadd.err.txt` (empty); driver log ring before and after.

| Criterion of `step2-README.md` | Result |
|---|---|
| 1 `vadd: ok`, `api failures 0` | yes; the process exit code was not captured by the script (empty value of `Start-Process -PassThru`) |
| 2 both passes `mismatches 0 of 65536` | yes |
| 3 `scale` after `vadd` across an event wait | yes (criterion 2 covers it) |
| 4 pass 2 memory free equals pass 1 | yes, 8589926400 both |
| 5 time to the first launch | 4.0 ms after start |
| 6 properties | `arch 'gfx1013' warp 32 shared 65536 cu 40` |
| 7 driver log: no "not run", fault or timeout line | yes; 76 new lines, the one match of the script's filter is the ring header (the word "lost" in "lost to the wrap") |
| 8 no TDR, no bugcheck, no lost device | yes |

Answers of design section 7: the error stream is empty, so `D3DKMTLock2` on a device-local allocation works on this
part (no host-visible fallback), and the GPU address window does not overlap the host address space of the process.
Open question 8 (a second dispatch in flight): two kernels per pass, four in the run, no `hipErrorNotReady`.
