# Hosted GPU map-audit positive control

Status: source and /W4 /WX host build prepared; no lab execution yet.

Hypothesis: on the hosted Zink UMD, sequenced map auditing distinguishes ordinary
GPU clear/flip work from a deliberately CPU-filled image copied to the same
swapchain. The deliberate path must be detected before interpreting absence of
that path in desktop evidence.

Build build-runtime-audit-control.ps1 with the workspace Root. The build invokes
only --help; valid arguments create a visible window and belong on the lab's
interactive desktop. The program selects exactly one1002:13fe adapter, derives
its LUID, and requests FL10_0/BGRA8/320x240/two-buffer FLIP_SEQUENTIAL. The
runner must route this process to the exact candidate UMD and its hosted ICD;
the program's request alone is not proof of the loaded module identities.

Command: runtime-audit-control.exe ABSOLUTE_MARKER_PATH ABSOLUTE_STDERR_LOG_PATH.
Use a fresh run directory. Enable the existing hosted-device configuration in
the runner. The program enables BC250_HOST_AUDIT and BC250_UPLOAD_AUDIT itself,
and sets BC250_AUDIT_MARKER before device creation.

Redirect stderr directly to the named, readable file before process startup.
Do not reuse the old run121 ReadToEndAsync capture followed by an end-only write:
the program must observe checkpoints while it is alive. It opens the shared log,
publishes each numbered marker by same-directory replacement, Flushes and pumps
messages until the matching complete checkpoint line appears, with a5-second
per-marker deadline. An external45-second process watchdog is still required
because Map/Present/teardown can block inside runtime or driver calls.

One black warmup Present precedes marker1. The measured intervals are:

| Markers | Work | Expected map interpretation |
|---|---|---|
| 1-2 |16 GPU clears and Presents | No successful image-write maps for these GPU-only updates |
| 3-4 | Fresh green GPU clear and isolated staging readback | One deliberate image-read map;76800 pixels exact |
| 5-6 |12 CPU-filled staging images, GPU copies into backbuffer, Presents |12 successful full320x240 image-write maps; application memcpy count3686400 bytes |
| 7-8 | Copy the last uploaded cyan image into backbuffer and read back | One deliberate image-read map;76800 pixels exact |

CPU upload uses mapped RowPitch and writes exactly1280 bytes per row. Alternating
magenta/cyan upload frames finish cyan. The app prints its exact memcpy amount;
the map audit records requested access and lifetimes, not those individual CPU
stores. The two independent totals must be compared, not equated by definition.
Readback intervals perform fresh known-content work; they do not certify every
previously displayed frame. External primary/GDI capture may additionally check
the visible client, under a separately recorded capture bracket.

Before launch, coordinate the slot and check STOP, health/thermal constraints,
exact KMD164 and CPU baseline hashes from STATE.md. Pin candidate UMD CC82A2D9
(full hash in M686), hosted ICD3508416F and the freshly built control. Revalidate
all hashes and the registered ICDCF3948D6; run121's old93B1D1FD assumption is stale.
Retain rollback assets and an independent restoration path before temporary router
or ICD changes. Keep CPU DWM; this is a small-client audit control, not a desktop
promotion. Record original DWM identity and verify retention/restoration afterwards.

Require terminal exit0, both exact pixel checks, all8 checkpoint acknowledgements,
expected copy byte count, loaded module hashes and no reset/timeout/health failure.
Use analyze-map-lifetimes.py with each required START/END and the full process log
prefix. Reconcile sequences/checkpoint totals and inspect live maps; allow-live is
an inventory option, not acceptance of their contents. Require map bucket coverage
without overflow and preserve uploader events. A missing marker, wrong map count,
wrong pixel, or missing deliberate-copy detection rejects the control.

Only after this control passes should the candidate enter a bounded desktop run
with explicit workload/capture markers, DWM-owned GPU execution/fences, and
persistent-map/CPU-copy coverage. A passing small window is not G0 acceptance.

## Texture draw mode

--texture-draw preserves the eight markers and intentional UpdateSubresource
control, but replaces the clear-only GPU interval with16 full-screen triangle
draws alternating two immutable1x1 textures. At the readback boundary, two more
draws verify magenta and green after black clears, avoiding assumptions about
which swapchain buffer Present leaves available. Both shaders use t0/s0. This
exercises descriptor updates, not separate sampler indices or nonzero base vertex.
The final deliberate-copy readback remains yellow. Require all three76800-pixel
checks,18 reported draws, completed get_descriptor spans, matching checkpoint
counters and exact candidate module witnesses. Record log size/rate separately;
audited timings are not performance results.
