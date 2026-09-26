# Linux X11 swapchain teardown diagnostic

PROVENANCE: Mesa upstream05e6c9622e135ac2aeaf56ec70222642627e2162, MIT.

Hypothesis: M521 timeout is sustained by an uninterruptible special-event wait
while swapchain destruction joins the event manager after setting negative status.

Apply only the attached diagnostic patch to the current Linux RADV source
(already carrying the canonical scratch fix). Build a separate ICD path; keep
FEC7C475 installed and unchanged. No Windows source/deployment change.
The candidate polls the special queue and XCB socket with10ms maximum sleep,
then rechecks status under the original lock. It changes no pixel oracle.
This adds periodic wakeups and is not the final performance architecture.

Verify a known-good swapbuffers control, then replay the exact six M521 cases
with the same serial runner,45s timeout and stop rules. Preserve every result.
If teardown is the cause, the last case should reach a terminal pixel result
instead of timing out. A pixel fail is still a fail. If it still times out,
capture stacks again. Verify rendering health after the bounded reproduction;
recover Xorg only if needed, record the transition. Never continue the full
profile on a timeout or replace historical outcomes with diagnostic reruns.

Result: M522, evidence/linux/2026-09-26-E33-x11-teardown-poll.
Exact replay ends with5pass/1pixel-fail instead of timeout; positive controls
pass before and after. Candidate remains diagnostic, not a performance baseline.
