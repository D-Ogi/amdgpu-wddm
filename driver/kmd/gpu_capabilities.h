/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* Per-IP capabilities: the facts of one graphics part that other modules ask about (BD-332). Pure decisions, no
 * hardware access, so driver/kmd/test/amd_identity_test.c checks them on the host. The shape follows
 * queue_reset_caps.h: one row per PCI device id, a lookup by vendor and device, and no row for a part this driver
 * does not know.
 *
 * The FSR 4 answer is a statement about one part's ability to run AMD's FidelityFX INT8 provider. It is a table row
 * and not a pair of constants for two reasons: a part whose path was not measured must not get the answer, and a
 * second IP gets its own row without a second copy of the code (per-IP capability tables, the driver's direction).
 *
 * A row holds facts of the part only. Which parts have a row, and what each row says, is decided by a measurement
 * or a source on that part; the row comment below says which for the one row this driver has today. */
#ifndef BC250_GPU_CAPABILITIES_H
#define BC250_GPU_CAPABILITIES_H

/* What a part tells AMD's FidelityFX upscaler DLL 4.x about itself. NONE keeps the part's own family and revision:
 * the DLL then selects no FSR 4 provider and uses FSR 3.1.6, as it does when the KMD refuses the query. */
#define BC250_FSR4_CLAIM_NONE 0u
#define BC250_FSR4_CLAIM_INT8_NAVI2 1u

/* How packed dot products reach the silicon. HARDWARE is a part with the v_dot4 instruction class; SOFTWARE is a
 * part without it, where the lowering in dxil-spirv covers DXIL and RADV's NIR lowering covers SPIR-V. */
#define BC250_DOT_HARDWARE 0u
#define BC250_DOT_SOFTWARE 1u

typedef struct _BC250_GPU_CAPABILITY {
    unsigned short PciVendor;  /* the PCI vendor id of the part */
    unsigned short PciDevice;  /* the PCI device id of the part */
    unsigned Family;           /* the family value the DLL reads at 0x30 (FAMILY_NV: 0x8F on the BC-250 row) */
    unsigned RevisionOwn;      /* the part's own external revision the DLL reads at 0x34 (0x84 on the BC-250 row) */
    unsigned Fsr4Claim;        /* BC250_FSR4_CLAIM_*: what the part claims to the DLL */
    unsigned RevisionClaim;    /* the revision the claim reports at 0x34; unused with CLAIM_NONE (0x32 on the row) */
    unsigned DotClass;         /* BC250_DOT_* (SOFTWARE on the BC-250 row: no v_dot4, M865) */
    unsigned Native16;         /* 1 when the part runs native 16-bit shader operations */
    unsigned WaveMin;          /* the smallest wave size the part runs */
    unsigned WaveMax;          /* the largest wave size the part runs */
    unsigned Fp8;              /* 1 when the part runs the FP8 shader path of FSR 4 */
    unsigned Wmma;             /* 1 when the part runs wave matrix multiply and accumulate */
} BC250_GPU_CAPABILITY;

/* The parts whose capabilities this driver knows. Add a row only with a measurement or a source for that device.
 * The row: ASRock BC-250, PCI 1002:13FE, GFX1013. Measured facts behind it: no hardware packed 8x4 dot product and
 * v_dot2_f32_f16 writes zero (M865, M866), so dot products are lowered (M928); no WMMA and no FP8; native 16-bit
 * operations and waves 32 to 64 (GAPS.md "Met"); and the FSR 4.1.2 INT8 provider runs on unit A through the Navi 22
 * claim (scratch/fsr4-t34 kit8). */
static const BC250_GPU_CAPABILITY g_Bc250GpuCapabilities[] = {
    { 0x1002u, 0x13FEu, 0x8Fu, 0x84u, BC250_FSR4_CLAIM_INT8_NAVI2, 0x32u, BC250_DOT_SOFTWARE, 1u, 32u, 64u, 0u, 0u },
};

/* 1 and the row of one table when (vendor, device) is in it, 0 otherwise. A foreign vendor never matches. The
 * table and its count are arguments so that a test can prove the lookup is generic; the driver passes its own. */
static __inline int Bc250GpuCapabilityLookup(const BC250_GPU_CAPABILITY* rows, unsigned count, unsigned vendor,
                                             unsigned device, BC250_GPU_CAPABILITY* out)
{
    unsigned i;
    if (vendor != 0x1002u || !rows || !out) return 0;
    for (i = 0; i < count; i++)
        if (rows[i].PciVendor == (unsigned short)vendor && rows[i].PciDevice == (unsigned short)device) {
            *out = rows[i];
            return 1;
        }
    return 0;
}

/* 1 and the row when the part is in the driver's table, 0 otherwise. */
static __inline int Bc250GpuCapabilityFor(unsigned vendor, unsigned device, BC250_GPU_CAPABILITY* out)
{
    return Bc250GpuCapabilityLookup(g_Bc250GpuCapabilities,
                                    (unsigned)(sizeof(g_Bc250GpuCapabilities) / sizeof(g_Bc250GpuCapabilities[0])),
                                    vendor, device, out);
}

#endif
