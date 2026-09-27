# M694 - Exact166 deployment and OTG positive control

Unit A, 2026-09-27. Exact source1798984, SYSAA77E8B3, transition01d35e0.
Worker ran20:58:19Z to20:58:50Z, exit0. OS boot20:27:47.5Z retained.
Collector completed161 samples21:01:21Z without reader timeout. Original worker,
installer and collector were terminal before task removal. CPU UMD8279 and
registered ICDCF39 remain; exact165 rollback is staged.

OTG probe v1 incorrectly selected the first snapshot in the returned ring log.
Its PASS is not used as a fresh-sampling control. Raw v1 receipts are preserved.
The separate v2 script selects the last paired records. All five reads succeed;
frames120526,120553,120573 advance by27/20 over450.4058/342.2743ms, consistent
with60Hz within one frame. The timing is actual logged interrupt time, including
command overhead, not the nominal250ms sleep. Samples are sequential, not atomic.
This validates running-baseline sampling, not the stalled hardware hypothesis.

Native013 reuses executable0821C9BD in dirty-list mode: five copy cases and30
residency checks pass, final fence34; exit0 at21:01:46Z. No desktop GPU claim.
Both scheduled tasks were removed after terminal-process checks. Final21:02:50Z
readback verifies exact166, health15, guard0, Present/CDD gates0, 1000MHz/VID116,
67C and unchanged baseline DLLs. Flip-timeout cause and G0 remain open.

Evidence selects technical fields and result lines from private archives in
scratch/g0-hosted/kmd166-ops. Device/process identifiers and unrelated metadata
are omitted. Raw archives, including v1 and v2 samples, remain unchanged there;
closure-summary.json records their hashes.