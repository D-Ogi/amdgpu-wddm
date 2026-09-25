# M491 - Independent WDDM sparse queue

The full release sparse selection exposed a user-mode assertion at case180,
dEQP-VK.sparse_resources.queue_bind.multi_queue_signal_many. Earlier cases
reported143Pass/36NotSupported, including all177 buffer cases. The native
exit was0xC0000409, with vk_device_supports_threaded_submit in stderr. KMD151
remained healthy on the same Windows boot; no new selected fault events or
dumps were observed. The worker stopped and restored the baseline ICD.

## Cause and change

RADV's dedicated sparse queue unconditionally started the host submission
thread used by its Linux implementation. WDDM native timelines select immediate
submission, so that request violated the common runtime's mode invariant.
The WDDM sparse path already queues waits, mapping transactions and signals
through Windows; it does not need the Linux CPU-wait thread.

Dedicated WDDM sparse queues now retain immediate submission and own a separate
winsys context on the graphics node. They use its companion mapping fence and
queued waits/signals. Sharing the rendering context would allow a wait on the
sparse queue to block a later signal on the graphics queue. The independent
context is released at queue cleanup. Linux retains its existing threaded path.
RADV's sparse family maps to the graphics IP only for the WDDM submission path.

## After-change control

Candidate4D027149571DC000DA1E5006E6E393FCA6178DB32F1D9CB25D684E60849A5805
passes the former failing test, then all remaining release queue-bind cases:
17Pass/1NotSupported. The skipped multi_queue_wait_many_signal_many_other
requires a queue combination this device does not expose. This is preserved
for the same-Mesa Linux comparison, not treated as a passing test.
The run completed16:59:36Z. Loaded module witnesses select the candidate and
System32 loader. Health retains generation56484062069/epoch5,1000MHz/VID116,
67.125C and the same boot. No Windows, DWM or KMD restart was needed.

The complete port patch replays exactly onto Mesa05e6c962 with77 changed paths.
Source, build log, failed run and successful queue run are in controls.zip.
Original/published hashes are in manifest.json; LUID/UUID identifiers are
redacted where present. No firmware, desktop screenshot or memory dump is
published. The prior full run is intentionally retained with its failure.

All19078 release sparse cases have been launched again on the fixed candidate;
that ongoing run is not included in this result and is not an acceptance claim.
Full sparse completion, Linux parity and the other M12 requirements remain open.
