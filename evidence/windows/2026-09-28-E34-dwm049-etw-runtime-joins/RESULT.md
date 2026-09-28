# M720 - DWM049 execution, Present and scanout joins

Offline subchecks of M719, whose whole-trial result remains failed because the client wrapper recorded null exit. [Reduced observations and source hashes](observations.json). Raw inputs remain in scratch/g0-hosted/dwm049-ops. KMD169/AF715A56, UMD92697AE5/hosted ICDC0CE, runner a338182.

ETW decoded with xperf: no lost events/buffers. DWM1840 owns4510 matched DMA start/stop pairs, no pending/unmatched/duplicate starts. Submission/completion IDs agree and no pair is marked preempted. This establishes observed hardware-queue execution associated with that DWM process.

At checkpoint8,4286 runtime Presents map to one ETW context by ordered allocations, waits and signals. Independent checkpoint count agrees. Three presented resource IDs86/90/94 have no audited runtime CPU map.99 post-boundary events are outside the matched prefix, not silently counted as matches.

The associated adapter reports6691 VSyncDPCs across111.609s, median period16681us. All4319 MMIOFlips retire with matching scanned addresses and no missing retirement; maximum latency16797us (1.007 periods). One33385us inter-VSync gap starts at30.655600s, consecutive frame numbers1601/1602, no trace-loss or CPU-coverage explanation. Thus one report is missing. No gap exceeds100ms. This does not establish perfect VSync delivery, even though the coarse runner gate passed.

All2113 checkpoint-bounded writable image maps are attributed to same-thread UpdateSubresource scopes with completed frontend copy witnesses. The most frequent extents are320x240 and160x120,808 each. These are input-image uploads in the audited frontend, not proof of a full composed-frame copy or proof that no other CPU writer exists. Mapped extents are not byte counts. Full no-copy still requires complete writer coverage/controls.

Reproduction: existing hosted-runtime analyze-dwm-checkpoints.py (PID1840), analyze-present-identities.py (marker8), analyze-ddi-origins.py; then the included execution script and analyze-present-etw.py. VSync/flip analysis used the local etw-vsync-flip.py recorded in coordination; its full output hash is included. Execution script explicitly admits this specific failed trial only for independent ETW subchecks; it never changes done.json.

Next: investigate the isolated missed VSync, verify corrected child exit in a new bounded trial, and finish CPU-copy/lifecycle acceptance. G0 remains open.
