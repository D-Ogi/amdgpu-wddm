# M268: actual Mesa producer to KMD parser roundtrip

2026-09-23, host only. The generator extracts the actual BC250 allocation struct, constants and constructor from the patched Mesa checkout. RADV input flag definitions come from its radv_radeon_winsys.h; expected GEM flags use the separate contract's AMD UAPI constants.4096cases cover two heaps,256input flag combinations and eight wire alignments. Tests check full flags, cache-policy validity, CPU cached choice, allocation size/alignment and exact VA. Existing parser/v1 compatibility suite also runs.

run_umd_blob.ps1 -ProducerRoot P:/bc-250/scratch/mesa-wddm2 passes4096 producer cases with0failures and the kernel compile-check. -OmitCacheIntent only modifies generated test code to zero the emitted GEM bits; it exits1 with5120failures. This prevents the original missing producer assignment from escaping consumer-only tests. No production code/binary changed this turn.

Lab state: prior M267 full-start stream remains alive on host with no new remote output. An independent SSH15-second read timed out again. No new start/reset/installation/GPU test was issued. Owner screen observation remains pending. Neither these host tests nor the live waiting SSH process establish a completed GPU start.
