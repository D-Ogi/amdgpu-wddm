# Scratch address upper-bits diagnostic

Hypothesis: ACO's unconditional FFFF high16 in hw_init_scratch is unsuitable
for Windows low GPU VAs. M510 proves identical sizing but different VA range.
LLVM23.1.2 SIFrameLowering.cpp PAL path masks descriptor high to16bits then
adds the wave offset; this is a reference, not proof of a RADV bug.

Single-variable candidate: for GFX10/10.3 only, subtract the descriptor swizzle
bit without adding FFFF0000. Keep other generations untouched. This yields
zero-extended48-bit scratch VA as in the inspected LLVM path.
First run the same bounded shader on Linux, verify emitted prolog and pixel
oracle. If it passes, keep the candidate isolated and test on Windows after
preserving Linux results and cleanly unmounting the build image/NTFS.
A Linux pass shows compatibility only. Windows pixel pass after repeated prior
failure is needed before attributing the fix. Do not waive the original test.
