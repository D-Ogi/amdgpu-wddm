# M563: cross-process content passes; hosted resource release rejects teardown

Unit A, project base 1d7ec58. The new control creates two independent processes
and native D3D devices, using the system hardware adapter without app-local
D3D11/DXGI DLLs. The parent creates a shared RGBA8 texture and the child opens
it. GPU EVENT-query completion precedes each CPU-event ownership transfer.
Each direction writes a unique iteration value and direction byte, then the
other process copies to staging and checks every pixel. Every100 exchanges
close/recreate/reopen the texture with a new extent. This is serialized sharing,
not a cross-device GPU-fence protocol or a presentation-copy measurement.

## Controls

| Run | Stack and oracle | Result |
|---|---|---|
|064|CPU UMD8279AC7F, original control B0F8EEF1,10 exchanges|20480 pixels per direction pass|
|065|Same stack/control,1000 exchanges and10 generations|2702400 pixels per direction pass|
|066|Hosted UMD B69AA635/ICD3508416F, original control,10 exchanges|Pixels pass, but teardown logs object_refs=2, hosted device loss and VK_ERROR_DEVICE_LOST. The printed PASS is not accepted|
|067|Same hosted stack, strengthened control64B47285|Exit1: GetDeviceRemovedReason after final owner release returns887a0005|
|068|CPU baseline, identical strengthened control,1000 exchanges and10 generations|2702400 pixels per direction pass; both processes exit normally, DWM84 unchanged|

The original control checked rendering but not the device status after its
final release. The strengthened control checks GetDeviceRemovedReason after
each owner/imported-resource close. Both executable identities are retained;
the initial weaker result is not silently replaced. Baseline restoration and
DWM84 are recorded for completed hosted controls066/067.

## Diagnosis and candidate

The existing hosted release helper waits for the context fence and resets
ctx->batch_states, then requires sole resource/object references. Source review
finds another reference owner: find_completed_batch_state moves completed
states into screen->active_batch_states, where objects remain referenced until
submit_queue reclaims that list. A final release need not cause another submit.
The runtime log's extra object reference is consistent with this missing drain;
the fixed-versus-unfixed lab comparison is still required to confirm causality.

completed-batches.patch factors the existing locked reclamation function and
calls it after the hosted helper's context reset. Ownership checks and GPU waits
remain intact. Exact three-file UTF-8/LF replay passes, the UMD builds, and all
eight existing fast gates pass. Candidate F8BB0D3E is retained in the manifest.
These gates do not by themselves test the new runtime lifetime behavior.

## Pending validation and lab coordination

Candidate attempt069 did not launch the control: the registered ICD could not
be overwritten while another process held it. Its finally reported the same
sharing violation while attempting restoration. Immediate independent hashes
confirmed baseline ICD9C40083C/UMD8279AC7F and DWM84. Attempt070 then stopped at
its initial baseline check, before mutations, while another coordinated D3D12
probe used ICD4D027149. Both setup failures are retained separately from067's
actual device-loss failure. They say nothing about candidate rendering.

The candidate is built but NOT lab-validated or promoted. Continue with the
short corrected control, then1000 exchanges and a native Present regression
after the other lab window ends. Capture KMD object counters before/after, and
distinguish global background changes from leaks. Full M13.2 residency,
process-exit/lifetime stress and G0 acceptance remain open.

Public logs are raw captured output. No images, memory dumps, account names or
credentials are included. Build outputs stay outside the repository.
