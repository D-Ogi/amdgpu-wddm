# Documentation style

People must act on these documents, and agents must read them without a human to ask what a
sentence meant. This page states how we write them. `docs/glossary.yaml` holds the terms, and
`tools/quality/doclint.py` is the gate.

## The base

The base is ASD-STE100, Simplified Technical English, Issue 9, from the ASD-STE100 Maintenance
Group (<https://asd-ste100.org>). Part 1 of that standard is nine sections of writing rules.
Part 2 is a controlled dictionary of about one thousand approved words.

The specification text and the dictionary are ASD copyright. This repository reproduces neither
of them. What follows is our own statement of the rules we keep, with our own examples.

We take Part 1, and we replace Part 2. Part 2 alone cannot carry a GPU driver, because almost
every noun we need is a technical noun outside it. The standard already lets a writer keep the
technical nouns and verbs of the subject field. For us those are register names, DDI names, WDDM
terms and the verbs of our own stack. `docs/glossary.yaml` binds them, one meaning each.

## Rules by document type

| Document type | Examples | Rules |
|---|---|---|
| Procedure | kit and tool READMEs, `docs/build.md`, lab plans | At most 20 words a sentence. One instruction a sentence. Imperative mood. One action a step. Steps as a vertical list. A warning stands before the step it belongs to |
| Descriptive engineering | ADRs, `docs/design/`, the status table of the top README | At most 25 words a sentence. At most six sentences a paragraph. Active voice. No noun cluster longer than three words, or explain it once |
| Record of a measurement | release notes, experiment write-ups, `RESULT.md` | One claim a sentence. Every number with its unit. The status words of our own legend, never a new one |
| Safety | thermal limits, AC power rules, firmware bans | The action first, then the risk it holds off. A limit as a number. No hedge |
| Code comment, commit message | | One topic. Short sentences. No gate reads them |
| Owner conversation, `agent-discussion` | | Out of scope, and outside this repository |

Across every type: no semicolon, no em dash, no Latin abbreviation such as `e.g.` or `i.e.`, and
no marketing adjective. A list item never ends in `and` or `or`. Write the article and the word
`that` out in full.

## One term, one meaning

Each thing in this stack has one name. Pick the name once, then use it every time.
`docs/glossary.yaml` is that list, with one sentence a term. Add a term as soon as a review finds
a second name for one thing.

A spelling under `instead_of` in the glossary is a finding. Rotation between plain synonyms is a
finding too: a document that says `check` in one place and `verify` in another names one action
twice, and a reader cannot tell whether the second one is a third step.

## Structure, which the standard does not cover

- One document type a file. A procedure and an explanation do not share a page.
- One source of truth a fact. The facts graph under `docs/facts/data/` is that source.
- A generated page carries its banner, and no hand edits it.
- No number and no status is copied between files. Link to the one place that owns it.

## The gate

`tools/quality/doclint.py` runs in `tools/quality/quick.ps1` and in CI. It reads `README.md`,
everything under `docs/`, and every Markdown file under `driver/` and `tools/`.

The gate is a ratchet, and not an order to rewrite anything.
`tools/quality/doclint_baseline.txt` records the findings each document had on 2026-10-07. A
document in that file must not get worse. A document outside it must have no finding at all.

- `python tools/quality/doclint.py` is the gate.
- `python tools/quality/doclint.py --files docs/build.md` reads the named documents.
- `python tools/quality/doclint.py --prune` lowers a baseline line after a document improves.

No document has to be rewritten for this gate. A document that is edited for another reason
should come out lower than its line, and `--prune` then writes the lower number.

The rule engine is `tools/quality/third_party/ste_lint.py`, a vendored MIT linter with its
provenance beside it. Its advisory findings, passive voice and compound tense, are printed and
never gated. Hedges are never flagged, because our confidence in a claim is part of the claim.

## Out of scope

- `evidence/` is immutable, so no gate may ask for an edit there.
- `experiments/` is the record of one run each, and the gate leaves it alone for the same reason.
- Generated pages, which the gate skips by their banner or their `.generated.md` name.
- A Polish saying with its English gloss, which the repository style welcomes in a README or a
  comment. Keep it out of facts, evidence and safety rules.
