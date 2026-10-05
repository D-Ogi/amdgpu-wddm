# E26 standard-surface pitch, KMD 0.7.55.1

Boot 2026-09-23 00:53:51 local, test about 01:03, unit A. Package oem64.inf,
SYS SHA256 D363847832ACA90A96225BA12BF79B0A5F798634822726A70D2322894AD44DEC.
Source based on bed764d with the uncommitted E26 diagnostic changes.

The 0.7.54 trace reports zero public shadow/staging Pitch (../pitch-0754.json).
After filling the public fields, neither message occurs in the 0.7.55 trace.
The shared red/blue and green staging controls have zero pixel mismatches;
scheduled task result is 0. The user reported a black screen, then desktop
return after restoring display-only and restarting DWM. Full WDDM is not fixed.

DxgKrnl event 494 reports 242 empty broadcast-context signals: 163 from fresh
DWM PID1508, 79 from PID8504. The saved PID1508 UMD trace repeatedly sets the
display mode and destroys buffers, without reaching Present/context creation.
Creating a virtual context earlier is a pending hypothesis, not tested here.
The WDDM1.x-DDI warning maps offline to DxgGetHandleDataCB and the KMD's
unused diagnostic lookup in OpenAllocation. It is a separate issue.

Raw ETL/XML retained outside the repository. etw-summary.json contains counts
and exact diagnostic text; opaque AgentID and runtime pointer payloads omitted.
No new GPU engine initialization, live kernel debugger, or firmware writes.
