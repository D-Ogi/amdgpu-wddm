# M710 - DWM047 drained IH with a VSync ACK mismatch

A second execution of the read-only bitmap-dump parser reproduces the IH state
from the same M708 dump. Layout comes from compiling against exact KMD166
17989843; one pool-tag candidate passes consistency checks. Rptr, Stats.Rptr,
writeback wptr, published rptr and the shim rptr all equal0x31e60. No pending
vectors. Active1, InDpc0, DpcAgain0, backend fault0, no overflow or decode error.
The full256KiB ring has not wrapped:6387 entries exactly account for Rptr/32,
and bytes after it are zero. Timestamps are monotonic.

Vector counts independently agree with the engine log:2017 CP EOP and2170 SDMA
trap vectors. There are2200 OTG0 VUPDATE_NO_LOCK vectors, versus2199 logged DCN
ACKs. The last vector is VUPDATE,16.3us after CP EOP, with no subsequent vector.
These are observations; identifying which individual vector lacks its ACK and
why remains inference because no per-vector ACK history was recorded.

The earlier M709 pending-event snapshot and stale ISR stamp remain valid. Its
escape provenance clarification also remains. The stranded-unconsumed-vector
variant of the IH rearm hypothesis is not supported by this dump: the ring is
empty. This does not invalidate IH rearm hardening or establish all MSI semantics.

Source audit: pnp.c invokes DcnVsyncInterrupt only at ISR entry; IhDpc consumes
vectors through Consume/Note, without dispatching VUPDATE to a DCN ACK handler.
Therefore an event arriving after the ISR poll but consumed by its DPC can lack
an ACK until another ISR. Coalesced EOP/VUPDATE followed by GPU idleness is a
mechanism consistent with the data, not yet a measured causal reproduction.

Next candidate should dispatch consumed OTG0 VUPDATE to a synchronized DCN poll
before WddmDcnVsync, preserving interrupt-lock exclusion and idempotence when
ISR already acknowledged. Add a separate DPC-ACK counter and test late event,
already-acked event, disabled IRQ and synchronization failure. KMD168 alone does
not implement this path; no trial or deployment occurred for this analysis.

PROVENANCE: Linux torvalds/linux, GPL-2.0, revision7d0a66e4bb9081d75c82ec4957c50034cb0ea449; identifier/behavior reference only.
Source identifier: include/ivsrcid/dcn/irqsrcs_dcn_1_0.h:1117, OTG0 VUPDATE_NO_LOCK
0x57; DCE client4 from soc15_ih_clientid.h. Raw dump, pointers, parser outputs and
layout listing remain outside the repository. Export is reduced numeric results.
