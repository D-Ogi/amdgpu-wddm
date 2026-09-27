# Production Present packet GPU content control (M673)

Native011, unit A, 2026-09-27. ExecutableD8CCA711 built /W4 /WX from90f36f6;
exact KMD164/9B9B99D3, baseline8279AC7F/CF3948D6. Six packet/planner inputs match
exact164 source after newline normalization (raw LF/CRLF hashes are retained).
No driver, registry, renderer, clock or OS change.

--run-present-list calls the production Bc250EmitGfxPresentBltList, including
ACQUIRE_MEM at every pass and full aligned NOP padding. Five cases pass with zero
pixel mismatches and30 before/after residency checks. The small cases cover all four
GTT/VRAM combinations; source/destination pitches differ. Minimum16-DWORD buffers
force10 submissions for10 copy packets, larger small buffers use3 submissions.
The1920x1200 case uses2400 row packets over2 complete IBs. A vertical stripe, untouched
padding and other destination bytes retain their sentinel. Independent direct-memory
DMA readback verifies the destination after the monitored fence, not a CPU replay of
the Present packet builder. All9371648 bytes of the large readback are compared.
Final fence41; all source/destination/readback allocations stay resident as queried.

Original process5972/start13:14:35.4538336Z ends13:14:43Z exit0. Task removed and
closure13:15:40Z confirms CPU9852/boot retained,health15,1000MHz/66.375C. Baseline
UMD/ICD hashes unchanged. Full raw logs remain private, hashes retained; selected
COPY_* result/residency lines are published without modification.

Scope: BC2S transport on a test-owned UMD context. This proves packet execution and
content for the exact Present wrapper, but does not exercise D3DKMTPresent, the BGP1
private-record consumer, CDD residency or its allocation lifetimes. Existing host
private-record and validation controls remain separate. DWM032 execution evidence
M672 and this content control do not yet constitute a full BGP1 integration oracle
or G0 acceptance. Unsupported integration paths are not enabled by this experiment.
