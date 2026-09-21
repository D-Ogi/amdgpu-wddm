# driver/

Empty on purpose until M1 is closed. Planned layout (ADR 0002):

| Directory | Contents |
|---|---|
| `kmd/` | WDDM kernel-mode miniport: PnP/start/stop, display hand-over from firmware, VidMm/VidSch contracts. Owns all Windows-specific code |
| `shim/` | The small part of the Linux kernel API that the imported AMD code needs: MMIO accessors (`RREG32`/`WREG32` over BAR5), delays, locks, DMA-able memory, firmware loading, the slice of `amdgpu_device` the IP blocks touch |
| `amdgpu-import/` | AMD's IP-block code from Linux, as unmodified as possible, each file with the kernel commit it came from |
| `umd/` | User-mode driver. Direction decided in an ADR at M8 |

Rules that apply from the first line of code here: no literal register offsets, every MMIO path that has not been proven on hardware behind a gate that defaults to off, DDIs that are not implemented return an honest failure code.
