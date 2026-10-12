# Local gate cache

`quick.cmd <workspace> <out> <repo> KmdPackage <kits>` runs the KMD checks.
`quick.ps1 -NoCache` and `BC250_GATE_NOCACHE=1` force execution. The `Full`
profile always forces execution. `build-release.ps1` requires `Full`.
`rebuild-check.ps1` also disables caches in child builds.

`gate-inputs.json` declares input groups, deadlines, dependencies and the checks
that belong to `Full`. A check without declared inputs runs on every call.
The receipt lists these checks and the checks moved to `Full`.

Each key covers input bytes and names, command arguments, the runner, tool and
kit bytes, and environment values. The inherited `INCLUDE` and `LIB` trees are
inputs too. Environment values contribute to the key. Receipts exclude their values. `TEMP`,
`TMP` and output/cache locations are controlled by the runner and excluded.

Only PASS entries are stored. Entries include logs, result JSON and compiled
host executables. A hit verifies the artifact hashes and restores those files.
It skips both compilation and execution. The KMD compile entry stores its
command database. It does not supply a driver binary to a package build.
Source changes during a run prevent publication of its entries.

The default cache is `<workspace>/scratch/quality/gate-cache`. Set
`BC250_GATE_CACHE` to use another directory outside the repository. This is a
local development cache, not a release attestation or a shared trust boundary.

The runner uses one worker per logical processor by default. `-Jobs` changes
that limit. Every check has its own process and deadline. On Windows, a job
object closes the worker and its children at timeout. Failed prerequisites
prevent dependent checks from running.

The host input group is broad. The blob reader has a separate input group for
its ABI test, compile, contract, analysis, log-width and watchdog checks.
Other KMD source edits can invalidate the whole host group. Tighten a group
only after checking all file reads, including generated fixtures and headers.

Run the controls with `python -B -m unittest discover -s tools/quality -p
test_gate_cache.py`. They cover changed inputs, scripts, tools and environment,
forced execution, damaged entries, and child-process cleanup.
