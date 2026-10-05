# Local paging stream candidate0759

Source HEAD bed764d plus uncommitted E26/E27. Unit A was not touched.
KMD0.7.59.1 built and test-signed, not installed. SYS SHA256:
F1F31CF89F88F77A8BD44ACCDD0AF8237D0E7E70C39E078AE62020DB3E136016.

The actual portable builder is linked into the KMD and host test. It splits at source
and destination4KiB boundaries, resolves each slice independently, emits through the
existing AMD packet helpers, and returns cumulative data-byte progress distinct from
DMA command-byte offsets. BuildPagingBuffer publishes partial packets/pointer/remaining
space before requesting another DMA buffer. Data-byte counters widened to64bits.

The accumulated command budget is bounded by live SDMA ring max_dw after reserving
fence and alignment. The OS64KiB buffer is not a promise that the live ring can accept
64KiB at once; existing SDMA max_dw is1024, with an8KiB physical ring.

Host evidence:
- Fragmented17-page source/destination permutations, copy with differing unaligned
  offsets, and fills: every destination byte matches an independent byte-level oracle,
  including untouched prefix/suffix. Tiny7/14-dword and127-dword budgets force resumes.
- Buffer canaries remain unchanged. Insufficient space publishes no progress.
- Translation failure after an earlier slice publishes no part of the current batch;
  scratch words may have been written, but written count/next offset stay at entry.
- Overflow/invalid progress/unaligned fill rejected. Ring-budget boundary cases pass.
- Synthetic1GiB stream verifies262144page commands cover every source/destination
  address exactly once across partial buffers. No1GiB GPU transfer occurred.
- Existing real AMD packet tests22/22pass, extracted DmaSize oldFAIL/newPASS,
  six preemption scenarios pass. Full KMD builds/signs; portable code /W4 /WX and kernel flags pass.

Limits: this does NOT complete paging. System pages still lack MC mapping. The single
mutable shadow and unsupported/error completion policies remain unresolved. Those must
be fixed before enabling this as an acceptance path. No runtime performance claim.
Source snapshots have comment-only clarifications after the recorded build.
