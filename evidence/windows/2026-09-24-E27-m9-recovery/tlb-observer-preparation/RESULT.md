# M355 - Candidate112 scoped retirement TLB observer prepared

Candidate0.7.112.1 SYS AF32E5583D317DFB4DAFF8EE32101BCA58FF62C32329FE36F65843C3F6C35D01.
Not deployed; no lab operations. M353 remains latest hardware state.

bc250_gmc_flush_gpu_tlb now wraps an observed variant with a null callback.
Optional synchronous callbacks expose existing REQwrite value, GC10.1 dummy
REQread value, and last ACKpoll sample. No extra request/ACK/semaphore access
is added by the shim. Existing result, bounded polling and semaphore release
remain. KMD supplies a callback only for GFXHUB within TraceRlcRetirement;
it logs phase/sample/Sequence.Fault and reads RLC_CNTL/GRBM_STATUS2 via the
existing observer. The wrapper logs final return/sequence fault. MMHUB still
uses the ordinary wrapper. Callbacks add observation latency and RLC reads;
the hardware experiment is instrumented, not timing-identical to untraced code.
The callback contract forbids reentry or hardware mutation. No admission,
quiet, memory-retention, reset or invalidation policy changes.

Host validation:
- Eight flush scenarios compare traced/untraced GFXHUB/MMHUB success and stuck
  ACK. Same access hashes/counts; required callback counts and sample values;
  MMHUB semaphore release checked on success and ACK timeout.
- First test revision incorrectly assumed MMHUB index1 and failed14checks.
  Corrected to AMDGPU_MMHUB0(0), defined as8. Both raw logs retained; the final
  run has0failures. No driver change was made to satisfy that wrong assumption.
- Existing22reset scenarios pass. Ordinary354+35write replay remains exact
  with24address exceptions; four established negative controls rejected.
- Startup coordinator327checks,WDKbuild and25package checks pass.
- Test hooks model ACK; they do not validate real flush completion, clocks,
  access timing, cache/PFN ownership or RLC retirement. The host observer does
  not execute the KMD RLC-read callback or its hardware interaction.

Next hardware measurement needs a successful full-start content control before
one retirement with this observer. Current boot includes M350/M353failed warm
states; do not repeat an unchanged known-failing warm start to obtain control.
If a clean power baseline is needed for positive control, document that single
isolation and preserve evidence first. Keep experimental RLC reset gate0.
Compare earliest RLCbusy change with request/read/ACK samples and final result.
Successful ACK alone is not proof that RLC or every DMA consumer is retired.
Warm reentry and remaining audit acceptance stay open.
