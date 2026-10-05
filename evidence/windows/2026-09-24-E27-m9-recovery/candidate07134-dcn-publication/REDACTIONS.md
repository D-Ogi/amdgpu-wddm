# Privacy and capture handling

Only unique PCI instance/interface paths were redacted in the following files;
native encoding, line endings and other output are retained. Original raw files
remain in workspace scratch/m9. No firmware binaries or secrets are included.

- startup-info.log
- preflight.log
- install.log
- guard-control.log
- final.log

The BMP scanout includes local network addresses in the overlay. It stays at
scratch/m9/dcn134-artifacts/d3d-scanout-0.bmp outside the repo. Its SHA256 is
recorded in validation.json. It was visually inspected via an in-memory reduced
JPEG preview after PNG exceeded the tool output limit. No source BMP was edited.
The banded fbdump capture is not an atomic snapshot of one refresh.
