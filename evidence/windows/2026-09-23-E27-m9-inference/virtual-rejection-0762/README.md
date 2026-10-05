# Local virtual UMD rejection, KMD0762

2026-09-23. Host tests only; no lab access or deployment.
Base repository HEAD bed764d plus the captured uncommitted source. Microsoft contract:
ref/windows-driver-docs-ddi at7515063cea4c9e98db6a92986c5b4ddb0463fd16,
wdk-ddi-src/content/d3dkmddi/nc-d3dkmddi-dxgkddi_submitcommandvirtual.md, return values.

The actual pre-dispatch BC2S validation now returns STATUS_INVALID_PARAMETER for
malformed or unsupported UMD packets. The wrapper records the rejected fence before
releasing its active-submission count. The report DPC waits for pending hardware,
active submissions and pending completion publication, then updates the per-node
last-completed notion without sending DMA_COMPLETED for the rejected packet.
A validity bit distinguishes an uninitialized fence from an actual fence value zero.
Signed 32-bit deltas handle normal sequence wrap (outstanding distance below2^31).
A real late completion releases the predecessor even when the watchdog fault remains
sticky; the watchdog flag itself is not evidence that work is still executing.

Validation test links the actual parser and extracts pre-dispatch code from old/new
WddmSubmitUmdImpl. Old code fails the first truncation; new code passes valid controls,
every truncation, null/capacity mismatch, wrong node/magic/IB and unsupported multi-IB.
The harness ends after validation: it does not exercise ring dispatch or the full DDI.
The actual report DPC passes the six existing preemption controls and rejection cases:
busy GFX/SDMA, watchdog then late real fence, active DDI, completion arriving during
publication, first high-bit fence, no regression after later completion and32-bit wrap.
Watchdog regressions pass (old-source FAIL is the required negative control).
The existing BC2S contract suite passes host tests and kernel compilation.

Full KMD build/sign passes. Candidate package scratch/build/bc250kmd-0762/package-umd,
version0.7.62.1, SYS SHA256:
DB84714DF65FD7B587F5ABB0BCB0D4A280A46B316A4F42B094AA01218B24E0E5.
No runtime evidence of OS device-fault behavior or rejection ordering is claimed.
Valid ring-refused work still has a software-completion fallback; non-UMD malformed
Present/paging handling, system-page mapping and actual reset recovery remain open.

Raw test/build outputs and extracted/source files are preserved here. No redactions.
Build scripts retain workspace paths; before.c inputs are archived as wddm-before.c.
