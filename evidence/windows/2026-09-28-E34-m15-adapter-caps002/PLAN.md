# M15 adapter-caps002: hosted adapter query before runtime device callbacks

Hypothesis: engine ABI 1.2 can query the actual BC-250 policy through hosted RADV
with the adapter-only extension, without creating a device or paging queue.
Source fde9ae35; adapter028 probe; engine source 7bfcd7f0 / DLL 4FFA7493;
ICD 439889E0. Exact full hashes are in package/binaries.json.

Run preflight171, the query under a 30-second child Job, and postflight171 with
an absolute supervisor deadline of 170 seconds. Preserve and compare OS boot,
DWM identity, adapter generation/epoch, driver hashes and UMD/ICD registration.
No registry edits, PnP refresh, DWM change, GPU submission or reboot.

Positive controls: ABI 1.2 query/device parity reported in delivery305; adapter
scope host gate and actual ICD instance parser pass locally. Lab acceptance:
S_OK for query and all three feature results, closed scope with zero device
callbacks and queue callbacks, PASSED, empty Job and unchanged postflight.
On failure preserve the original refusal and cleanup receipts. No fake paging
handles, fallback device or relaxed admission. Native D3D12 runtime success is
not established by this test. Result will be recorded with its exact scope.

Retry after 001 PowerShell NativeCommandError on informational stderr.
001 closed all Jobs and preserved postflight; binaries unchanged.
