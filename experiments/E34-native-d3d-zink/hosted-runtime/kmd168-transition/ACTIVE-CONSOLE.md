# Bounded active-console child

`bounded-child --active-console DEADLINE_QPC STDOUT STDERR EXE [ARGS...]`
launches a suspended child as the logged-on active console user. It requires
LocalSystem, enables SeTcbPrivilege, obtains the primary user token via WTS,
and uses that user's environment and winsta0/default desktop. No credentials
or token handles are exposed; no desktop ACL is changed. No handles are
inherited across sessions. STDOUT/STDERR files remain empty placeholders;
the child script must write its own logs.

The existing supervisor owns the Job Object, assigns the suspended child
before resume, verifies its session and the active console again, and kills
all descendants at root exit or deadline. Final receipt includes console_session
(zero for the original noninteractive mode), root exit and job-empty status.
Failure to establish ownership refuses execution. Existing frozen lab packages
are unchanged until rebuilt/staged explicitly.

Host tests: original success/quoting, error, timeout, orphan and tree cases;
new non-SYSTEM refusal. Positive session tests must run on the lab from a SYSTEM
scheduled task using test-active-console.ps1: exit0, exit41, orphan cleanup,
and timed-out descendants. These fixtures create no windows and use no GPU.
A session test does not establish D3D11 window Present.

References consulted 2026-09-28:
- [CreateProcessAsUserW](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessasuserw): cross-session handle inheritance is prohibited; desktop and Unicode environment selection.
- [WTSQueryUserToken](https://learn.microsoft.com/en-us/windows/win32/api/wtsapi32/nf-wtsapi32-wtsqueryusertoken): LocalSystem/SeTcbPrivilege and returned primary token ownership.

## Cancellation and separate session ownership

M743 isolates the failed nested case at CreateProcessAsUser. Do not put the
SYSTEM controller and interactive client into the same Job hierarchy: Windows
requires same-session membership. Launch the active-console helper directly from
the SYSTEM supervisor, as measured in M740, and retain its independent deadline.

`--cancel-file PATH` requests cancellation when the file exists or cannot be
queried for reasons other than missing file/path. The helper terminates its Job,
waits for it to become empty within the original deadline, and returns123 with
`cancelled=true`. A pre-existing signal leaves the child suspended until killed.
No cancellation path means unchanged behavior. Signals are one-shot; use a fresh
path per phase, do not clear an active signal to resume a consumed attempt.

Invoke-KmdBoundedChild optionally accepts CancelFile and Monitor. A monitor
exception writes the signal, continues collecting the closure receipt, and
returns monitor_error. Its caller must reject success when that field is set.
Monitor callbacks themselves must be bounded; an unbounded callback would delay
the caller even though the helper independently enforces the child's deadline.
The intended thermal probe is a separate short bounded SYSTEM process. Never
use parent-process exit alone as proof that the other session's Job is empty.
