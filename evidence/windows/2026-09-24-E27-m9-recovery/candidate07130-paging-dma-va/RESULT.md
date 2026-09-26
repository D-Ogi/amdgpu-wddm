# M420 - GPU-addressable OS paging DMA storage

Date2026-09-24, unit A, Windows,1000MHz/820mV.

## Change and result

PagingBufferSegmentId now selects the prepared aperture segment2 when it is
advertised, instead of system-memory0. Local Microsoft DXGK_QUERYSEGMENTOUT
allows aperture or0; OUT4 uses a one-based segment ID. Historical M66 rejected
local segment1, not aperture2. No-segment configuration still answers0.

Candidate0.7.130.1 SYS BB96304B7A6F4C84819871CD5EAA3C240BFD3EF3BC9C9D0C9CD8F4F145F71D64
starts full WDDM. The post-start log records955nonzero-VA BuildPagingBuffer
calls and1029zero-VA calls. Final count is26070nonzero,1029zero. Ring submission
logs now select private records at nonzero addresses such as0x410000, unlike
the historical VA0 submission convention. This makes OS paging DMA GPU-addressable
for the next IB integration; the driver still copies physical commands from
private data into SDMA, so GPU fetching this OS buffer is not yet established.

The eight detailed CPU-physical/VA-walk observations fell between the permanent
log head and retained tail before collection. The counter proves those probes
ran, but their match results are missing. Do not claim CPU/GPU mapping identity
or cache acceptance from this evidence. Preserve future bounded probe results
in summary state or collect them before wrap; no rerun was performed just to
recover these lost log lines.

## Validation

Actual KMD routing/segment/shadow/alias host suite:329199checks,0failures.
WDK build/signing passes. The installation's existing four VMID0 and two VMID2
startup controls pass all bytes and fresh fences. Current RADV8shader CPU hashes
and MLP argmax match. stories15M7/7 and TinyLlama23/23offload outputs match E14
in full after CR normalization; all native/worker exits0. The control script
is BOM-free ASCII and its first ErrorActionPreference assignment succeeds.

Final GFX2354/2354, SDMA22310/22310, zero timeout/refusal and noTDR.
One PnP disable/install/enable; no OS/DWM/ACrestart. Windows boot11:44:14,
DWM4448since13:58:18 through16:22:43; M412D3D/LLVM23.1.2 and M414RADV retained.
Full0 is consumed one-shot state, guard0, both SDMA optional gates0,
display gates1; final66.8C. Scanout captured privately, not visually accepted.

After native results were collected, the first final-script upload encountered
an SSH connection reset. No final script or log existed for that attempt.
Retrying only this read-only collection succeeded without reset or workload
repetition. A transient upload error is not evidence that the OS hung.

Raw logs redact only PCI instance/interface identifiers. validation.json
independently checks content/native exits and counters. Source snapshots and
hashes identify the installed build; later native-record development is separate.
Full M9 resource, mapping/cache/lifetime, native execution and performance gates
remain open. The remaining zero-VA calls must retain their supported path.
