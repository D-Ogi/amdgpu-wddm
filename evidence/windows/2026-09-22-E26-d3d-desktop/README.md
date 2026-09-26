# E26 desktop investigation - incomplete

Base repository: bed764d. Unit A, Windows, boot 2026-09-22 22:14:36.
No GPU engine bring-up in these probes. M8 compute is a separate result.
Full WDDM does not yet produce a working desktop; every probe restores the
stub registration and display-only in a finally block.

PROVENANCE: Mesa (MIT), source checkout 801c9763c6043f0de8408e905a5324eea06d81d7.
The D3D10 frontend uses softpipe: CPU rasterization, not GPU acceleration.

## Controls and results

- Direct WARP registration fails hardware-device creation; standalone WARP works.
- Mesa registration creates a feature-level 10_0 device. This alone does not
  establish composition, sharing, presentation, or stability.
- render_probe clears 640x480 RGBA pixels green and reads every pixel through a
  staging resource. The initial color run has zero mismatches and a green
  pre-GDI frame; scanout during the run is black. That run restored the adapter
  before the rendering process exited, so its final device-removed result is
  not a stability measurement. Later scripts wait for task completion, with a
  60-second deadline, before restoration.
- Explorer displayed an assertion in tgsi_exec.c: TEMP-relative source indexing
  was treated as ADDR-only. The diagnostic patch restores generic register-file
  fetching for source and dimension indices. tgsi-index-test-verified.txt is
  captured stdout of the executable regression test; the earlier similarly
  named file transcribes the same initial tool output.
- KMD revision 49 moves the gated CPU copy from Present construction to
  SubmitCommandVirtual. order-callback2/mesa-order2-kmd.txt shows the root set,
  a 4096-byte software submission, then 1200 rows copied. Scanout remains black.
- Callback prototypes are unfinished. Device-hung errors and failed callbacks
  are retained; no successful return is treated as proof of displayed pixels.

## Capture limitations

Root text files are the first snapshot. order-callback2 is a later snapshot;
process traces can contain multiple adapter tests because a process can survive
adapter restart. Probe prefixes and times identify each run. Deliberate DWM
termination is logged as exit 0xffffffff and is not evidence of a spontaneous
crash. Some probes did not finish all captures (for example order stopped when
Explorer could not be terminated); finally still restored display-only.

PNG images are reduced previews of raw BMP captures. Green frame-4876-30 uses
RGBA bitfields; scanout previews are 960x600. Raw capture archives remain under
workspace scratch/dwm. No shader trace, HWND, or kernel pointer is a GPU register
address. No hardware register, firmware, or disk-layout experiment is included.

The installed KMD 0.7.49.1 is C765B3A77FDE95CFE3DC1D0464C937842E8D0A601B360285851C18DC859513E8.
The original M8 suite has not yet been rerun on this KMD revision.
