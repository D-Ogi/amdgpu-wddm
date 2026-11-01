"""The register sets of the init-trace lane, by name only (offsets come from regcalc through regnames.Names).

SNAPSHOT: what a state probe reads through amdgpu's own register mapping at each trigger (rows L16, L32, L34).
Every name here was read without harm by E03's pre-driver sweep of unit A
(evidence/linux/2026-09-21-E03-init-trace/sweep-before-run1-GC-complete-then-hang.log, run2-nonGC for MP0); none is an
indexed data port, a semaphore or an MMEA register (facts M16).
TRIGGERS: the register writes of amdgpu after which a snapshot is taken (the snapshot is read at the entry of
amdgpu_device_wreg, that is, just BEFORE the write lands; the next trigger shows the state after it).
"""

# (tag used as the kprobe field name, register name for regcalc)
SNAPSHOT = [
    ("gi", "mmGRBM_GFX_INDEX"),            # positive control: must equal the last GRBM_GFX_INDEX write of the trace
    ("gcntl", "mmGRBM_GFX_CNTL"),          # SRBM-style me/pipe/queue selection of GFX10 (positive control as well)
    ("spi", "mmSPI_PG_ENABLE_STATIC_WGP_MASK"),
    ("cc", "mmCC_GC_SHADER_ARRAY_CONFIG"),
    ("ucfg", "mmGC_USER_SHADER_ARRAY_CONFIG"),
    ("aon", "mmRLC_PG_ALWAYS_ON_WGP_MASK"),
    ("pgc", "mmRLC_PG_CNTL"),
    ("rlc", "mmRLC_CNTL"),
    ("rlcst", "mmRLC_STAT"),
    ("gpm", "mmRLC_GPM_STAT"),
    ("sched", "mmRLC_CP_SCHEDULERS"),
    ("me", "mmCP_ME_CNTL"),
    ("mec", "mmCP_MEC_CNTL"),
    ("cpstat", "mmCP_STAT"),
    ("cpc", "mmCP_CPC_STATUS"),
    ("hqd", "mmCP_HQD_ACTIVE"),            # follows the current GRBM_GFX_CNTL selection, as amdgpu set it
    ("f0", "mmSDMA0_F32_CNTL"),
    ("f1", "mmSDMA1_F32_CNTL"),
    ("psp64", "mmMP0_SMN_C2PMSG_64"),      # PSP ring/bootloader status (amdgpu polls it itself)
]

TRIGGERS = [
    "mmGRBM_GFX_INDEX", "mmGRBM_GFX_CNTL",
    "mmMP0_SMN_C2PMSG_64", "mmMP0_SMN_C2PMSG_67",
    "mmMP1_SMN_C2PMSG_66",
    "mmRLC_CNTL", "mmRLC_PG_CNTL", "mmRLC_CP_SCHEDULERS",
    "mmCP_ME_CNTL", "mmCP_MEC_CNTL", "mmCP_HQD_ACTIVE",
    "mmSDMA0_F32_CNTL", "mmSDMA1_F32_CNTL",
    "mmSCRATCH_REG0",
]

# Registers whose every access the decoder lists (rows L16, L32, L34), besides the snapshot set.
WATCH = [n for _, n in SNAPSHOT] + ["mmSCRATCH_REG0", "mmRLC_SAFE_MODE", "mmCP_HQD_PQ_WPTR_LO", "mmCP_HQD_PQ_RPTR",
                                    "mmCP_MQD_BASE_ADDR", "mmCP_MQD_BASE_ADDR_HI", "mmRLC_SPM_MC_CNTL"]

# PSP and SMU mailboxes (psp_v11_0.c / smu_v11_0.c use these; names checked with regcalc in gen_probes.py)
PSP = {"status": "mmMP0_SMN_C2PMSG_64", "wptr": "mmMP0_SMN_C2PMSG_67", "ring_lo": "mmMP0_SMN_C2PMSG_69",
       "ring_hi": "mmMP0_SMN_C2PMSG_70", "ring_size": "mmMP0_SMN_C2PMSG_71", "sos": "mmMP0_SMN_C2PMSG_81",
       "bl": "mmMP0_SMN_C2PMSG_35"}
SMU = {"msg": "mmMP1_SMN_C2PMSG_66", "arg": "mmMP1_SMN_C2PMSG_82", "resp": "mmMP1_SMN_C2PMSG_90"}
