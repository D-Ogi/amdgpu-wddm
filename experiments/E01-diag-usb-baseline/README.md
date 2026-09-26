# E01: Linux baseline of our unit with the diagnostic USB

State: run once on unit A, 2026-09-21 (tool commit `16a37cc`, probes id `79152824`). Tool: `tools/diagusb` (record its commit and the `probes.json` id with the result).

## Hypotheses

- H1: Register offsets produced by regcalc are the real BAR5 offsets on the BC-250.
- H2: Before any GPU driver is loaded (the state a Windows driver inherits), GC registers are readable, and `SCRATCH_REG0` and `GRBM_GFX_INDEX` are writable from the host through plain BAR5 MMIO.
- H3: The stock compute-unit configuration is visible per shader array: `CC_GC_SHADER_ARRAY_CONFIG = 0xFFF80000`, `SPI_PG_ENABLE_STATIC_WGP_MASK = 0x7`.
- H4: The offsets used by the previous driver attempt do not hold those values.

## Procedure

1. Build and write the stick (`tools/diagusb/README.md`). BIOS: UEFI boot, Secure Boot off, IOMMU disabled.
2. Boot the first menu entry ("full"). Phase A probes raw BAR5 with `amdgpu` blacklisted; phase B loads `amdgpu` and reads the same registers through debugfs.
3. Collect the result: QR codes (decode with `tools/diagusb/decode_qr.py`), the `bc250/results/run-NNN/` directory from the stick, or SSH.
4. If the machine hangs in phase B, reboot into "raw probe only" and note which file was the last one written in the previous run directory.

## Expected

| Observation | If the hypothesis holds | If not |
|---|---|---|
| verdict `A_BANK0.0` / `A_BANK0.1` | `CC=FFF80000 SPI_PG=00000007 STOCK` | any other value: record it, compare with phase B, do not theorize before comparing |
| verdict `A_SCRATCH_RW` | present | `A_SCRATCH_NOT_RW`: check `A_GRBM_STATUS` and `A_MMIO_LIVE` first (is GC alive at all before the driver?) |
| `B_GB_ADDR_CONFIG` | `00100044 GOLDEN` | offsets or header version wrong for this ASIC: H1 is in doubt |
| `B_IPD_GC` | version 10.1.3, bases `0x1260`, `0xA000` | the static IP offset table does not match this unit |
| predecessor claim offsets | none of them shows `FFF80000` / `00000007` | if one does, understand the aliasing before anything else |

## Result

Run 001, evidence `evidence/linux/2026-09-21-E01-diagusb-run-001/`, collected over SSH (Wi-Fi) and decoded from the QR chunks as a cross-check. Boot entry "full", no hang, `modprobe amdgpu` took 8.5 s.

| Hypothesis | Outcome |
|---|---|
| H1 | Holds. See `facts.md` M1 for the controls. Note: the control this README planned (`B_GB_ADDR_CONFIG ... GOLDEN`) failed, the register reads `0x00000044` before and after init (M5). H1 rests on four other, independent controls instead; the planned "raw equals debugfs" comparison turned out not to be a naming control at all, because debugfs takes the same byte offset |
| H2 | Holds under Linux (M2, M3): GC registers readable, `SCRATCH_REG0` and `GRBM_GFX_INDEX` writable with no driver bound |
| H3 | Half right (M4): `CC_GC_SHADER_ARRAY_CONFIG = 0xFFF80000` in both arrays, but `SPI_PG_ENABLE_STATIC_WGP_MASK` is `0x0000FFFF` as left by the BIOS and becomes `0x7` only after amdgpu init. The verdict therefore said `OTHER` for phase A, correctly |
| H4 | Holds (M9): none of the predecessor's offsets shows those values |

Surprises worth keeping: `GB_ADDR_CONFIG` bit 20 (M5); the GPU idles at 81-82 C / 60 W in a text console (M11); the unit is in the stock 24 CU state although it was sold as "40 CU" (M4), so any unlock happens in software after boot.

Changes in `facts.md`: M1-M11 added; S1, S2, S4 point to their measurements; S3 split by M4/M5; R1 is now refuted by measurement too.
