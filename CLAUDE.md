# Working rules for this repo (humans and AI agents)

The previous driver attempt did not fail because of the hardware. It failed because of method: hand-computed addresses, conclusions drawn without a positive control, and 160 KB of "agent memory" full of contradictory certainties. These rules exist so that we do not repeat it. Read `docs/01-evidence-rules.md` before your first change.

## Hard rules

1. **No hand-typed register addresses.** A BAR5 offset comes only from `tools/regcalc` (or, in C code, from `SOC15_REG_OFFSET` over the original AMD headers). A literal such as `0x5C3C` in code or docs without an `mm*` name next to it is a bug.
2. **No register names from memory.** If `regcalc.py lookup NAME` says NOT FOUND, the register does not exist. We do not guess.
3. **Facts live only in `docs/facts/data/*.yaml`**, the facts graph (one file per area; format in `docs/facts/README.md`), each with a status (`HYPOTHESIS`, `MEASURED`, `CONFIRMED`, `REFUTED`) and a link to a file in `evidence/`. `docs/facts.md` and the pages under `docs/facts/` are generated from it by `tools/facts/gen_facts.py --write` and never edited by hand; `--check` is the gate. Experiment write-ups are not sources of facts. A contradiction with an existing entry is resolved immediately (fix the entry, state why, link the newer fact to it with a `supersedes` or `refutes` edge), never by appending a second version.
4. **"The write is blocked / the hardware does not allow it" requires the checklist** in `docs/01-evidence-rules.md` (address proven by reading a known value, block out of reset and clocked, GRBM bank selected, compared with Linux on the same unit). The words "impossible", "definitive", "locked" without an evidence ID are banned.
5. **Positive control first.** Before a measurement means anything, show that the same method returns a known value where we know it from Linux.
6. **`evidence/` is immutable.** Dumps are never edited. A new measurement is a new file.
7. **Import AMD code, do not retype it.** Init sequences come from `amdgpu` (MIT) through `driver/shim`. Every deviation from Linux gets a comment stating the reason.
8. **Other projects' notes are not sources of facts.** The agent notes and docs of earlier BC-250 driver attempts can give experiment ideas and verified code (SMU mailbox, PSP ring), never addresses or conclusions.
9. **Missed Linux measurements go to `docs/linux-session-wishlist.md` at once.** The lab unit runs Windows, and a Linux boot of it is expensive. Whenever you notice something that would have been easy to capture under Linux, add a row (what, why, date) instead of keeping it in your head. Before any Linux session, plan it from that file; after it, move the finished rows to "Done" with their evidence directory.
10. **No AI agent is named as author or co-author.** Commits and pull requests carry no `Co-Authored-By` trailer for a model, and no document, evidence write-up or experiment note says that an agent wrote or recorded it. The author of this repository's content is the licensor named in `NOTICE`; agents are tools (owner, 2026-09-26).

## Hardware safety

- GPU voltage and clocks: only within the limits in `docs/hardware.md`. No SMU messages outside the allow-list.
- BIOS flashing, SPI flash or CMOS writes: only with the owner's explicit consent and a programmer-made backup.
- Writing to a register of unknown meaning is forbidden. "Let me see if it accepts a write" is not a method of discovery.
- The first contact with any new unit goes through `tools/diagusb`, whose only MMIO writes are the two standard probe writes every driver performs (`GRBM_GFX_INDEX` bank select, `SCRATCH_REG0` pattern test), both restored, both skipped in its read-only mode.
- Secrets (Wi-Fi config, SSH keys, keys of debug channels) and lab network addresses stay outside the repo. Never copy them in, never print them.

## Long-term direction (owner, 2026-09-23)

The performance target is for Windows on the BC-250 to outperform Linux on the same hardware.
Treat optimization and sound architecture as core engineering work. Use profiling to identify
costs, then measure changes with equivalent workloads, clocks and relevant configuration;
preserve output correctness and report material differences in the comparison. A passing
functional test is not evidence that the performance target has been reached.

Design for years of development: clear interfaces, maintainable code, useful diagnostics,
and architecture that can support batching and concurrent work without weakening synchronization
or memory ownership. The longer-term ambition includes open-source alternatives to AMD
applications built around this work. Record measurements as facts (docs/facts/data/) with evidence;
keep aspirations distinct from demonstrated capabilities.

## Toolchain and Mesa freshness (owner, 2026-09-24)

Prefer bleeding-edge dependencies where practical, including LLVM and Mesa.
Use the newest upstream LLVM release and evaluate current Mesa development
upstream instead of treating an old CI version pin as the long-term target.
Record exact upstream commits, local patches, resolved build dependencies and
runtime versions. Validate rendering/content and comparable performance before
promoting a new lab baseline. Keep previous working artifacts for rollback and
comparison. A concrete compatibility failure may justify a temporary pin; record
the failing evidence and the work needed to remove it. Do not claim a newer
version is faster or correct merely because it builds.

## References

Use primary sources for contracts, and record the exact revision used:

- WDDM and kernel driver contracts: the Microsoft driver documentation
  (https://github.com/MicrosoftDocs/windows-driver-docs), the DDI reference
  (https://github.com/MicrosoftDocs/windows-driver-docs-ddi) and the headers of the WDK the
  build uses. A header of that WDK wins over a page.
- AMDGPU shader ABI, address spaces, synchronization scopes and the GFX10 memory model: LLVM
  `llvm/docs/AMDGPUUsage.rst`, read together with `AMDGPUMemoryModel.rst`. Check every example
  against gfx1013 before applying it.
- Example code of another GPU generation, a driver without preemption, or conference slides does
  not establish a contract for this hardware or this driver.

Distinguish shader/backend guarantees and AMDHSA runtime assumptions from our RADV/ACO and WDDM
implementation. Shared physical memory alone does not establish CPU/GPU cache coherence. Establish
the actual mapping attributes, command-stream cache operations and synchronization from the
Microsoft contracts, the AMD and Mesa sources and lab evidence; the LLVM guide alone does not prove
those properties.

## Style

- Everything in the repo is in English: docs, code, identifiers, commit messages.
- Plain hyphens `-` in prose, no em dashes.
- One exception to the English rule, by the owner's wish: a Polish saying or proverb that fits the situation
  is welcome now and then in READMEs and comments, in Polish with its diacritics, followed by a
  short English gloss. Dry, slightly dark humour likewise. Let future readers pick up some Polish culture
  along the way. Never in facts (`docs/facts/data/`), evidence files or safety rules, where precision comes first.
- Small single-topic commits. An experiment result is committed together with its evidence and the facts update (data and regenerated pages).

## Commands

```
python tools/regcalc/regcalc.py lookup <mmNAME...>
python tools/regcalc/regcalc.py reverse <byte_offset...>
python tools/regcalc/regcalc.py grep <regex>
python -m unittest discover -s tools/regcalc
python -m unittest discover -s tools/diagusb
python -m unittest discover -s tools/win/bc250rd   # the register generators, with the UVD/VCN deny band
                                                   # (all three are the 'register-generators' gate of quick.ps1)
python tools/facts/gen_facts.py --write      # after editing docs/facts/data/*.yaml
python tools/facts/gen_facts.py --check      # the facts gate (also in tools/quality/quick.ps1)
python tools/facts/gen_facts.py next-id
python -m unittest discover -s tools/facts
```
