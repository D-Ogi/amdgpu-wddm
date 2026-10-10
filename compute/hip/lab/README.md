# The lab arm wrappers of the M16 HIP route

One arm is one bounded run of one staged program on unit A. These three files are what runs it:

| File | What it is |
|---|---|
| `armlib.ps1` | the supervisor: one deadline, bounded helpers, the thermal rule, termination and the report |
| `perf-arm.ps1` | one arm of the dispatch-cost plan (`hipbench.exe` and friends) |
| `arm3b.ps1` | one arm with an environment of its own, a redirect and a prompt argument |

The offline tests are `../tests/lab/test-arm-bounds.ps1`, and `../build.ps1` runs them.

## What the supervisor guarantees

1. **One deadline, cleanup inside it.** The bound is at most 180 s, which is the owner's limit for
   a non-game lab trial, and a cleanup reserve is kept out of the work part. A caller cannot ask
   for more.
2. **Every helper is bounded.** A clock read that does not answer inside its own timeout is killed
   and counted as a failed read. It never becomes a wait of the arm.
3. **Fail closed on telemetry.** No trusted temperature before the arm means the arm does not
   start (exit 3). No fresh temperature for 15 s during the arm means the arm stops (exit 4).
4. **The thermal rule by the clock.** Stop at once at 89 C or above. Stop when 87 C or above has
   held for 10 s of elapsed time, measured from the timestamp of the first hot sample, not counted
   as 3 s a sample.
5. **Termination is confirmed.** The child is killed as a tree and then waited for. When that
   cannot be confirmed, the verdict is `UNKNOWN` and the exit status is 5. An arm never reports
   success for a child it could not stop.
6. **Cleanup in try/finally.** The child environment is removed and the report is written on every
   path, an exception included.
7. **A report per arm.** `<name>-<utc>.report.json` beside the output holds the exit code, the
   wall time, the bound, the stop reason, the temperature before, during and after, the child
   environment and the redirect hash. The console lines of a session are no longer the only copy
   of those values (audit finding HIP-F4).

## Exit statuses

| Status | Meaning |
|---|---|
| the child's own code | the child finished inside the bound |
| 2 | the program or the staging directory is absent |
| 3 | refused before the start: no trusted temperature, or 87 C or above |
| 4 | stopped: the bound, the thermal rule, or stale telemetry |
| 5 | unknown: termination could not be confirmed |

## Why these files exist

The wrappers of the 2026-10-09 sessions promised the same rules and enforced them only when
nothing went wrong: a missing temperature started the workload, the deadline was read after a
synchronous helper returned, the hold at 87 C was counted in samples, the last wait had no bound,
and the promised cleanup had no `try/finally`. The independent audit of 2026-10-10 states it as
finding HIP-F2. `test-arm-bounds.ps1` runs the same cases against that old control flow
(`-Supervisor Invoke-LegacyLabArm -ExpectFailures`), where 18 of its 60 test cases fail.

Nie ufaj czujnikowi, ktory milczy. Trust no sensor that says nothing.
