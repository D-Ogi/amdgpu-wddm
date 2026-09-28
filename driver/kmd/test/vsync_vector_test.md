# Consumed VUPDATE dispatch control

`run_vsync_vector.ps1` extracts actual DcnVsyncInterrupt, synchronized vector
poll and Bc250DpcRoutine. The host models MMIO event latching and interrupt-lock
exclusion, plus WddmDcnVsync's ACK-token consumption. It does not model physical
scanout address retirement, actual MSI timing or the complete WDDM report path.

The late-event control first runs ISR polling with no event, then supplies an
already-consumed vector and latched event before the DPC. The synchronized poll
must ACK once and expose one report token. Removing the vector dispatch via
-DropVectorDispatch -ExpectFailure fails that assertion. ISR-first must remain
idempotent. Other controls cover disabled interrupts, synchronization/read/write
failures and no-vector invocation. The IH suite independently executes actual
Consume/IhTakeVsync to verify client/source filtering and one-shot latch delivery.

The new diagnostic line reports DPC polls, successful DPC ACKs and synchronization
failures separately. Existing DCN poll counters and entry timestamp now include
both ISR and synchronized DPC polls; InterruptCount/InterruptLastTime still denote
actual ISR entry. Do not equate total no-event polls with ISR calls after this change.
A synchronization failure is recorded, never manufactured into a successful ACK.

Candidate mechanism comes from M710's drained ring and2200 VUPDATE vectors versus
2199 ACKs. Passing this host model is not proof of DWM047 causality or G0 stability.
