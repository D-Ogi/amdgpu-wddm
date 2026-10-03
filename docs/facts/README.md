# The facts graph

Facts live in [`data/`](data/), one YAML file per area. Everything else about facts is generated from those
files by [`tools/facts/gen_facts.py`](../../tools/facts/gen_facts.py): the index [`../facts.md`](../facts.md),
one table per area (`<area>.md`), the live list [`current.md`](current.md), the ID list [`ids.md`](ids.md)
and the mermaid graphs in [`graphs/`](graphs/). Generated pages start with a "Generated ... Do not edit"
comment. Statuses and what a fact needs: [`../01-evidence-rules.md`](../01-evidence-rules.md).

## Adding or correcting a fact

1. `python tools/facts/gen_facts.py next-id` gives the next free `M` number. IDs are never reused or renumbered.
2. Add the fact to the area file it belongs to, anywhere in the list (the generator sorts by ID). Moving a fact
   to another area means moving its entry to the other file; its ID stays.
3. A fact that corrects or overturns an older one gets an edge to it (`supersedes` or `refutes`), and the older
   entry is fixed in place: its status and text say what holds now and why. Never append a second version.
4. `python tools/facts/gen_facts.py --write` regenerates the pages and puts the data file into canonical form.
   Commit the data and the pages together, with the evidence.
5. `python tools/facts/gen_facts.py --check` is the gate (also run by `tools/quality/quick.ps1`): IDs unique and
   well formed, statuses valid, every edge resolves, every cited `evidence/` path and every relative link exists
   in the tree, data files canonical, generated pages up to date.

## Format

Each data file is a strict subset of YAML, readable by any YAML parser and by the generator without one
(standard library only). A file starts with its area:

```yaml
area: hardware                       # same as the file name
order: 1                             # position in the index
title: "Hardware, registers and firmware"
description: "..."
facts:
  - id: M1
    status: CONFIRMED
    date: "2026-09-21"
    claim: "The regcalc offsets address the registers they are named after (S1, S2 hold on hardware)"
    detail: "..."
    edges:
      - {"type": "supports", "to": "S1", "auto": true, "cue": "..."}
```

Keys of a fact, in this order (the generator writes them so):

| Key | | Meaning |
|---|---|---|
| `id` | required | `M<n>` measured or derived by us, `S<n>` read from source code, `R<n>` refuted prior art |
| `status` | required | `HYPOTHESIS`, `MEASURED`, `CONFIRMED` or `REFUTED` |
| `date` | required | `YYYY-MM-DD` |
| `date_from` | optional | set when the date was not written with the fact: `evidence-path` or `text` (first date in its evidence or text) or `git` (date of the commit that added the row); migration only |
| `claim` | required | what is claimed, one line |
| `status_text` | optional | the full status when it qualifies the status word, e.g. `MEASURED (n = 1 run)`; must start with `status` |
| `detail` | optional | how it was measured, numbers, limits |
| `evidence` | optional | evidence paths and links |
| `source` | optional | `S` facts: the source read |
| `why` | optional | `R` facts: why the claim is refuted |
| `edges` | optional | relations to other facts, one JSON object per line |

Text values are double-quoted JSON strings on one line. Links inside them are relative to `docs/`, as in the
old single-file table; the generator rewrites them for pages one directory down.

## Edges

An edge sits on the fact that does the supporting, refuting, superseding or citing, and points at the other:

| `type` | Meaning |
|---|---|
| `uses` | this fact cites or builds on the other one |
| `supports` | this fact confirms the other one |
| `refutes` | this fact contradicts the other one |
| `supersedes` | this fact replaces the other one (a correction) |

`"scope": "partial"` limits a `supersedes` or `refutes` to part of the other fact (a cause, an inference, one
clause); without it a `supersedes` removes the other fact from [`current.md`](current.md). `REFUTED` facts are
never in it. `"auto": true` marks an edge derived from wording when the old table was migrated; `cue` keeps that
wording, and `cue_in` names the fact whose text it is in when that is the other end of the edge.

## Migration (2026-10-03)

The table-based `docs/facts.md` of `43f186b3` (780 `M`, 4 `S`, 3 `R` rows) was converted by
[`tools/facts/migrate_facts_md.py`](../../tools/facts/migrate_facts_md.py);
[`tools/facts/verify_migration.py`](../../tools/facts/verify_migration.py) shows that every row's text is in
the data unchanged. Areas were assigned by keyword scoring and are meant to be corrected by hand where they
are wrong. Edges were derived only from explicit wording ("see M10", "corrected by M112", "REFUTED in part by
M106", "Supersedes M214", or a plain mention, which is `uses`). In rows from M26 on, M1-M15 name the roadmap
milestones and S0-S5 the ACPI sleep states, so those mentions are not edges.
