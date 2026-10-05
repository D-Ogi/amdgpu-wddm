# Ordered PTE update packet groundwork

New bc250_sdma_paging_update_ptes composes the existing AMD WRITE_LINEAR PTE
packet with the existing noninterrupting fence/memory-poll pipeline barrier.
A later FLUSH_TLB must follow this barrier; the helper does not itself invalidate
TLBs, translate DXGK_PTE, select a process, allocate memory or touch live tables.
It is NOT yet called by UPDATE_PAGE_TABLE. Runtime behavior remains local0769.

Complete write-plus-barrier capacity is reserved before any output. Inputs reject
absent data/scratch, zero marker, misalignment, destination wrap and invalid count.
Caller must retain the destination table and separate scratch allocation, avoid
aliasing them, and supply a marker distinct from preceding commands in the buffer.
Existing paging scratch slot5 and command-position markers are the integration
mechanism; no new scratch reuse policy is introduced here.

run_paging.ps1 -KmdRouting:6182checks,0failures. Tests include every count1..512,
all PTE values, packet addresses/count, noninterrupting marker fence then matching
memory poll, exact aligned capacity, one DWORD short/no output, tail canaries and
invalid inputs. Prior copy/fill/VMID flush/actual KMD route controls also pass.
These are host packet assertions, not hardware memory-ordering proof.

A512-entry update uses1038DWORDs, aligned1040, already larger than the live SDMA
max_dw1024 before its6DWORD interrupting outer fence. This mandates multipass.
With no prior commands, existing budget gives1018DWORDs; packet alignment reduces
usable reservation to1008, permitting497entries (14+2*497). Remaining15entries
must go in a later paging buffer. Multipass must track entry progress separately
from command bytes and translate Repeat correctly, without partial publication on
bad input. This arithmetic is source-derived, not a runtime measurement.

Full WDK build/sign PASS, scratch/build/ordered-pte-dev, retained0769version:
DEVELOPMENT ONLY, DO NOT DEPLOY. Official0769 candidate unchanged.
Dev SYS SHA256 CE8581F4B31334BEA5F0E9D9E2C14F8640CCC0B30A802493B0257B7998D0A4DD.
No hardware access, deployment, reset or agents.

Integration gates: CPU_VIRTUAL paging-process initialization remains immediate
and may have NULL pDmaBuffer; GPU_PHYSICAL updates need ordered emission with
multipass and translated VRAM destination. Initialization before stage8 needs an
explicit engine-state policy. Never silently drop required updates on NOT_READY,
and do not perform immediate CPU writes merely because an active GPU is busy.
Bootstrap/error policy and actual OS/GPU page lifetime are still open.
