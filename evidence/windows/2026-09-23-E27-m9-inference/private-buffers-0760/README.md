# Local0760 per-DMA-buffer command ownership

No lab deployment or GPU experiment. Source HEAD bed764d plus uncommitted E26/E27.
Signed candidate SYS C6C74EBB01E141C0BD5A14F7B758C06C09B2CCC5D0595E26F341D56C8D339DAC.

Microsoft contract: ref/ddi-display/d3dkmddi.md:26930 says PagingBufferPrivateDataSize
allocates private nonpaged storage per paging buffer. Physical private start/end fields
and virtual private pointer/size identify that buffer's data during submission.
Original docs commit7515063cea4c9e98db6a92986c5b4ddb0463fd16, WDK26100 declarations.

BuildPagingBuffer now writes independent per-buffer records, copies commands to pDmaBuffer,
advances private and DMA pointers/sizes, and invalidates stale record tails. Submit validates
an exact range, including all its records, before copying payloads to SDMA. Global shadow
allocation and global GPU-address/high-water lookup removed. No requested-size clamping.
Physical command and private offsets checked against their distinct capacities. Virtual
paging records are accepted only from non-UMD contexts with zero UMD private-data length.

Host tests build B after A then consume A correctly; test physical nonzero private/command
offsets, partial ranges, zero GPU base, wrong identities, gap/overlap/truncation/overflow.
Malformed late records cause zero visitor calls. /W4 /WX host and kernel compile pass.
Stream1GiB simulation, DmaSize oldFAIL/newPASS and6preemption regressions also pass.
KMD builds/signs. This establishes source/host properties only; actual OS delivery of private
paging data still needs lab validation. System mapping and false completion policies remain
open. Existing engine-state teardown rundown also requires further review.
