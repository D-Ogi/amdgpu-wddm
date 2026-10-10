# The facts graph

Facts live in [`data/`](data/), one YAML file per area. Everything else about facts is generated from those
files by [`tools/facts/gen_facts.py`](../../tools/facts/gen_facts.py): the index [`../facts.md`](../facts.md),
one table per area (`<area>.md`), the live list [`current.md`](current.md) and the mermaid graphs in
[`graphs/`](graphs/). Each generated page opens with a "GENERATED ... do not edit by hand" banner. Statuses and
what a fact needs: [`../01-evidence-rules.md`](../01-evidence-rules.md).

A fact's status lives on its area page only, and no page counts facts, so adding a fact changes its data file,
its area page and `current.md`, plus the pages of the facts it has edges to. Only the data file is edited by
hand.

## Areas

A fact goes to the area of the component whose behaviour it establishes, not of the tool or the OS that
observed it: a register value read under Linux is `hardware`, what amdgpu or Mesa do under Linux is `linux`.
`games` and `tooling` take precedence over the component areas; between the others, the borders are:

- `kmd` / `display`: the kernel driver's Present, Blt, flip, VidPN and scanout code is `display`; its memory,
  paging, submission and start/stop are `kmd`.
- `display` / `d3d`: a DWM or desktop trial (does the desktop compose, what is on the screen) is `display`;
  the D3D user-mode driver measured with test clients is `d3d`.
- `d3d` / `icd`: a RADV change measured through Vulkan (CTS, a standalone ICD control) is `icd`; measured through
  the D3D runtime or the DXVK/vkd3d-proton engines it is `d3d`.
- `icd` / `kmd`: the user-mode half of a winsys or allocation contract is `icd`, the kernel half `kmd`.

The one-line definition of each area is the `description` in its data file; [`../facts.md`](../facts.md) lists
them.

## How to add a fact

1. Get the ID: `python tools/facts/gen_facts.py next-id` prints the next free `M` number (say `M790`).
2. Append the fact to the end of its area file, e.g. `docs/facts/data/kmd.yaml`. Text values are double-quoted
   and on one line; links are relative to `docs/`:

   ```yaml
     - id: M790
       status: MEASURED
       date: "2026-10-03"
       claim: "What holds, in one sentence"
       status_text: "MEASURED (n = 1 run)"
       detail: "How it was measured, the numbers, what it does not show"
       evidence: "[E51](../evidence/windows/2026-10-03-E51-name/RESULT.md)"
       edges:
         - {"type": "uses", "to": "M780"}
   ```

   `status_text`, `detail`, `evidence` and `edges` are optional; `status_text` only when it says more than `status`.
3. Run `python tools/facts/gen_facts.py --write`. It regenerates the pages and rewrites the data file in canonical
   form (sorted by ID). Commit the data and the pages together with the evidence; `--check` (in
   `tools/quality/quick.ps1` and the `facts` CI workflow) fails if the pages were not regenerated.

## Correcting, moving, checking

- A fact that corrects or overturns an older one gets an edge to it (`supersedes` or `refutes`), and the older
  entry is fixed in place: its status and text say what holds now and why. Never append a second version.
- Moving a fact to another area means moving its entry to the other file; its ID stays. Links elsewhere in the
  repository to its old page (`facts/<area>.md#m123`) then fail the gate until they are updated.
- A share is the share of the counts next to it, and a rate normalised to another clock says which of its
  numbers the model made. `python tools/quality/claim_shares.py` is that gate (`claim-shares` in
  `tools/quality/quick.ps1`): it reads every data file, compares each percentage with its own count pair or
  difference, and asks a row that normalises a rate for the words `estimated` or `modelled`. The ratchet of
  rows that already carried an unlabelled normalisation is `tools/quality/claim_shares_baseline.txt`. With
  `--files` it reads any document the same way, line by line, so a write-up can be checked before it is
  published. It came out of the audit of 2026-10-10, which found a measured frame-time difference presented as
  time inside a shader pass ([M842](games.md#m842)) and a share taken over the wrong one of two costs.
- `python tools/facts/gen_facts.py --check` is the gate (also run by `tools/quality/quick.ps1`): IDs unique and
  well formed, statuses valid, every edge resolves, every cited `evidence/` path and every relative link exists
  in the tree, links to fact anchors name the right page, data files canonical, generated pages up to date.

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

## Unsupported prior-art claims

Claims such as "registers are SOS-locked" or "NBIO is locked at EFI boot" are neither confirmed nor refuted as
hardware behaviour: they were measured at wrong offsets and are simply unsupported. Under Linux nothing is locked
before the driver loads ([M2](hardware.md#m2), [M3](hardware.md#m3)); the same reads under Windows are experiment
E02. Prior art, claim by claim: [`../prior-art-vs-hardware.md`](../prior-art-vs-hardware.md).

This note stood under the refuted-claims table of the old `facts.md`. It is kept here, not as a fact, because
"unsupported" is none of the four statuses: an `R` fact would call the claims refuted, which the note avoids.

## Migration (2026-10-03)

The table-based `docs/facts.md` of `43f186b3` (780 `M`, 4 `S`, 3 `R` rows) was converted by
[`tools/facts/migrate_facts_md.py`](../../tools/facts/migrate_facts_md.py);
[`tools/facts/verify_migration.py`](../../tools/facts/verify_migration.py) shows that every row's text is in
the data unchanged. Areas were assigned by keyword scoring and are meant to be corrected by hand where they
are wrong. Edges were derived only from explicit wording ("see M10", "corrected by M112", "REFUTED in part by
M106", "Supersedes M214", or a plain mention, which is `uses`). In rows from M26 on, M1-M15 name the roadmap
milestones and S0-S5 the ACPI sleep states, so those mentions are not edges.
