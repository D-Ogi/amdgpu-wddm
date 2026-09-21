# Register addressing on Cyan Skillfish

Status of this page: derived from kernel source (`HYPOTHESIS` until E01 confirms the values on our unit).

## The rule

`amdgpu` names a register by an IP block, an instance, and an `mm` offset. Both the IP segment base and the `mm` offset are **indices of 32-bit words**:

```c
/* soc15_common.h */
#define SOC15_REG_OFFSET(ip, inst, reg) (adev->reg_offset[ip##_HWIP][inst][reg##_BASE_IDX] + reg)

/* amdgpu_reg_access.c */
ret = readl(((void __iomem *)adev->rmmio) + (reg * 4));
```

So, with `rmmio` = BAR5:

```
byte_offset_in_BAR5 = (IP_BASE[inst][mmREG_BASE_IDX] + mmREG) * 4
```

For GC on this ASIC (`cyan_skillfish_ip_offset.h`): segment 0 = `0x1260`, segment 1 = `0xA000`, i.e. byte bases `0x4980` and `0x28000`. Which segment applies is given per register by `mmREG_BASE_IDX` in the offset header.

Sanity check that needs no hardware: `mmGRBM_STATUS = 0x0DA4`, segment 0, gives `0x8010`, the byte address `GRBM_STATUS` has had on Radeons since R600.

## The only way to get an offset

```
python tools/regcalc/regcalc.py lookup mmNAME
python tools/regcalc/regcalc.py reverse 0xOFFSET     # what, if anything, lives there
python tools/regcalc/regcalc.py --ip MMHUB --reg-header third_party/linux-amdgpu/mmhub_2_0_0_offset.h lookup mmMMVM_CONTEXT0_CNTL
```

`regs/` holds generated tables. In C code use the AMD headers and `SOC15_REG_OFFSET`; never a literal.

## Banked registers

Some GC registers exist once per shader engine / shader array (on this chip: 1 SE, 2 SA). Select the bank by writing `GRBM_GFX_INDEX` first, restore broadcast (`0xE0000000`) afterwards. Fields: `INSTANCE_INDEX` [7:0], `SA_INDEX` [15:8], `SE_INDEX` [23:16], `SA_BROADCAST_WRITES` bit 29, `INSTANCE_BROADCAST_WRITES` bit 30, `SE_BROADCAST_WRITES` bit 31 (`gc_10_1_0_sh_mask.h`). Compute queue registers (`CP_HQD_*`) are selected by ME/pipe/queue through `GRBM_GFX_CNTL` instead.

Under Linux, `/sys/kernel/debug/dri/N/amdgpu_regs` takes a **byte** offset (the kernel does `pos >> 2`), bit 62 of the file position enables bank selection (SE bits 33:24, SA bits 43:34, instance bits 53:44, `0x3FF` = broadcast), and the read goes through the same `RREG32` as the driver itself. There is no special privilege involved.

## The two historical mistakes (do not repeat)

| Who | Formula used | Example: `CP_RB0_BASE` (mm `0x1DE0`, seg 0) |
|---|---|---|
| correct | `(seg + mm) * 4` | `0xC100` |
| Keshas-dev | `seg + mm * 4` | `0x89E0` (they used other hand-picked values as well) |
| ZEROAESQUERDA | `mm * 4` | `0x7780` |

`tools/regcalc/test_regcalc.py` pins the correct values and asserts that the predecessor's offsets do not decode to the registers they were believed to be.
