# Working rules for this repo (humans and AI agents)

The previous driver attempt did not fail because of the hardware. It failed because of method: hand-computed addresses, conclusions drawn without a positive control, and 160 KB of "agent memory" full of contradictory certainties. These rules exist so that we do not repeat it. Read `docs/01-evidence-rules.md` before your first change.

## Hard rules

1. **No hand-typed register addresses.** A BAR5 offset comes only from `tools/regcalc` (or, in C code, from `SOC15_REG_OFFSET` over the original AMD headers). A literal such as `0x5C3C` in code or docs without an `mm*` name next to it is a bug.
2. **No register names from memory.** If `regcalc.py lookup NAME` says NOT FOUND, the register does not exist. We do not guess.
3. **Facts live only in `docs/facts.md`**, each with a status (`HYPOTHESIS`, `MEASURED`, `CONFIRMED`, `REFUTED`) and a link to a file in `evidence/`. The journal and experiment write-ups are not sources of facts. A contradiction with an existing entry is resolved immediately (fix the entry, state why), never by appending a second version.
4. **"The write is blocked / the hardware does not allow it" requires the checklist** in `docs/01-evidence-rules.md` (address proven by reading a known value, block out of reset and clocked, GRBM bank selected, compared with Linux on the same unit). The words "impossible", "definitive", "locked" without an evidence ID are banned.
5. **Positive control first.** Before a measurement means anything, show that the same method returns a known value where we know it from Linux.
6. **`evidence/` is immutable.** Dumps are never edited. A new measurement is a new file.
7. **Import AMD code, do not retype it.** Init sequences come from `amdgpu` (MIT) through `driver/shim`. Every deviation from Linux gets a comment stating the reason.
8. **`P:\BC-250\ref\keshas-driver\AGENTS.md` and its `docs/` are not sources of facts.** Take experiment ideas and verified code (SMU mailbox, PSP ring) from there, never addresses or conclusions.
9. **Missed Linux measurements go to `docs/linux-session-wishlist.md` at once.** Unit A runs Windows now; every boot of the diagnostic stick costs the owner a trip to the machine. Whenever you notice something that would have been easy to capture under Linux, add a row (what, why, date) instead of keeping it in your head or in the journal. Before any Linux session, plan it from that file; after it, move the finished rows to "Done" with their evidence directory.

## Hardware safety

- GPU voltage and clocks: only within the limits in `docs/hardware.md`. No SMU messages outside the allow-list.
- BIOS flashing, SPI flash or CMOS writes: only with the owner's explicit consent and a programmer-made backup.
- Writing to a register of unknown meaning is forbidden. "Let me see if it accepts a write" is not a method of discovery.
- The first contact with any new unit goes through `tools/diagusb`, whose only MMIO writes are the two standard probe writes every driver performs (`GRBM_GFX_INDEX` bank select, `SCRATCH_REG0` pattern test), both restored, both skipped in its read-only mode.
- Secrets (Wi-Fi config, SSH keys) live in `P:\BC-250\secrets\`, outside the repo. Never copy them in, never print them.

## Style

- Everything in the repo is in English: docs, code, identifiers, commit messages.
- Plain hyphens `-` in prose, no em dashes.
- Small single-topic commits. An experiment result is committed together with its evidence and the `docs/facts.md` update.

## Commands

```
python tools/regcalc/regcalc.py lookup <mmNAME...>
python tools/regcalc/regcalc.py reverse <byte_offset...>
python tools/regcalc/regcalc.py grep <regex>
python -m unittest discover -s tools/regcalc
python -m unittest discover -s tools/diagusb
```
