# ledger - the work ledger of this workspace

Work in this project does not fail at the keyboard. It fails after it is written, when a branch, a scratch
directory or a finished tool never reaches main, a release or the documentation. This tool is the gate against
that. It keeps one file, `LEDGER.json`, and answers one question from it: which piece of work is finished or
half-finished and is still not landed.

```
python tools/win/ledger/ledger.py brief                     one screen, for the start of a session
python tools/win/ledger/ledger.py check                     the gate; exit 1 on debt or drift
python tools/win/ledger/ledger.py check --release 0.7.205.100-tester.11
python tools/win/ledger/ledger.py render                    writes LEDGER.md next to LEDGER.json
python tools/win/ledger/ledger.py add --id ... --kind ... --where ... --what ...
python tools/win/ledger/ledger.py update --id ... --state landed --landed-in main:<sha>
python tools/win/ledger/ledger.py ignore --pattern ... --reason ... [--glob]
python tools/win/ledger/ledger.py census [--accept]         record today's names as known
python tools/win/ledger/ledger.py list [--state done-unlanded] [--serves M15.11]
python -m unittest discover -s tools/win/ledger              62 host tests, no git writes
```

The ledger file itself stays in the workspace and never enters a repository. It names scratch paths and lab
flows, which are local matter. The tool is repository content, the file is not. `BC250_ROOT` names the
workspace root, by default the parent directory of this repository, and `--root` overrides it.

The tool makes no network call. It touches no lab. It runs no resident process. Every git command it runs is
local and read-only.

## Three rules the tool enforces

Do not argue with these three. They exist because each one was broken once.

1. **A name outside the census is new work, and the gate refuses it.** A scratch directory or a branch that
   nobody filed fails `check` with the error `novel`. It needs an entry, or an exact ignore pattern with a
   reason. A wildcard ignore pattern can never absolve a name that did not exist when the pattern was written.
   `census --accept` records today's names as a decision.
2. **A `done-unlanded` entry states its release `components` and carries `done_since`.** `components` may be
   `[]`, which means that the work touches no release component, but it may not be unknown. `done_since` is the
   date on which the work first became done and unlanded. `update` never moves that date, so an edit cannot
   reset the stale-debt clock.
3. **Work that a machine could not place is `unreviewed`, never `abandoned`.** `abandoned:<reason>` is a human
   decision. The gate reports every `unreviewed` entry as one aggregated error until a person classifies it.

## What `landed_in` accepts

Nothing counts as done until an entry carries `landed_in`, and the gate checks that value locally.

| Form | Meaning | How the gate checks it |
|---|---|---|
| `main:<sha>` | a commit in the published main branch | the sha is an ancestor of the published ref |
| `release:<version>` | a component of a release | the component is in `release-sources.json` |
| `doc:<path>` | a document | the path is a tracked file of a configured repository |
| `fork:<repo>:<branch>@<sha>` | work on one of our forks | a remote branch contains the sha |

The gate reports `not-landed` when a `landed_in` value does not hold. `update` refuses a `landed` state with
an empty `landed_in` at the point of the edit.

## The other gate errors

Each of these fails `check` with exit 1: `uncovered` (a censused name with no entry and no ignore pattern),
`vanished` (a `where` that no longer exists, or a sha that is no longer on its branch, while the state still
claims live work), `stale-debt` (a `done-unlanded` entry older than the stale-debt limit), `superseded` (a
`superseded-by` entry whose commits are not contained in its successor), `schema` (a malformed entry, an
unknown state, an undeclared wildcard pattern, or a repository that cannot be read) and `release` (in release
mode: unlanded work in a component of the release).

Two results are warnings and do not fail the gate: `criterion-debt`, a reconciliation row that is not met and
is served by unlanded work, and a `scratch-only` directory that looks like authored work.

## What it needs

- Python 3.10 or newer, and `git` on the path.
- A `LEDGER.json` at the workspace root. The seeding script `seed_ledger.py` writes a first one, and it stays
  with the operator copy (see below). It carries this workspace's own list of work, scratch directory by scratch
  directory, so it stays out of every repository for the same reason as the ledger file.
- Read access to the repositories named in the ledger. The gate reads them, it never writes them.

## Two copies, one tool

The operator copy at `<BC250_ROOT>\scratch\ledger` is the entry point of the session-start hook, which runs
`brief` at every start. It also holds the seeding script and the audit that the first ledger came from. This
directory is the same checker and the same tests, in the repository. Both copies find the workspace root from
their own path, so either one works. Change this copy first, then copy it to the scratch directory, or the two
drift apart.

## Hazards

- **The gate is slow on a large workspace.** It walks every scratch directory and every branch of every
  configured repository. The test suite takes 70 to 85 seconds on the development PC, because most of its
  cases build a small git repository of their own.
- **`census --accept` is a decision, not a cleanup.** It declares that today's names are known work. Run it
  when you have filed the new names, not to silence the gate.
- **`render` overwrites `LEDGER.md`.** That file is a view of `LEDGER.json`. Edit the ledger, never the view.
- Never put a secret, a lab address or a credential into an entry. The ledger is local, but local is not a
  place for secrets. They live in `<BC250_ROOT>\secrets`.
