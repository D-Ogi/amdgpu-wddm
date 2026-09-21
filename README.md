# bc250-win

Windows GPU support for the ASRock BC-250 (AMD Cyan Skillfish APU, GFX 10.1.3, PCI `1002:13FE`).

**Status: milestone M0 - nothing in this repo has been confirmed on hardware yet.** The current state always lives in `docs/facts.md` and `docs/00-goal-and-roadmap.md`.

## Starting point

Linux (`amdgpu` + Mesa RADV) fully drives this hardware. The existing Windows driver attempt (`Keshas-dev/AMD-BC-250-Windows-Driver`) stalled on the conclusion "registers are firmware-locked". Our analysis (`docs/predecessor-analysis.md`) indicates this is an artifact of a register addressing bug, not a hardware property. This repo starts over with a different method:

1. **Linux on the same physical unit is the reference.** Every Windows measurement is compared with a Linux measurement.
2. **Addresses and sequences come from AMD's MIT-licensed code, never from hand calculation.** See `tools/regcalc` and `docs/02-register-addressing.md`.
3. **Every hardware claim carries a status and evidence.** See `docs/01-evidence-rules.md`.

## Repo map

| Path | Contents |
|---|---|
| `docs/` | Goal, roadmap, rules, architecture, ADRs, predecessor analysis |
| `docs/facts.md` | The only list of established facts. Each entry has a status and an evidence link |
| `experiments/` | Experiments `Exx`: hypothesis, procedure, expected result, result |
| `evidence/` | Raw dumps from hardware (Linux and Windows). Immutable once added |
| `journal/` | Lab notebook, one file per day. Chronology, not a source of facts |
| `regs/` | Generated address tables (do not edit by hand) |
| `tools/regcalc/` | Register address calculator driven by kernel headers, with tests |
| `tools/diagusb/` | Bootable diagnostic USB: probes the board under Linux, shows results as QR codes |
| `tools/win/` | Measurement tools for Windows |
| `driver/` | The driver: `kmd` (WDDM miniport), `shim` (Linux compatibility layer), `amdgpu-import` (AMD code), `umd` |
| `third_party/` | Foreign code kept in the repo, with provenance and license |

## Quick start

```
python tools/regcalc/regcalc.py lookup mmGRBM_STATUS mmSPI_PG_ENABLE_STATIC_WGP_MASK
python tools/regcalc/regcalc.py reverse 0x5C3C
python -m unittest discover -s tools/regcalc
```

## License

Our code: MIT (matching the `amdgpu` code we import). Foreign code: see `THIRD-PARTY.md`. AMD firmware blobs are not part of this repo.
