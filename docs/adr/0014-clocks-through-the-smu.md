# ADR 0014: clocks through the SMU, measured on Linux first

Date: 2026-09-22. Status: **accepted** (split out of ADR 0010 point 6 at the owner's suggestion). State on
2026-09-27: point 1 is done (the kernel driver owns the operating point, the legacy startup task is disabled;
deployment record in the workspace STATE file); points 2 and 3 are open; point 4 (the 85 C stop enforced in
the driver) is not implemented: the driver reads the temperature (`driver/kmd/smu.c`) but does not act on it.

## Context

The GPU runs at one fixed, low operating point, applied after boot by a startup task outside the driver. It is a
safe point for bring-up and a poor one for games: no line of driver code will move the frame rate as much as the
clock does. The limits are in `docs/hardware.md`, and so is the allow-list: only the SMU messages Linux's
`cyan_skillfish_ppt.c` sends, with the same argument ranges.

## Decision

1. Power management belongs to the driver. Today the SMU mailbox is driven by the `tools/win/bc250rd` instrument,
   not by `bc250kmd`; the mailbox sequence moves into the shim (imported from `cyan_skillfish_ppt.c`, hard rule 7),
   `bc250kmd` sets the operating point itself, and the startup task goes away when that works.
2. First a fixed, higher point inside `docs/hardware.md`'s limits, chosen from what Linux runs this unit at under
   load; dynamic scaling (idle down, load up) only after that, and only with messages on the allow-list.
3. Every step is preceded by a Linux measurement on the same unit: available operating points, clocks and voltage
   under load and idle, temperatures, the messages amdgpu sends (wishlist). Nothing is extrapolated from other boards.
4. The 85 C stop rule is enforced in the driver once it owns the clocks, not only in the lab procedure.

## Consequences

- Blocks nothing in ADRs 0011 to 0013 and is scheduled beside them; its Linux measurements ride in the same session.
- Until point 2 is done, every frame-rate number carries the operating point it was measured at, or it is not
  recorded.
