# Static explanation of write-only Map after removal

No GPU trial or debugger was run. The saved d3d11.dll SHA256697BE4DB matches a fresh
read-only hash of System32/d3d11.dll on unit A. PE RSDS GUID matches PDB info GUID;
PE age1 matches DBI age1. PDB info-stream age is2, recorded as a distinct value,
not silently treated as1. A local MSF/CodeView reader and Capstone decode the
public-symbol locations and PE bytes; function range is bounded by unwind metadata.

CContext::DDI_ResourceMap_DR at RVA0xA9CA0 calls CompleteContextRemoval. Under its
error-state conditions, MapType1 or3 goes to the callback with0x88760870. Type2,
4 and5 go to GetBackingMemorySubresource; the returned16-byte mapped-subresource
structure is copied to the caller's output. The helper allocates backing memory
if its stored pointer is null. These are static code paths, not an execution trace
of errors001, and private field meanings beyond these instructions are not asserted.

Microsoft's Map API documentation explicitly promises DEVICE_REMOVED for CPU-read
access. It does not state that every write-only Map must return a failed HRESULT:
https://learn.microsoft.com/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-map
Retrieved2026-09-28. Local equivalent: ref/sdk-api-docs/sdk-api-src/content/d3d11/
nf-d3d11-id3d11devicecontext-map.md:100.

Together with M753's injected OOM, DDI removal callback, null-on-failure shell code,
S_OK API result and sticky device removal, this supports runtime-owned fallback
memory as the explanation. It does not prove which Map wrapper branch executed.
The original test's requirement of failed HRESULT for WRITE_DISCARD was too strong.
Errors002 will retain sticky removal and add READ on a pre-created staging resource:
healthy CPU control must map successfully; the removed GPU device must report removal.
No old result is relabelled as passing. First-failure output-pointer ownership has
not been traced live. The next test does not read write-only mapped memory.
