# Hosted GPU fence ordering and native Present control

Unit A, 2026-09-26. Base bc250-win e21a08d, Mesa upstream05e6c962 plus
previous project changes and the incremental present patches. Mesa MIT;
callback declarations from WDK/SDK10.0.26100, ref/ddi-display/d3dumddi.md.
manifest.json binds candidate014 ICD8341308E, UMDD0BB42E7 and the control.
No KMD update, OS reboot or DWM restart.

## Implemented ordering

Private ABI4 publishes each queue's successful GPU progress signal to its
D3D device. Publication follows accepted GPU signal; retirement tracking is
updated first so publication failure cannot lose the fence needed for cleanup.
Context and sync destruction clear their published witnesses.

For hosted Present, Zink flushes and UMD inserts GPU waits for published render
fences into the separate runtime Present context. PresentCb is followed by a
UMD-owned monitored-fence signal. Subsequent render submissions wait for that
Present value once per context/value. This orders the next render after the
previous Present without CPU fence_finish in the hosted Present path. A failed
submission/signal prevents continued use. CPU waits remain in allocation and
teardown paths, including a10-second async wait for Present during destruction.

Runtime-imported images join Zink's batch-end release tracking, transition to
GENERAL/FOREIGN and skip external semaphore export. Later Vulkan use reacquires
ownership through the existing image-barrier path. CPU baseline Present is
unchanged. These are private integration changes, not an upstream API claim.

## Results and unsuccessful controls

- run020: first ABI4/barrier candidate passes the unchanged three4096-pixel
  shared-surface checks, including sibling-device destruction.
- run021/022:120 Presents pass, end-only staging readback has0/76800 mismatches.
  run022 screen captures contain46800 exact red, blue and green pixels each in
  the fixed interior ROI. This version orders render before Present only.
- run023: attempted Present directly on the RADV GFX context; process ended
  before three captures. The runner threw before preserving process streams.
  Its incomplete-capture error does not establish the underlying exit code.
- run024: never entered the test. A runner generator replaced023 inside the ICD
  SHA256 while renaming the run. The hash gate rejected it. Later generation
  replaces named filename/task prefixes only.
- run025: direct GFX-context Present returns887a0005 on frame0, process exit8,
  although the GPU wait succeeds. No claim that this is a hardware reset; DWM
  PID1052 and baseline restoration remain intact. This approach is not retained.
- run026: final bidirectional candidate exits0 after120 native Presents.
  Logs witness render->Present waits, Present signals and Present->render waits
  with values1-3, all HRESULT0. Final staging readback has0/76800 mismatches.
  Each of three independent composed-screen captures has46800 exact expected
  pixels in ROI [180,200,440,380]. Capture times are recorded in the raw log.
  No device removal or teardown failure; DWM PID1052 unchanged. CPU UMD8279AC7F,
  registered ICD9C40083C and prior app UMD restored and hash checked.

Both DLL builds and all eight existing scoped gates pass. The12 changed source
files were replayed in an isolated directory using git -c core.autocrlf=false
apply, with LF-normalized inputs, and match the final source byte hashes.
The manifest preserves original and LF input hashes. This verifies this
incremental patch, not a full clean build of all earlier project patches.

Full-screen PNGs stay outside Git because they include unrelated desktop
content. capture-hashes.json identifies the unchanged local screen026 images
under scratch/g0-hosted. screen026-analysis.json reports only the fixed test
ROI's pixel counts. No image was modified to create those measurements.

## Limits and next test

This320x240 DISCARD swapchain is a GPU-rendered client composed by CPU DWM.
The existing KMD blit path may copy client surfaces on the CPU; there is no
no-copy or GPU-DWM claim. End-only staging readback and three diagnostic screen
captures are explicit CPU readbacks. Next exercise flip-model sharing and
resource rotation, then bounded DWM switching with rollback. G0 still requires
correct primary output, DWM-attributed GPU work and instrumented exclusion of
steady full-frame CPU copies, including persistent mapped writes.

Multiqueue timing, error injection,1000-iteration cross-process sharing, full
resource lifetime and full clean source replay are not certified by these tests.
