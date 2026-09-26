# amdgpu-wddm

An open Windows (WDDM) driver stack for AMD GPUs, developed first on the ASRock BC-250 (AMD Cyan Skillfish APU,
GFX 10.1.3, PCI `1002:13FE`, VRAM carve-out, no resizable BAR). The repository's working name inside the
workspace is `bc250-win`; the Mesa side lives in the companion repository
[mesa-amdgpu-wddm](https://github.com/D-Ogi/mesa-amdgpu-wddm). Not affiliated with or endorsed by AMD,
ASRock or Microsoft.

**Current work (2026-09-26): M12 breadth on the accepted M10 baseline and the M13 accelerated desktop; M9 and M11 stay open.** See the measured results and open limitations in [facts](docs/facts.md) and the [roadmap](docs/00-goal-and-roadmap.md), whose last section points to the working roadmap; the accelerated desktop has its own [M13 acceptance gates](docs/m13-accelerated-desktop-roadmap.md).

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
| `SECURITY.md`, `NOTICE`, `CONTRIBUTING.md` | How to tell genuine releases from fakes; required attribution; inbound license for contributions |
| `driver/` | The driver: `kmd` (WDDM miniport), `shim` (Linux compatibility layer), `amdgpu-import` (AMD code), `umd` |
| `third_party/` | Foreign code kept in the repo, with provenance and license |

## Quick start

```
python tools/regcalc/regcalc.py lookup mmGRBM_STATUS mmSPI_PG_ENABLE_STATIC_WGP_MASK
python tools/regcalc/regcalc.py reverse 0x5C3C
python -m unittest discover -s tools/regcalc
```

To build the KMD, LLVM and the Mesa components from source, follow [docs/build.md](docs/build.md).

## License

Copyright (c) 2026 D-Ogi. Source-available under the **PolyForm Noncommercial License 1.0.0** (`LICENSE.md`): free for noncommercial use, modification and sharing, provided the `Required Notice` lines in `NOTICE` stay attached. No commercial use, including bundling with hardware or OS images for sale. Reasons: `docs/adr/0004-polyform-noncommercial.md`.

Third-party code keeps its own license (`THIRD-PARTY.md`). AMD firmware blobs are not part of this repo.

**Beware of fakes.** This project publishes source and signed checksums only, never Windows images, installers from file hosts or BIOS files. See `SECURITY.md`.
