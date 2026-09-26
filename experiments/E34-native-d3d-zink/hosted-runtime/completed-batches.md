# Reclaim completed global batches before runtime resource release

PROVENANCE: Mesa (MIT), existing completed-batch reclamation in zink_batch.c.

The hosted release helper waits for its context and resets ctx->batch_states,
then requires sole resource/object ownership before returning the runtime
allocation. find_completed_batch_state can already have moved a completed
context batch to screen->active_batch_states. That list keeps its object
references until submit_queue next reclaims it. A final release with no further
submission can therefore see object_refs=2 and report false device loss.

Factor the existing locked screen-list reclamation into
zink_batch_reclaim_completed and call it both from submit_queue and the hosted
release helper after its fence wait/context reset. Retain both ownership checks.
Do not decrement an unexplained reference or bypass GPU completion. The only
runtime insertion into the screen active list is the completed-state path;
its context-specific reset has already occurred before insertion. Lock order
remains active_batch_states_lock then free_batch_states_lock.

Apply completed-batches.patch after audit-buckets.patch. The manifest binds
UTF-8/LF before/after hashes for the three files. Build gates, patch replay and
cross-process negative/positive controls are required before promotion.

Control066 initially reported PASS pixels but its teardown log reported device
loss. The strengthened control checks GetDeviceRemovedReason immediately after
each resource close. Control067 on the unchanged UMD rejects the final owner
release with887a0005. These failures are retained, not counted as acceptance.
