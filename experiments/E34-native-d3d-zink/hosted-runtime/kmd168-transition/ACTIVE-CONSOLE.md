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
