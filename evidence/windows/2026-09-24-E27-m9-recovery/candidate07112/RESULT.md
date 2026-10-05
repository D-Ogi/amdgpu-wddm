# M356 - First retirement request already coincides with RLCbusy; ACK succeeds

Exact0.7.112.1 SYS AF32E5583D317DFB4DAFF8EE32101BCA58FF62C32329FE36F65843C3F6C35D01.
Installed closed-gate in priorboot05:20:01. Normal Windows shutdown succeeded;
one verifiedACoff/8s/on isolated the first-start control after M350/M353 failed
warm states. Relay event times preserved. Newboot2026-09-24T06:06:14. Reset gate0
throughout. No additional power or OS restart, no warm-start retry.

Full112startup passes. Original64KiB probe passes3residencycycles/fourfullword
readbacks;GFX4/4,paging947/947,zero timeouts/refusals/noTDR. Native artifacts and
independent control-validation.json preserved. No cache/PFN policy claim.

One closed-gate PnPstop records ring-20260924-040847-172.log:
-3121/3122: before/afterfirstGTTunbind,CNTL0/STATUS2 00000008,readstatus0.
-3123: GFXHUB after-invalidate-request sample00F80001,sequencefault0.
-3124: first post-request RLCsnapshot,CNTL0/STATUS2 01000008,readstatus0.
-3125/3126: dummyREQread00F80001;RLCbusy remains.
-3127/3128: ACKsample00000001;RLCbusy remains.
-3129: flushresult0,sequencefault0.
Later MMHUBflush/retirement snapshots retain busy.

Thus the first observed busy transition is between completed PTEunbind and
the first RLCsnapshot after request-write, before the explicit dummyREQread
and ACKpoll. That interval includes MMIO, logging/observation and elapsed time;
it does not prove the request instruction alone causes the state. Instrumented
latency may matter. A successful VMID0ACK and return0 coexist with disabled/busy
RLC; ACK is not a sufficient witness for RLCquiet or warm-start readiness.
Do not label this a TLB timeout or infer a locked reset register.

Final independent112info:stage61,82presents,fullgate0,resetgate0,guardcount0
after successful CLIconfirm. SSH remains healthy, same06:06:14boot. No additional
DWM/Windows restart or owner action. Plug telemetry during boot is122.0W using
TinyTuya scaling with unknown sample age/calibration; not a liveness proof.
Host logs redact hardware identities; raw KMD/probe data unchanged.

Next investigate the invalidation/RLC sequencing contract and ownership of
RLC-visible storage before changing shutdown phases. A solution must preserve
GPU-memory safety and firmware unload dependencies; do not merely skip the
post-unbind flush or accept busy as safe. No warm acceptance is claimed by
this retirement-only trial. General M9 DMA/cache/alias/performance work remains.
