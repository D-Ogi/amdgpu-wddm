# Instancing windowed resolution and D3D9 rendering

PROVENANCE: DirectX-SDK-Samples MIT; DXVK zlib; Mesa MIT.

Unit A, KMD151, Mesa05e6c962 candidate8B5EC055, DXVK3.1.1. Diagnostic002
adds source-line logging to DXUT enumeration without changing selection.
Both APIs exit3 with DXUTERR_NOCOMPATIBLEDEVICES (80040902). The command
requests a1080x720 window; DXUT checks preserved dimensions against fullscreen
monitor modes even for windowed combinations. Restricting that check to
fullscreen allows both applications to create their devices and render.
No driver capability check is weakened; exact window dimensions are retained.

D3D9 diagnostic003 completes with exit0 and a capture, but the runner correctly
rejects661 CSV rows. DXUT increments its counter and quits when it is greater
than -quitafterframe. Runner now uses659 for exactly660 completed frames.
Diagnostic logging is removed from final package004. The four-file benchmark
patch applies to pinned07e3eaa1 and matches the compiled source (LF normalized).

D3D9-004 completes660 ordered frames, exit0,1080x720 PNG, and witnesses all
three required modules. Visual inspection shows the textured colored cube
array. Health retains generation560773206/epoch5. Timing covers CPU render
submission only, not present or GPU completion; this is not performance acceptance.

D3D10-004 creates the device and records660 ordered frames, with all five
required modules witnessed. D3DX10SaveTextureToFileA fails with80004005 at
frame660 and the application exits5. No PNG exists, so this run is not accepted.
The capture failure's cause remains open; it is not evidence of a GPU reset.

Registry restoration is verified after every retained trial; no OS restart
was requested. Candidate not promoted. Linux/Proton image and performance
parity, full CTS, OpenGL/OpenCL and the remaining M12/M12.1 scope remain open.
