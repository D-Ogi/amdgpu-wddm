# M440 client positive-path addendum

Source/host only, no lab or development-PC hardware access. This supersedes only
the reader CLI source/header and reader build identities in typed-smu-clients.
The KMD, control DLL and managed client sources there are unchanged.

Final source review found a leftover `(!argc < 3)` term after removing raw smu
argument parsing; this made the first built reader CLI refuse every command.
It compiled cleanly, demonstrating why the initial build was insufficient.
Removed the term and rebuilt. The first execution control then exposed the
legacy2000MHz/1129mV CLI ceilings: the test backend correctly rejected the
request, but the CLI should have rejected it earlier. Aligned those limits to
hardware.md and the KMD policy1500MHz/900mV, then rebuilt once more. Both findings
were corrected before any deployment; no new backlog item was added. BD-004 is
the existing matching limits item and remains live-integration work.

Final5 controls pass using a test DLL in an isolated scratch directory:
- Actual reader CLI `clock 1000 820` and `clock-check 1000 820` reach the typed
  request and report1000MHz/VID116, exit0.
- Out-of-policy2000/1129 and removed raw `smu` command exit2 before a request.
- Actual compiled managed Driver performs READ, SET and temperature conversion
  against the same fixture, all expected values match. The monitor entry point
  is never called: no UI, resident process or global hotkey runs on this PC.

The fixture exports the real signature but performs no adapter lookup or hardware
operation. This establishes argument routing and native/managed marshaling, not
live SMU behavior. Native transport/owner controls are in typed-smu-clients.
Fixture source was built in scratch/m9/smu-client-fixture/ using the retained
build.cmd; exact final binaries are hashed in identities.json. Earlier failed
client test output remains alongside the successful fresh log. Lab135 unchanged.
