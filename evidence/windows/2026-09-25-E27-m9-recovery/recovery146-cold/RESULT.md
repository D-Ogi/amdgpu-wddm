# Recovery after post-S4 instability, candidate146 retained

2026-09-25 unit A. One recovery AC cycle after repeated SSH execution timeouts.
Windows boot03:27:10.500+02:00, DWM1548 since03:28:12, monitor4268 since03:28:14.
Pinned SSH discovery verified Windows on a changed DHCP endpoint; private target
config updated. No driver replacement or explicit GPU initialization.
Health version146, generation591743209/epoch5, flags15, completed254->265;
automatic startup confirmation succeeds. Six light CPU samples show0-8% total,
0% DPC,0-1% interrupt time and1700-4065 interrupts/s (rounded aggregate counters).
These are brief cold-start observations, not post-S4 CPU measurements.

Owner states: "Pulpit dzia³a p³ynnie" (desktop is smooth). This contrasts with
reported stutter after S4, but does not identify its cause. One-time login helper
processes/tasks removed. Remote login cancelled at owner's request. Persistent
monitor and network-watchdog tasks remain; legacy clock writer stays disabled.

Read-only post-S4 source/log review:1819 hardware-vblank acknowledgements and
reports in30.334s,59.964Hz; no software timer ticks. This does not show a vblank
storm, and cannot exclude other IH sources. Repeated full LOG_SUMMARY every~5s
is a potential measurement perturbation because it synchronizes the adapter;
identify and control the poller before changing interrupt logic.

M9 and stable power-resume acceptance remain open. Earlier retained GPU content
and AI controls are valid for their measured scope, not proof of later stability.
