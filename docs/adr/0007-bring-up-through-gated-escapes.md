# ADR 0007: hardware bring-up (M4-M6) runs inside the display-only miniport, driven through gated escapes

Date: 2026-09-21. Status: accepted.

## Context

- Our miniport owns `1002:13FE` and runs the display without touching a register (E06, facts M28).
- `D3DKMTEscape` reaches `DxgkDdiEscape` of a display-only driver (facts M29). ADR 0006 left the control
  channel for M4-M6 open; this closes it.
- Bring-up is a long series of small hardware steps (a scratch write, GART registers, an interrupt ring, a
  PSP command, a first packet). Each one can hang the SoC. Each one has to be an experiment with evidence.
- Only one driver can own the PCI function, so the steps cannot live in a second PnP driver. `bc250rd` is a
  software-only driver that maps the same BAR for reads; it stays, as the witness that does not share code
  or state with the driver under test.

## Decision

1. **Bring-up code lives in `bc250kmd`, in files of its own (`mmio.c`, later `gart.c`, `ih.c`, `psp.c`,
   `ring.c`), and nothing in the display path calls it.** With every gate closed the binary behaves as the
   M3 driver: that is the state after every install.
2. **Gates are registry values under `Services\bc250kmd\Parameters`, read once per device start, default 0,
   reset to 0 by every install.** `EnableMmio` maps BAR5 and allows reads; `EnableMmioWrite` allows writes.
   Later steps get their own gates. Opening a gate is an act of an experiment, recorded in its evidence.
3. **Every register access goes through two generated tables** (`regs.generated.h` from `gen_regs.py`, which
   takes offsets from `tools/regcalc`): reads are limited to the list that was read on this unit without harm
   and without side effects (facts M16, M25), writes to registers named in `WRITABLE` with the experiment that
   justified each. Code that needs a register uses its generated name. No literal offsets.
4. **Escapes are the control channel and they are for administrators.** The handler checks the caller's token.
   Generic commands stay primitive (`READ_REG`, `WRITE_REG`); a multi-step hardware sequence that must not be
   interrupted half-way becomes its own command implemented in the kernel, not a script of register writes
   from user mode.
5. **Every write experiment has an independent witness and a way back**: the value is read back through
   `bc250rd` (its own mapping, its own code), a sweep against a noise floor shows that nothing else changed,
   and the old value is restored where that makes sense.
6. **From display-only to a full WDDM driver (M7) the bring-up files move over unchanged**; only the entry
   point and the VidMm/VidSch DDIs are new (ADR 0006 point 6).

## Consequences

- One binary carries both the owner's working display and experimental hardware code. The gates, the tables
  and the start budget are what keeps the first from depending on the second.
- An escape is a synchronous call in the caller's context at PASSIVE_LEVEL: fine for register work and ring
  setup, not a data path. M7 replaces it with the real submission path.
- A hang during an experiment still needs the owner's hand on the reset button. Experiments therefore keep
  announcing themselves on the overlay and keep the underclock in place.
