# Working rules for this repo (humans and AI agents)

The previous driver attempt did not fail because of the hardware. It failed because of method: hand-computed addresses, conclusions drawn without a positive control, and 160 KB of "agent memory" full of contradictory certainties. These rules exist so that we do not repeat it. Read `docs/01-evidence-rules.md` before your first change.

## Hard rules

1. **No hand-typed register addresses.** A BAR5 offset comes only from `tools/regcalc` (or, in C code, from `SOC15_REG_OFFSET` over the original AMD headers). A literal such as `0x5C3C` in code or docs without an `mm*` name next to it is a bug.
2. **No register names from memory.** If `regcalc.py lookup NAME` says NOT FOUND, the register does not exist. We do not guess.
3. **Facts live only in `docs/facts/data/*.yaml`**, the facts graph (one file per area; format in `docs/facts/README.md`), each with a status (`HYPOTHESIS`, `MEASURED`, `CONFIRMED`, `REFUTED`) and a link to a file in `evidence/`. `docs/facts.md` and the pages under `docs/facts/` are generated from it by `tools/facts/gen_facts.py --write` and never edited by hand; `--check` is the gate. The journal and experiment write-ups are not sources of facts. A contradiction with an existing entry is resolved immediately (fix the entry, state why, link the newer fact to it with a `supersedes` or `refutes` edge), never by appending a second version.
4. **"The write is blocked / the hardware does not allow it" requires the checklist** in `docs/01-evidence-rules.md` (address proven by reading a known value, block out of reset and clocked, GRBM bank selected, compared with Linux on the same unit). The words "impossible", "definitive", "locked" without an evidence ID are banned.
5. **Positive control first.** Before a measurement means anything, show that the same method returns a known value where we know it from Linux.
6. **`evidence/` is immutable.** Dumps are never edited. A new measurement is a new file.
7. **Import AMD code, do not retype it.** Init sequences come from `amdgpu` (MIT) through `driver/shim`. Every deviation from Linux gets a comment stating the reason.
8. **`<BC250_ROOT>\ref\keshas-driver__WARN-AGENTS-md-is-not-facts\AGENTS.md` and its `docs/` are not sources of facts.** (`BC250_ROOT` is the workspace root, by default the parent directory of this repository.) Take experiment ideas and verified code (SMU mailbox, PSP ring) from there, never addresses or conclusions.
9. **Missed Linux measurements go to `docs/linux-session-wishlist.md` at once.** Unit A runs Windows now; every boot of the diagnostic stick costs the owner a trip to the machine. Whenever you notice something that would have been easy to capture under Linux, add a row (what, why, date) instead of keeping it in your head or in the journal. Before any Linux session, plan it from that file; after it, move the finished rows to "Done" with their evidence directory.
10. **No AI agent is named as author or co-author.** Commits and pull requests carry no `Co-Authored-By` trailer for a model, and no document, evidence write-up, journal or experiment note says that an agent wrote or recorded it. The author of this repository's content is the licensor named in `NOTICE`; agents are tools (owner, 2026-09-26).

## Hardware safety

- GPU voltage and clocks: only within the limits in `docs/hardware.md`. No SMU messages outside the allow-list.
- BIOS flashing, SPI flash or CMOS writes: only with the owner's explicit consent and a programmer-made backup.
- Writing to a register of unknown meaning is forbidden. "Let me see if it accepts a write" is not a method of discovery.
- The first contact with any new unit goes through `tools/diagusb`, whose only MMIO writes are the two standard probe writes every driver performs (`GRBM_GFX_INDEX` bank select, `SCRATCH_REG0` pattern test), both restored, both skipped in its read-only mode.
- Secrets (Wi-Fi config, SSH keys) live in `<BC250_ROOT>\secrets\`, outside the repo. Never copy them in, never print them.

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

## Local Microsoft documentation

Consult the workspace's local Microsoft sources first for WDDM and driver contracts:

- `<BC250_ROOT>\ref\windows-driver-docs\windows-driver-docs-pr\` contains conceptual
  guides, including `display/tile-resources.md`, GPU virtual memory, scheduling,
  kernel memory, debugging and Driver Verifier. Use each section's `toc.yml`.
  The recorded snapshot is staging at `110f60eaf2ac5836e644d320c1e92c1011f2af5e`;
  see `<BC250_ROOT>\ref\README.md` for scope and provenance.
- `<BC250_ROOT>\ref\ddi-display\` contains the consolidated DDI reference with
  exact declarations extracted from WDK/SDK 10.0.26100. Use the original headers
  in `<BC250_ROOT>\toolchain\nuget\` for build-version-specific checks.

Search these sources before searching the web for the same contract. Use online
primary sources when the needed topic is absent, newer behavior must be verified,
or a discrepancy remains; identify that specific gap. Record the local revision
used and retain public upstream links in shareable documentation. These driver
collections do not imply that every Win32/D3D12 or Vulkan topic is available locally.

## Local reference catalog

Use `<BC250_ROOT>\ref\README.md` to locate reference sources and their exact
revisions, scope and licenses before searching online. The catalog also covers
DirectX specifications, AMDGPU kernel documentation, IGT tests, HLK graphics
pages, ISA XML and other WDDM implementations. Respect the `__WARN-` directory
suffixes and catalog limitations: wrong-generation examples, absent preemption,
and talk slides do not establish contracts for our hardware or driver. Record
specific missing material when online access is needed.

## Local LLVM AMDGPU documentation

Consult `<BC250_ROOT>\ref\llvm-project-23.1.2\llvm\docs\AMDGPUUsage.rst`
for AMDGPU target features, address spaces, shader ABI, synchronization scopes
and the GFX10-GFX11 memory-model sequences. Read its memory-model sections
alongside `AMDGPUMemoryModel.rst`, as the guide explicitly requests. Check
architecture and runtime assumptions before applying an example to gfx1013.
Use this local reference before searching online for the same topic; retain
exact source identity when recording conclusions.

Distinguish shader/backend guarantees and AMDHSA runtime assumptions from our
RADV/ACO and WDDM implementation. Shared physical memory alone does not establish
CPU/GPU cache coherence. Establish the actual mapping attributes, command-stream
cache operations and synchronization using the relevant MS contracts, AMD/Mesa
sources and lab evidence; the LLVM guide alone does not prove those properties.

## Style

- Everything in the repo is in English: docs, code, identifiers, commit messages.
- Plain hyphens `-` in prose, no em dashes.
- One exception to the English rule, by the owner's wish: a Polish saying or proverb that fits the situation
  is welcome now and then in journals, READMEs and comments, in Polish with its diacritics, followed by a
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
python tools/facts/gen_facts.py --write      # after editing docs/facts/data/*.yaml
python tools/facts/gen_facts.py --check      # the facts gate (also in tools/quality/quick.ps1)
python tools/facts/gen_facts.py next-id
python -m unittest discover -s tools/facts
```

## Lab OS transitions (owner, 2026-09-24)

The owner authorizes autonomous Windows/Linux transitions on unit A using the
connected diagnostic USB and existing boot steering. Plan Linux work from
`docs/linux-session-wishlist.md`, preserve evidence and record boot history.
Review stale plans: completed, superseded and deferred work must be distinguished
from measurements still required. Ordinary OS transitions need no renewed approval.
