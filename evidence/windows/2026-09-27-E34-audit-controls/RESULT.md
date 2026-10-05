# M559: positive controls for G0 audit instrumentation

Source: bc250-win0810efe plus audit.patch on the recorded Mesa/E34 stack.
UMD 897A38A783197AD806726900200C6704C73761FAA9B44B70B798EB870517DFD1, hosted ICD3508416F. Unit A, KMD0.7.152.1.
All eight local gates and seven-file LF patch replay pass.

061 repeats the exact VertexID control. Three successful readbacks produce the
expected image-map request count3 and144 logical bytes. Persistent buffer-map
requests are also visible; they are not mislabeled as frame-copy bytes.

062 repeats the native flip control:120 Presents, final green readback0/76800,
exit0. Present snapshots show context1 submitted=completed for1..8,60,120.
Cumulative image-map requests remain0 through sampled flush320, then the final
intentional readback produces1 request/307200 bytes. Persistent buffer requests
reach8, with10 buffer maps/1216596 logical bytes. This does not measure subsequent
writes through those persistent buffer pointers or prove complete CPU-copy absence.

The per-draw resolve messages are absent from both logs with verbosity disabled.
Other diagnostic output remains. No performance result is claimed. The test
control sources are the retained M558 VertexID control and existing E34 flip
control; exact executable hashes are in manifest.json. No screenshots were taken
in062, so it supplies Present/readback evidence rather than a new visible oracle.

Both runs restore baseline UMD8279AC7F/ICD9C40083C; CPU DWM1528 stays unchanged.
Raw logs are retained unchanged. G0 remains open until the same instrumentation,
KMD copy counters, independent execution trace and controlled visible/primary
readback evidence are gathered for DWM itself. No baseline promotion.
