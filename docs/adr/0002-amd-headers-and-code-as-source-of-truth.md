# ADR 0002: AMD's MIT-licensed headers and code are the source of truth

Date: 2026-09-21. Status: accepted.

## Context

Three independent attempts derived register offsets by hand and all three got them wrong, each differently (`prior-art.md`). The information they needed is published by AMD under the MIT license inside the Linux kernel.

## Decision

1. Register offsets come only from AMD's `*_offset.h` / `*_ip_offset.h` headers: through `tools/regcalc` in scripts and docs, through `SOC15_REG_OFFSET` in C.
2. Hardware bring-up sequences are imported from `amdgpu` (`gmc_v10_0.c`, `gfx_v10_0.c`, `psp_v11_0_8.c`, `smu_v11_0`, `navi10_ih.c`, `sdma_v5_0.c`, `nv.c`) and compiled against a compatibility shim in `driver/shim`, in the manner of the BSD DRM ports. We do not re-implement them from reading.
3. The kernel commit each imported file came from is recorded next to it.

## Consequences

- The shim (MMIO accessors, delays, locks, DMA memory, firmware loading, the parts of `amdgpu_device` the IP blocks rely on) is real work and is the core of M4-M6.
- GPL-only parts of the kernel cannot be imported. The AMD driver files carry MIT notices; each import is checked individually and listed in `THIRD-PARTY.md`.
- Deviations from Linux behaviour need a comment with the reason.
