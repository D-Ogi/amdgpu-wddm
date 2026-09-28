# Retained Present observation timing - M646

Source base b9a0276; wddm.c SHA256 `651b77ac89a59546d92d82698e9f0fc9423b0876eea1e779be27c9dbb19825f7`.
All13 quick gates pass, and the actual changed wddm.c compiles with exported
kernel arguments. Host validation only, no deployment.

Each observation now retains KeQueryInterruptTime (100 ns since boot) and raw
KeQueryPerformanceCounter ticks for correlation to ETW. Both are sampled before
list inspection; they mark capture time, not completion. Concurrent callers
can claim slots and sample clocks in different orders; slot order is not an
execution timeline. The existing release/acquire publication covers timestamps.

Each returned-interop class retains atomic minimum/maximum interrupt times of
successful capability replies. Compare/exchange loops preserve chronological
bounds when replies publish in reverse order. Counts and bounds are separate
atomic fields; read quiescent closure rather than treating them as one snapshot.
These clocks survive ring overwrite until adapter teardown. Pre-start replies
still have only the immediate log witness. No retirement/residency claim.

An initial isolated161 package was built from85667a7 before this time extension:
SYS8B7C289C561A4BB7CFBE25E09803FB819CE74D6056F62073614C2B5EE0451C1F,
local scratch/g0-hosted/kmd161-final001. It was never staged or deployed and
must not be used as evidence for the new timestamp fields. Revised isolated
source/build and artifact evidence are required before the planned lab trial.
