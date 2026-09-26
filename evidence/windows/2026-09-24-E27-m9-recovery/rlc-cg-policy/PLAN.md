# M390 - Honor RLC clock-gating policy

Apply the exact AMD condition for GFX_MGCG/GFX_CGCG/GFX_3D_CGCG before requesting safe mode. Preserve stricter ACK handling when requested and paired exit. Import masks mechanically from v6.18.52 amd_shared.h. Extend existing reset model with zero flags, unrelated MGLS and each relevant flag, including disturbed post-reset state. Run full GFX replay and kernel compile. This is a local fix; no hardware/warm success claim before a new candidate test.
