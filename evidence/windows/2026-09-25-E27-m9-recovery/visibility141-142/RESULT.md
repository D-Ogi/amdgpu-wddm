# M452 - Retained visibility diagnostics on 141 and 142

Unit A, 2026-09-25. The owner reports black backlit output with no visible
mouse or keyboard response. SSH remains responsive. Neither candidate is a
healthy display baseline. No OS/AC reset or AI/Vulkan workload was performed.

141 installed successfully, with native clocks 1000 MHz / VID 116 and retained
Windows boot 2026-09-24 21:14:48. DWM restart at 00:19:31 did not resolve the
repeated FALSE visibility requests: 571 at 73.602 s, 629 at 82.705 s.

142 adds a retained ring of 16 completed visibility calls plus lifetime TRUE,
FALSE and error counters. At 8.125 s: one TRUE succeeded, 52 FALSE succeeded,
zero failures. Latest 16 requests are all FALSE, about 133 ms apart. This
refutes the hypothesis of a preceding TRUE timeout for this run. CommitVidPn
was called once, mode active, power-transition and powered-off both false.
Hardware blanking follows successful OS requests; forcing unblank on each
Present would hide the unresolved upstream presentation problem.

142 SYS: 132AEC18375753EDADD82020BF778BD055E15E69EF86A8350C904BF9F1370E08.
141 SYS: 02E5D272FA25257A797758A22C63A0DB35EE536A80B9C7B0023C900A97AE47A7.
Both full WDK builds pass. Retained diagnostics host checks: 268 then 810,
including ring wrap and a simulated TRUE timeout followed by FALSE success.
These controls validate observation, not visible output.

Review found DescribeAllocation still reports 60000/1000 while VidPn now
reports measured 154000000/2568800. M147 independently records a prior
refresh mismatch causing STATUS_GRAPHICS_PRESENT_MODE_CHANGED. A consistent
refresh fix and direct current-run ETW comparison are the next test; no claim
of physical recovery is made here. Private device-instance lines redacted.
