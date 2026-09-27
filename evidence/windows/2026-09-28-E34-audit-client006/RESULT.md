# M703 - runtime import/Present identity control on a small GPU client

Exact KMD166, new UMD00b2e15e/0FB2A9F2 and hosted ICD C0CE, runner3d515d1.
The UMD full615-step build linked successfully. Only the runtime identity/fence
witnesses changed relative to the005 UMD; texture workload is identical.

All three76800-pixel readbacks pass (magenta,green,yellow),18 texture draws and
8 markers. The deliberate12 CPU-copy frames account for3686400 application
bytes and remain detected. GPU interval has32 descriptor spans/1792 bytes.

Strict identity analysis joins29 completed Presents to two successful imported
runtime resources26/30, matching allocation handles, device owner, dimensions,
format, context, waited fences and monotonically signalled Present values. No
CPU map of either imported resource appears. All runtime events reconcile with
checkpoint runtime_events.29 is independently required by the workload source:
one warmup Present,16 GPU Presents and12 CPU-copy Presents, all before marker8.
This checks a missing whole final Present, which ordinal chains alone cannot
exclude. Queue callback witnesses do not establish hardware completion on their
own and this small client is not DWM/G0 acceptance.

The existing strict map parser initially rejected the new runtime_events field.
It now validates that field against the observed separate runtime sequence,
rejects mixed schemas/lost events, and retains legacy compatibility.14 store
controls and the old005 positive/negative controls pass. Identity tests include
17 malformed-stream mutations, pointer reuse, padded MSVC pointers and an
independent total-count negative. The preservation of a trace tail still requires
an independent count or other end witness for workloads without a known count.

Client/watchdog exit0, tasks removed, baseline8279/CF39 restored. Closure
23:33:11Z September27: exact166,health15,1000MHz/VID116,67.25C; CPU DWM9552,
OS boot and health generation/epoch unchanged. Closed receipt23:34:12Z.
One PowerShell observer timed out; an independent cmd heartbeat and terminal
receipts confirmed completion. No restart or duplicate trial was issued.

Raw logs, module identities and archive remain in scratch/g0-hosted/audit-client006-ops.
Export contains workload stdout and reduced numeric results/hashes only.
No permanent deployment or accelerated-desktop acceptance follows from this test.
