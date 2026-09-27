# Known-content window for engine Present validation

Status: prepared, not run on the lab. This is a content and routing control for
G0, not a replacement for the desktop acceptance requirements.

## Hypothesis

With exact KMD164 and CDD-DWM interop enabled, the WSI KMT Present route can
update a known-colour window through the real DxgkDdiPresent/BGP1 path and the
GPU-composed desktop displays the expected client pixels.

## Program and build

Build `build-wsi-colour-control.ps1` with the local MSVC/SDK and Vulkan-Headers.
The compiler uses /W4 /WX. Only `--help` and invalid arguments are safe host
controls; a valid ICD argument creates a window and is for the lab session.

`wsi-colour-control.exe ABSOLUTE_ICD_DLL` loads that ICD directly through
vk_icdGetInstanceProcAddr, without the Vulkan loader, manifest or registry.
Require its printed loaded path and independently sampled module SHA256 to match
the chosen artifact. This selects only the probe's ICD; DWM continues to use its
separately verified hosted ICD. Confirm direct WSI dispatch at runtime before
interpreting results; a build alone does not prove that interface works.

The program accepts exactly one AMD 1002:13fe device, a graphics/present queue,
a 640x480 UNORM swapchain, opaque alpha and FIFO. It clears swapchain images on
the GPU: 20 red frames, 20 green, 20 blue, about 100 ms apart. It logs every
Present return and render fence separately, then prints physical client bounds
and holds blue for 10 seconds. No image mapping or CPU pixel upload is performed
by the program. The ICD's diagnostic CPU WSI remains a separate positive control.

Acquisition and render fence waits have five-second deadlines. Render-finished
semaphores are per swapchain image and reused after reacquisition; command buffer
and acquire semaphore reuse follow the render fence. These CPU render waits are
intentional for a content control, not a performance or overlap benchmark.
A render fence does not witness the later Present copy. ICD calls and teardown
can still block: the interactive lab runner must enforce an external deadline.

## Required bounded lab procedure

1. Coordinate the lab slot, check STOP/thermal limits and verify exact KMD164,
   baseline UMD/registered ICD hashes, and candidate 0CD4A98D or a reviewed
   successor. Do not reuse old E46 scripts that assert registered ICD93B1D1FD.
2. Run the same executable and candidate with `BC250_WSI_CPU_PRESENT=1` first.
   Capture the blue client at `capture_ready`; verify the exact client interior
   is blue in both GDI capture and independently sampled primary. A failed
   positive control invalidates the image oracle.
3. In a bounded GPU DWM trial with verified startup and rollback, run the probe
   with CPU fallback absent, `BC250_WSI_PRESENT_LOG` set, and submit tracing on.
   Start DxgKrnl/process ETW before launch. Keep a 45-second process watchdog
   within the existing three-minute desktop trial and its independent rollback.
4. Poll the original process identity and log. On `capture_ready`, capture both
   images while it holds blue. Record client bounds, frame/result, timestamp and
   hashes; exclude decorations/overlay overlap explicitly, not arbitrary failed
   pixels. Do not infer a visual pass from Present success or window existence.
5. Correlate the probe's Present events with Blit_Info, KMD BGP1 source/destination
   metadata and completed GPU fences. Distinguish its jobs from DWM startup CDD
   copies. Admission refusal before KMD is a failed routing control, not a GPU
   packet failure. Preserve source and destination geometry/residency evidence.
6. Remove only this run's tasks, stop its ETW session, verify baseline restoration,
   STOP/health/temperature and collect durable results before releasing the slot.

The independent packet-content control M673 already passed through BC2S. This
new probe specifically tests the real Windows Present route; neither test alone
proves whole-desktop correctness or excludes every steady-state CPU frame copy.
No lab run, G0 pass or promotion is claimed by this preparation.
