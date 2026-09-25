# Win32 present completion and WDDM wait deadlines

PROVENANCE: Mesa MIT; Vulkan CTS Apache-2.0; Diligent Apache-2.0; DXVK MIT.

KMD151 and Mesa05e6c962, DXVK3.1.1. Candidate763628F6 adds synchronized
presentation IDs and both Win32 wait callbacks. Completion is published only
after the existing GDI/DWM presentation boundary; the DXGI path flushes DWM
for nonzero present IDs before publishing completion. Waits first consume the
WSI timeline semaphore, then wait for the presentation ID using one deadline.
Retirement wakes waiters. No capabilities are disabled to avoid test failures.

The first CTS run gives3Pass/3NotSupported, then watchdog timeout in future_frame.
Root cause in vk_wddm2_monitored_fence_wait_many: finite deadlines selected a NULL
hAsyncEvent, which makes D3DKMT wait indefinitely. Its old event loop also had
an inverted deadline condition and the synchronous path left result uninitialized.
CandidateA6D11B64 always uses asynchronous event notification, retries capped or
early timeouts against the absolute deadline, and closes the event. Win32 now
also fills presentId2Supported/presentWait2Supported surface query structures.

Sources consulted: local Vulkan-Docs chapters/VK_KHR_surface/wsi.adoc,
WaitForPresentKHR/WaitForPresent2KHR; local ref/ddi-display/d3dkmthk.md,
D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU.hAsyncEvent. DwmFlush's exact Win32
contract was missing locally and checked in Microsoft's reference:
https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/nf-dwmapi-dwmflush .
The synchronous composed path is a correctness baseline; asynchronous presentation
and its latency/performance remain work to measure, not an optimization claim.

CTS release vulkan-cts-1.4.6.2 f6a29701, executableAE7BEFDD, candidateA6D11B64:
- All15 wsi.win32.present_id_wait cases PASS, including future/no-frame timeout,
  two swapchains and wait2. No skips. QPA identities/results match the exact list.
- All7 synchronization.timeline_semaphore.wait cases PASS: host/device signal,
  all/one/poll and host-wait-before-signal. No skips. QPA results verified.
- Initial launcher rejected older35CBBC05 under system-icd/tools before any test.
  The valid release binary is in C:/BC250/m12/cts-release-tools.

D3D11 run005 on763628F6 completes660frames in32766ms. Showcase001 on that same
candidate completes3960frames in177892ms, exit0. The owner confirms the live scene
is visible on the physical monitor. This is a separate showcase, not a performance
baseline. Run006 on finalA6D11B64 also completes660frames with all five expected
modules. Both660-frame captures are1080x720 and byte-identical SHA2567489F181...
The captured scene has asteroids, skybox and Diligent D3D11 title. CPU/GPU CSVs are
complete, positive and their tick conversions verified. GPU interval is not
active-ALU time. These captures do not establish Linux image/performance parity.
Full PPMs stay in scratch/m12/<run>-collected; previews and hashes are included.

Generation560773206/epoch5 stays unchanged; no reboot or power cycle. Each run
restores baseline system ICD9C40083C. Candidate remains experimental. Full CTS,
Linux comparisons, OpenGL/OpenCL and D3D9/10/12 applications remain open.
