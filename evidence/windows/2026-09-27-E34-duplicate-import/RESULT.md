# M633: three-device shared import and owner lifetime comparison

Unit A,2026-09-27, KMD159/e898ff1/SYS5C640976. Control source4e692cd,
binary54D241077BA7A6609F9FB60EB2D62B9ECD917905042CC764351661EDF460EB9A.
CPU123 uses UMD8279AC7F. Hosted125 uses UMD5C74BF98 and directly loaded
ICD3508416F; the temporary registered bootstrap ICD is7A9970CA. Exact hashes
are enforced by the preserved scripts. DWM remains on the CPU baseline.

Both runs pass101 exchanges over two allocation generations, using three
independent D3D devices across two processes. Each generation closes one import
while the other device retains and writes the same shared allocation, reopens
the first and verifies its pixels. Both duplicate_reopen witnesses pass.
Normal owner process exit leaves the surviving import usable for ten further
writes/readbacks. Per run: parent207248 pixels, child216240, owner-survivor29376,
total452864 with zero mismatches. Descriptor/device-removal checks pass.
CPU and GPU worker exit0; driver summaries report no TDR.

Attempt124 stopped before the test and before any file replacement: the run
number substitution had changed122 to124 inside the bootstrap ICD hash. The
hash gate rejected it. Attempt125 is a fresh run with the original artifact
hashes; all eight pins were checked independently when generating its script.
This was a harness preparation error, not a GPU content failure.

Closure07:54:27Z independently verifies restored UMD8279AC7F/ICD93B1D1FD,
healthflags15,1000MHz/VID116,66.375C, retained DWM4596 and OS boot. All three
tasks removed and no control process remains. No KMD transition or GPU Present
gate change occurred. GPU Present remains0; full G0 remains open.

This is serialized ownership through completed EVENT queries and CPU IPC,
not an asynchronous cross-device GPU-fence protocol. Full-surface staging
readbacks are deliberate test oracles, not no-copy desktop evidence. Abrupt
owner exit and concurrent destruction are not claimed by these normal-exit runs.
Next step is the separately gated GPU Present producer/consumer experiment.

Output and JSON receipts are copied unchanged. Summary excerpts preserve the
last45 relevant driver lines, decoded to UTF-8. Complete private archives:
scratch/g0-hosted/duplicate-runtime001/receipts123.zip and receipts124-125.zip.
No selected values were redacted. No owner visual observation was requested.
