# Linux OML swap return and presentation recovery (M529)

Full016 has one terminal fail: swapbuffersmsc-return reports0 for frames0-2,
expected1-3. Identical upstream executable on RadeonSI passes. Source inspection
finds driswSwapBuffers forwarding the Kopper path to kopperSwapBuffersWithDamage,
whose normal exits return0 rather than an increasing SBC. It also discards
requested target MSC/divisor/remainder. This is a reference implementation gap,
not proof of a Windows display defect; original failure remains recorded.

Post-control after these attempts aborts134 in kopper_acquire. Exact-remainder
upload/start is gated on that control and fails before launching full017.
No claim that either individual attempt alone causes the later assertion.
Restart only Xorg,21335 ->2731, preserving AMD_DEBUG=export_modifier and same
RadeonSI binary. No OS/GPU reset; original RADVFEC7C475/Gallium1FA58014 remain.
Unchanged swapbuffers with retained drawables then pixel-passes/exit0.

All1078 outcomes retained:925pass/149skip/1warn/3fail. Exact44081 remaining
names start full017 after recovery, same configuration/oracles/45s stop gate.
Xorg restart is an explicit session boundary, not evidence of uninterrupted
stability. Final full017 result is not included here. Full M12.1-M13.1 open.
