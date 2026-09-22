# M8 WDDM address32 pointer expansion

Hypothesis: the BC250 caps path copies Linux address32_hi (0xffff8000), but
WDDM maps RADEON_FLAG_32BIT BOs at 0x100000000. The compiled shader therefore
loads its descriptors from the wrong VA. The M137 UTCL2 payload 2/0x48 decodes
with gmc_v10_0_process_interrupt to 0x800000002000.

Procedure: retain KMD 0.7.48.1 and the existing full job frame. Change only
BC250 winsys address32_hi to RADV_WDDM2_32BIT_HEAP_START >> 32. Build ICD with
the existing MSVC/Ninja configuration. Fresh Windows boot, E19 full gate with
engines, GPU VA and submission enabled, flip disabled. One bring-up, positive
control fence gfx 1 ib, then full fill_g1. Disable shader disk cache for this
measurement. Capture compute output, IH and KMD log before restoring display-only.
If fill_g1 matches, run E14's complete suite and wrong-shader control.

Expected: full fill_g1 returns the CPU hash 0x7018cdd513a22325 with a hardware
fence and without the prior UTCL2 fault. A timeout or mismatch refutes sufficiency.
Result: hypothesis sufficient on unit A. All eight tests pass (three repetitions
per workload), byte-identical E14 SPIR-V, hashes equal to CPU and Linux. The bad
fill shader is rejected by hash comparison; inthash still passes afterwards.
Evidence: `evidence/windows/2026-09-22-E25-m8-address32/`. Facts M138 and M139.
Full-WDDM display corruption is still present; return to display-only is measured.
The batch exit-code echo is malformed, so only actual comparison output is claimed.
