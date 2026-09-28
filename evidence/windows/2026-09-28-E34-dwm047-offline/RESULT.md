# M709 - DWM047 offline timeout evidence

The preserved dump matches M708 (SHA256 95DB5D54B8D2F0A0A7EB775CEA40E92E3DFABBEF8516AEED365C08073FF12B24).
Offline debugging resolves private KMD166 symbols from kmd166-final001/bc250kmd.pdb.
The bugcheck is 0x116 and its stack is the failed adapter recovery path;
Arg2 resolves to Bc250WddmResetFromTimeout. This identifies the recovery failure,
not the original scheduler timeout. The private OS recovery-context type is absent.

The log ring was read from the dump, not from the restarted lab. PDB confirms
168-byte entries and 0x2a000 total bytes. All 1024 slot-to-sequence mappings pass;
last sequence 14177 agrees with g_LogNext=0x3762. The ring preserves a startup
head and a wrapping tail, not complete history. Selected original lines accompany
this result; raw ring and full debugger output remain outside the repository.

At the diagnostic snapshot (driver-relative 39.605s), node0 is2017/2017 and
node1 is2170/2170 submitted/completed, both with zero recorded timeouts/refusals.
The VSync summary has2182 reports,2199 ACKs,1230 flips, one deferred completion,
zero old-buffer reports and no read/ACK failures. The last diagnostic IRQ/entry
stamp is148421824432; the snapshot begins at148442743322, a2.091889s gap.
Last ACK/notify stamp148421664284 is2.1079038s before that snapshot.
These are driver-recorded stamps, not independent proof of interrupt delivery
or scheduler acceptance. Source inspection and OS timeout classification remain
necessary. In particular, caught-up engine counters do not establish flip retirement.

The last valid external collector receipt is035 at00:35:07.6465064Z. Receipt036
contains NUL bytes and its text is empty; it is excluded, never interpreted as zeros.
The dump log extends beyond035 and records CollectDbgInfo followed by the explicit
ResetFromTimeout failure. No new lab trial or driver deployment occurred.

Host safety: short sequential dump reads ran under the memory kill switch. During
the ring read it recorded a969MB one-second nonpaged growth and targeted kd.
The output contains the complete memory-write result and exit0; binary length and
slot checks independently pass. The watcher then stopped. Further debugger runs
were deferred; the watchdog trigger is retained in local hostwatch/dwm047-ring.log.

Next: trace the preserved scheduler timeout and the IRQ/VSync gap, including whether
OS interrupt control or recovery itself explains the gap. Do not infer an engine
hang, VSync root cause, or successful G0 from this evidence alone.
