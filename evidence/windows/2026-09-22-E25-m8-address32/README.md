# E25: M8 completed on unit A

Windows boot 2026-09-22T22:14:36, KMD 0.7.48.1, Mesa fork 801c9763c6 with
`driver/icd/mesa-wddm2-bc250.patch`. The ICD SHA-256 is
71cb633dd40bb3ff54e4989b077726c27c8948fb1f7286829ff9b6aea0934aa8.

The BC250 winsys now derives address32_hi from its WDDM 32-bit BO heap,
0x100000000, instead of copying the Linux caps value 0xffff8000. The old
UTCL2 payload (2, 0x48) decodes to 0x800000002000 using AMD's
`gmc_v10_0_process_interrupt`, consistent with a truncated descriptor pointer
expanded using the wrong upper word. Fixing COMPUTE_PGM_HI alone (M136)
could not fix pointer constants compiled into the shader.

`address32-results/vulkaninfo.txt` lists AMD BC-250 (RADV GFX1013).
`comparison.json` verifies all eight GPU hashes against both CPU and Linux E14,
and all nine SPIR-V files byte-for-byte against the Linux evidence. The suite
used --runs 3. `negative.txt` catches the deliberately incremented fill value;
`positive.txt` subsequently passes inthash from the same shader directory.
The batch echo lost exit-code digits to cmd redirection parsing; do not infer
process exit codes from these files. Hash comparison and mismatches are intact.

`fill-first-kmd.txt` and `kmd-log.txt` record UMD submissions and hardware fence
completion on the WDDM path. The separate VMID 0 escape fence was only the
positive control, not the Vulkan workload. IH counted 82 vectors, with only
clients 8/20 and no UTCL2 or SQ fault. No TDR or bugcheck occurred. Temperature
was 71-72 C during the test; the driver returned to stage 61 display-only and
UnconfirmedStarts 0. Full-WDDM display corruption remains unresolved; its DWM
errors are not a successful display test. M8 is compute, not M10/M13.

Privacy: UUID lines were omitted from vulkaninfo.txt before publication. The
original tar and raw vulkaninfo remain in scratch/m8 outside the repository.
All other captured result files are raw. No OS reinstall or USB change was needed.
