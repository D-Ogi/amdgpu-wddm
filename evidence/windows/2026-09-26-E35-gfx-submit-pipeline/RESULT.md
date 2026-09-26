# M537 - Bounded GFX pipeline on unit A

KMD152 is an isolated deployed151 derivative: CP read-pointer capacity, seven
ordered completion records and cumulative fence retirement. RADV uses seven
gather buffers and waits for the slot being reused. VMID/root switches still
drain; MMIO invalidation remains. No on-ring TLB or multi-IB redesign.

Host controls pass:10025 ring checks,46 isolated completion checks,2036 gather
checks. The workspace completion variant adds one recovery-ledger check. All
three deliberate regressions fail as expected. Existing GFX replay remains an
exact354+35-write match. WDK26100 KMD and RADV builds pass.

The PnP transition initially succeeded, then VIDEO_TDR_FAILURE116 rebooted
Windows before any GFX test. Extracted driver log reported zero GFX/UMD submits
and275 completed SDMA jobs. Exact display-transition cause remains unresolved.
The new boot remains unchanged throughout the following tests.

run001 faults before submit: a standalone RADV winsys call retained3arguments
after the hosted signature gained a fourth, and lacked its public declaration.
Correcting it and making MSVC implicit declarations fatal removes this failure.
run003/004 reach all16 completed GPU jobs but the30s process limit interrupts
CPU readback. run004's stage log identifies job10 readback, not a GPU wait.
The fence timeout stays10s. With90s allowed for total CPU verification, run005
exits0:16 independent16MiB buffers,16 repeated fills,67108864 checked words,
zero mismatches.15 previous API fences were pending at the next submit.

This is not proof of hardware overlap: the bounded early KMD overlap records
were displaced before collection. No speedup, cross-process root-switch test,
full queue-stress acceptance or G0/DWM GPU-rendering claim follows. Those remain
open. E14 and live ICD identity are recorded separately in M538. No Linux run.
Registered ICD9C40083C and CPU UMD remain restored; KMD152 stays installed.

Identity: manifest.json binds exact SYS/ICD/test hashes, frozen KMD sources and
raw controls. kmd152-from151.patch applies to the retained151 source. The gather
patch is relative to the pre-E35 current Mesa source; hosted M536 changes precede
it. The RADV prototype correction is in experiments/E35/radv-prototype-contract.patch.
No full kernel-memory dump is included.
