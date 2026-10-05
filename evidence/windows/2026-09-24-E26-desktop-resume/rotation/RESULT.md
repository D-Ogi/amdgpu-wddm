# M407 - Correct resource rotation removes visible progressive redraw

PROVENANCE: Mesa 801c9763c6043f0de8408e905a5324eea06d81d7, MIT.
KMD remains 0.7.127.1. New Mesa DLL SHA256:
4A6B097AD176B8E1E1C98914CA2E7E2A1D419A6CE3FA4A18ACDB8E130C2FE7FD.

The old _RotateResourceIdentities copied pixels between textures but never
rotated kernel allocation identities. Local Microsoft dxgiddi.md (DXGI base
functions, line2469; WDK declarations26100) requires kernel handles to rotate
while runtime handles remain fixed. The patch rotates pipe resource, allocation,
GPUVA and CPU mapping together; retargets retained RTV/SRV plus bound framebuffer
and samplers; flushes preceding rendering. Rotation no longer copies pixels.
The incremental patch applies on top of the existing E26 branch-labels tree.

Validation:
- Mesa build succeeds. Extracted actual rotation function passes two/three
  buffers and six rotations each, checking backing/handle association, stable
  runtime handles, retained views and bound views. Omitting kernel-handle
  rotation produces18 failures. This host adapter is not a refcount/timing proof.
- DWM trace now rotates three handles. By41.430s the unchanged KMD reports174
  hardware flips, versus4 at41.457s with the old UMD. Owner confirmed the overlay
  stays whole and the descending bands disappeared. Actual scanout shows desktop.
- Native Direct3D control exits0: shared red and blue each0/2048 pixel mismatches,
  green staging0/307200; Present and device-removed status0. Captured scanout
  contains the green window. These shared controls use two devices in one process.
- Concurrent visible-desktop GPU residency control:64KiB,3 cycles,4 complete
  matching GPU readbacks; native0, graphics4/4 and paging1559/1559, no errors/TDR.
- Final independent observation12:38:45: same DWM11512 since12:32:51, no new
  Dwminit event after the intentional restart;3875 hardware flips,21681 hardware
  VSync acknowledgements, no refused flip/VSync.32 pending flips deferred their
  completion rather than being reported early. Temperature66.9C. SSH healthy.
  Windows boot remains11:44:14, guard count0, initialized desktop retained.

Limits: about six minutes of observation, not the30minute M13.1 acceptance or
24hour M13.7 soak. First synthetic Start-menu input did not yield a visible
menu in the capture, so menu acceptance is not claimed. Movement/resize and
broader lifecycle remain untested here. DWM draws execute on CPU softpipe;
hardware scanout, paging and separate GPU compute do not prove GPU D3D draws.
M13.1 remains open; M13 and M9 are not complete. See later MENU result for
follow-up UI checks. No Windows restart/AC cycle, no KMD changes in this fix.
