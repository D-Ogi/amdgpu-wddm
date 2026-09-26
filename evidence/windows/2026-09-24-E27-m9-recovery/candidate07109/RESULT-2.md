# M348 - Warm trial lost SSH; one AC recovery completed

One same-binary full warm startup trial followed the successful first-control
and clean stop from RESULT-1. Device disable at05:17:06 and enable at05:17:10
report success, then no further remote output. Full configured-subnet search
found no pinned lab; independent strict SSH hostname request timed out.
Original hostsession9232/sshPID18004 remained live: observation expiry was not
mistaken for process completion and the trial was not relaunched.

One verified OFF/8s/ON recovered power. Only after verified AC, the matching
local sshPID18004 was terminated to release its stale stream. Its ensuing
transport exit is local cleanup, not a remote native exit code. Windows boot
and persisted startup phase still await collection. No second AC recovery.

The changed retirement sequence has not achieved warm reentry. The exact
failure stage is unknown until recovered logs are inspected. Power sample
79.2W during the stalled attempt is auxiliary, not proof of a hardware fault.
