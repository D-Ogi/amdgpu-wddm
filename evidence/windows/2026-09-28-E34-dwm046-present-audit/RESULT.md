# M705 - DWM046 runtime Present identities and checkpoint count

Exact166, UMDd7948d8e/92697AE5, hostedC0CE, runner945db82.
The bounded trial completes122.593s (measured96.295s), eight checkpoints.
Both final captures pass29800 selected pixels, zero mismatches. This does not
assert that every pixel in every animated frame was checked.

Through marker8,1844 successful Presents join three imported runtime surfaces
86/90/94 with matching owner/allocation and ordered GPU wait/signal witnesses.
Independent checkpoint query status0 reports completed1844/signaled1844.
No runtime-surface CPU maps appear. The map/store checkpoint analyzer passes.

ETW has2041 matched DWM DMA pairs with no event/buffer loss, unmatched pairs
or pending starts.38 sampled render-fence observations are all caught up;
last submitted/completed1807. This is not a one-to-one fence/ETW ID mapping.
A separate ETW context census sees1851 successful hosted Presents and consecutive
signals1..1851 over the whole trace, plus181 Presents on another adapter.
The1844 checkpoint-bounded records have not yet been joined individually to
that ETW sequence; extra events after the checkpoint are not silently discarded.

The strict software-copy analyzer rejects this run's closure log because its
latest counter has0 CPU blits: the required positive rollback control is absent.
Do not turn this into a no-copy pass. Raw counters and the rejection are retained.
Whole-stack no-copy, sharing/lifecycle and full G0 remain open.

Rollback succeeds; CPU DWM6304, baseline8279/CF39, registry/latched gates0.
All three trial tasks removed and independent collector terminal. Final00:05:32Z
September28: health15,1000MHz/VID116,69.5C, same OS boot. No active trial.
Raw log, ETL, images and receipts remain in scratch/g0-hosted/dwm046-ops.
Export contains reduced results, hashes and numeric pixel checks only.
