# M401 - Observe the missing AMD reset-classification input

M400 warm failure persists despite corrected observer and final GART policy.
Existing RLC checkpoints record RLC_CNTL and GRBM_STATUS2 but not GRBM_STATUS.
AMD gfx_v10_0_soft_reset uses GRBM_STATUS for CP/GFX busy classification as
well as STATUS2 for RLC. An RLC-only sample cannot justify broader reset masks.

Add one allowed GRBM_STATUS read to existing GfxTraceRlcState checkpoints,
logging the value and read status separately to avoid truncating the RLC line.
No observer MMIO inside invalidation is reintroduced; no reset/write/policy
change. Compare successful first-load and stop checkpoints, then one changed
warm trial only after build validation. Keep exact source and register proof.
No newly successful reset or missing PSP command is assumed.
