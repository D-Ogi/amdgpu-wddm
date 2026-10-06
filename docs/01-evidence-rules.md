# Evidence rules

Short version: a claim about the hardware is worth exactly as much as the file in `evidence/` it points to.

## Where facts live

Facts are kept as a graph in `docs/facts/data/*.yaml`, one file per area, each fact with its ID, status, claim, detail, evidence, date and edges to other facts (`uses`, `supports`, `refutes`, `supersedes`). `docs/facts.md` and the pages under `docs/facts/` are generated from those files by `tools/facts/gen_facts.py`: edit the YAML and run `--write`; the quality gate's `--check` confirms that every cited `evidence/` path exists and that the pages are current. Format and workflow: `docs/facts/README.md`.

## Statuses

| Status | Meaning |
|---|---|
| `HYPOTHESIS` | Believed, not measured by us. May come from kernel source, prior art or reasoning. Says so |
| `MEASURED` | Observed once on our hardware; evidence file linked. Not yet cross-checked |
| `CONFIRMED` | Measured, and a positive control or an independent second method agrees (typically: Linux and Windows on the same unit, or raw MMIO and the kernel's debugfs) |
| `REFUTED` | Was believed or claimed (by us or by prior art) and a measurement contradicts it. Kept in the list so it is not re-believed |

An entry states: the claim, the status, the evidence path(s), the date, and the unit (board serial suffix or our nickname for it) when it could be unit-specific.

## Positive control first

A measurement method is trusted only after it has returned a value we already knew. Examples:

- Before trusting any raw BAR5 read under Windows: read `CC_GC_SHADER_ARRAY_CONFIG`, `GB_ADDR_CONFIG` and compare with the Linux baseline of the same unit.
- Before trusting "this write has no effect": show that a write to `SCRATCH_REG0` through the same code path does have an effect.

## Checklist before concluding "blocked / locked / not possible"

All of these must be answered in the experiment write-up, with evidence:

1. Offset: produced by `regcalc` and the register exists in the header for this IP version?
2. Positive control: does the same access method read a known value from a neighbouring register in the same IP block?
3. Block state: is the IP block out of reset, clocked, not power gated? What do its status registers say?
4. Banking: does the register need `GRBM_GFX_INDEX` (SE/SH/instance) or an ME/pipe/queue select first? Was the select itself verified?
5. Register semantics: is it read-only, write-once, or partly reserved according to `*_sh_mask.h` and the kernel's usage?
6. Reference: what does Linux read and write for this register on the same unit (debugfs or `umr`), at which point of init?
7. Ordering: does Linux touch it only after some other step (PSP/TMR setup, RLC safe mode, GFXOFF disable)?

Only when all seven are answered and the write still has no effect may a fact say the register is not writable from the host in that state. Wording stays factual: "write of X at state Y read back Z", never "impossible".

## Experiments

Each experiment lives in `experiments/Exx-name/README.md` and is written before it is run:

- Hypothesis (one sentence, falsifiable)
- Procedure (exact tool, version/commit, boot mode)
- Expected result if the hypothesis holds, and if it does not
- Result (filled in afterwards, with evidence paths)
- What changes in the facts (`docs/facts/data/`)

## Evidence files

- Raw tool output, unedited, under `evidence/linux/` or `evidence/windows/`, in a directory named `YYYY-MM-DD-Exx-short-name/`.
- Include the tool commit and, for the diagnostic USB, the `probes.json` id.
- Remove only what is private and irrelevant (MAC addresses, Wi-Fi names, serial numbers) and say that you did.
- Never edit afterwards. A correction is a new file.
