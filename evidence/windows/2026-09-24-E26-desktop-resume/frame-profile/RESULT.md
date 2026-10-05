# M408 - CPU drawing dominates visible frame stalls

Owner reports approximately one second of smooth cursor motion followed by
one second frozen, and the overlay seconds skipping. This supersedes any
interpretation of M407's intact image as responsive-desktop acceptance.

Instrumented UMD E1976C4254055F0B96E294E713DBC5798C07E41E61526DD6FC3738A712FC1CAD
retains rotation and softpipe, KMD127 unchanged. First32 DWM frames show ordinary
two-draw frames taking about380-400ms in draw entry points, with each draw about
180-210ms. Large draws exceed2seconds; frame4 accumulates5366.545ms drawing.
PresentCb is ordinarily0.04-0.06ms, with a sampled maximum2.602ms. These are CPU
wall times, not GPU timestamps. Inter-Present gaps include runtime idle time.
The measurement locates a dominant draw cost; it does not prove every stall has
one cause or separate shader execution from memory accesses within a draw.

Next bounded route: build llvmpipe using LLVM19.1.7, matching the current Mesa
Windows CI dependency version, and compare identical color/control/desktop
workloads. Llvm workers require an explicit render-fence wait before Present;
softpipe's synchronous flush cannot be assumed for a threaded renderer. CPU
rendering remains a diagnostic baseline, not M13 hardware acceleration.
