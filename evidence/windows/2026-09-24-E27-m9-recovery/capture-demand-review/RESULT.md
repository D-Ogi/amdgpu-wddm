# M381 - Construction lifetime defines capture demand

Source review only, 2026-09-24; no lab operation. Exact inputs and SHA256 are
preserved in sources.json and adjacent source snapshots.

WddmBuildCapturedVirtualTransfer attaches a plan on first construction and
retains it across MultipassOffset continuations. It detaches the plan when the
last batch has been accepted by WddmPublishPagingRecordCore and construction
progress reaches the end. It does not wait for a hardware fence. The commands
contain copied data, not references to the capture allocation. Thus queued GPU
buffer count is not itself the number of live capture plans. Resource sizing
must cover overlapping unfinished construction operations, including abandoned
continuations until their context is drained. PagingBuildLock serializes calls;
it does not by itself prevent interleaved operations across successive calls.

The reserved size comes from GfxPagingCaptureStorageSize(0,0,1 GiB). The exact
size helper includes endpoint offsets; valid endpoints must also fit the
advertised paging VA range. A hypothetical unaligned 1 GiB transfer cannot be
used alone to justify increasing the arena without checking that range. Neither
the selected transfer structure description nor the paging-buffer callback
contract inspected here supplies a numeric maximum for overlapping unfinished
transfers. The example's continuation loop is not a universal concurrency proof.

Microsoft QuerySegmentOut describes private storage per DMA buffer. It does not
make that storage a stable address across all buffers of a multipass operation.
Moving a retained capture pointer into that storage therefore needs an explicit
owner/lifetime design. Increasing PagingBufferPrivateDataSize alone does not
close the retained-plan resource guarantee.

Next implementation decision: derive a bound on unfinished construction from
paging-process scratch mapping ownership, or change capture ownership so its
resources are admitted with the owning operation/allocation before BuildPagingBuffer.
Keep physical identities stable when later logical PTE changes are accepted.
Do not size by arbitrary queued-buffer count, convert resource exhaustion into
unbounded DMA retry, or discard capture state between buffers. Current arena
fallback remains open; this review makes no claim of full resource acceptance.
