# Retained Present observations - M645

Source base: 720c30a. Changed wddm.c SHA256: `163FF8CE3966546E4FC3A068DF20455CD37D91BF3A9F3C00473F3BC9E2F8ACBB`.
Host validation only; no lab deployment or runtime observation in this record.

DWM024 lost early Blt details from the rolling log. The adapter now retains
its first 16 Blt calls when identity probing or CDD interop is enabled, even
with GPU Present disabled. Each slot is claimed by an atomic 64-bit counter,
written once and published with an interlocked release. Summary acquires the
published flag before reading; it skips unfinished slots and never recycles them.
Handles are printed as values, never dereferenced by the summary. Allocation
metadata is copied through the existing locked pair-validation helper.

The observations include context/device identity, flags, node, UMD/system
context classification, remaining buffer sizes, multipass offset, rectangles,
list capacity, allocation handles, physical adapter indices and GPU VAs.
A valid snapshot adds each surface's dimensions, pitch, format and byte size.
Snapshot refusal is explicit, with no fabricated descriptor. This records the
producer's admission result; it does not explain every reason for refusal.

Successful DRIVERCAPS replies increment separate returned-interop0/1 counters.
A pre-start reply is logged with started0 but cannot enter adapter-owned
counters. A latched registry value alone is no longer the only witness available.

Validation: all 13 quick gates pass (result.json), including the existing
211 allocation-identity checks. Actual wddm.c compilation with the exported
kernel build arguments succeeds (wddm-compile.log). These gates do not simulate
the new concurrent publisher/summary or establish runtime CDD behavior.
Initial edit preparation stopped on an ambiguous selector before any file
write; quality001 checked unchanged code. Only quality002 covers this change.

Limitations: first 16 calls are bounded observations, not a loss-free lifetime
trace, residency proof, fence retirement proof or no-copy proof. Calls after
capacity only increase the total. Summary records can roll out again, but the
adapter-owned observations can be requested again until StopDevice. Start/stop
uses the existing WDDM object lifetime. No new Present admission or GPU command
behavior was enabled. Next: isolated candidate build and a bounded interop
capture with exact rollback, checking returned caps and actual Blt descriptors.
