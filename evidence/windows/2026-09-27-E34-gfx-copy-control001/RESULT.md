# M608: native GFX copy control001 stopped before submission

Candidate231CD67CB0AC9178821916318FF6E277BC2CA8EAEF93E7A4EC40A03F40DA08FA,
built from7fb6115, runner73ede2d, diagnostic KMD153 SYS0C0DA4E13FF110606265274B063D3B6034459A294FE2A552FD5806E0971B05EB.
The first GTT-to-GTT case allocated, mapped, made resident and initialized its
buffers, then stopped at the first residency assertion: requested_heap=2,
observed=1, expected=2. QueryAllocationResidency itself succeeded. Exit1,
final_fence=0; no GFX copy SubmitCommand call was reached. This is not a hardware
copy failure or a positive execution result for the new emitter.

All logged FreeGpuVirtualAddress, DestroyAllocation2, synchronization/context,
paging queue, device and adapter teardown calls succeeded. Independent closure
at05:35:32Z found no native control process or running worker, removed the task,
and measured KMD153 flags15, epoch5,1000MHz/VID116,66.25C. DWM4596 and boot time
match the preflight. These checks do not establish absence of a kernel leak.

The test equated GTT with RESIDENTINSHAREDMEMORY. That is not established by the
local enum description. In the deployed KMD source c3499f1b, wddm.c BC2A creation
restricts both supported segment masks to aperture segment2 for GTT, and segment1
for VRAM; QuerySegment4 marks segment2 Aperture/CacheCoherent/CpuVisible. This
source contract is stronger than a preferred-heap request but does not itself
measure the allocation's physical placement. Preserve the failed gate while
researching the API semantics and a suitable placement witness. No new run or
capability promotion follows automatically from this result.

A private inspection script accidentally serialized Get-Content's decorated
string with ConvertTo-Json, producing excessive provider metadata. Closure uses
IO.File.ReadAllText for the error string; that output is bounded. The lab answered
an independent SSH check during inspection. Raw driver logs remain in the private
collection; their SHA256 values are retained in manifest.json. Included receipts
and stdout are unmodified; no private identifiers required redaction. G0 remains
open, including engine Present integration and desktop acceptance.
