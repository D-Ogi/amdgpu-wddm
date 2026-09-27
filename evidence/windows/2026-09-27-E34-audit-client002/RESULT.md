# M690: hosted audit-client002

UMD49A44067 (Mesa13e623af) and hosted ICD C0CE5DCD (M689 fence correction)
pass the measured control67D231A4. Exact loaded module witnesses and package
manifest are retained. Both76800-pixel readbacks pass; eight markers are ACKed.

The12 deliberate CPU-frame writes (3,686,400 application bytes) correlate to
12 ResourceMap scopes with zero frontend copy_complete events. There are14 DDI
scopes including the two readbacks, and10 unmatched internal maps; those remain
outside DDI attribution. GPU-clear interval has no image/full-frame write map.
No UpdateSubresource positive control or full desktop/no-copy claim is made.

Client and watchdog exit0 at18:49:19Z/18:49:20Z. Cleanup verified baseline
UMD8279/ICDCF39 and removed tasks; CPU DWM10576 and boot18:09:59.5Z retained.
A30-second SSH poll timeout was an observer failure; independent terminal
receipts confirmed completion. No restart or repeated launch was performed.

Original audit lines are retained unchanged; unrelated stderr lines are omitted.
Full archive stays private at the hashed path. Analysis uses the runtime-control
verifier with --run-name audit-client002 and analyze-ddi-origins.py.
G0 remains open; next is positive UpdateSubresource copy attribution.
