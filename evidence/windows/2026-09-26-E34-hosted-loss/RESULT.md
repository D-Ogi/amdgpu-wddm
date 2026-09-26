# Hosted device-loss propagation

Unit A, 2026-09-26; base f674d8b plus loss patches. Candidate015 manifest binds
ICD9064C398, UMD4764DAD9 and both exact controls. Mesa MIT. Private ABI5.

Removal/reset/hung HRESULTs now retain STATUS_DEVICE_REMOVED identity. Sticky
host status reaches Vulkan device-status, fence polling, post-wake and submit
paths. First detection reports D3DDDIERR_DEVICEREMOVED through SetErrorCb.
Render progress, Present and UMD paging observations cannot treat UINT64_MAX as
completion. Progress publication carries its CPU mapping; context/sync teardown
clears it. CPU-signal failure no longer destroys a still-owned sync object.

run027: positive120-Present control exits0, final76800 pixels exact, no removal.
run028: diagnostic refuses the third SubmitCommand before sending GPU work.
Application frame2 Present and GetDeviceRemovedReason report887a0005; Vulkan
reports VK_ERROR_DEVICE_LOST and bridge status is C00002B6. run029 substitutes
a local UINT64_MAX observation after the third submit, without modifying mapped
fence memory. The application detects loss at frame2 with887a0005. Negative
controls exit0 only after detecting loss; they are not successful render runs.

All runs restore CPU UMD8279AC7F and registered ICD9C40083C, DWM1052 unchanged.
No hardware TDR/reset, reboot or clock change. First UMD compile failed on a
missing explicit pointer conversion; corrected build and eight scoped gates
pass. Six changed files replay byte-for-byte with UTF-8/LF and autocrlf=false.

## Contract and limits

Microsoft [Handling Errors](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/handling-errors)
documents SetErrorCb and D3DDDIERR_DEVICEREMOVED. Local reference110f60ea and
WDK10.0.26100 callback declarations were used.
[Context Monitoring](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/context-monitoring)
describes read-only64-bit CPU fence mappings and asynchronous CPU waits.
It does not establish that every TDR writes UINT64_MAX. We reserve that value
as invalid in this private hosted contract. Injection validates rejection and
propagation, not real TDR recovery. Host status is sticky callback state and
fence observations, not kernel GetDeviceState on a borrowed device.

The pre-existing standalone page-fault branch still lacks GetDeviceState.
BD-035/036 failure-time leaks remain separate reviews; process exit is not a
leak audit. BD-041 CPU-signal ownership correction is source/build checked,
without failed CPU-signal injection. Full-stack clean replay and G0 remain open.
