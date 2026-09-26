# Bounded X11 event wait diagnostic (M522)

Hypothesis and patch: experiments/E33-m12-applications/linux-x11-teardown-diagnostic.md.
Mesa base05e6c962 plus canonical scratch fix. Linux RADV candidate
86DAD7113E3724F7D99C7A5BE61FB62DACE3F4418A763C5AA845302D27D45C25
lives only at /opt/bc250-x11-wake; original FEC7C475 remains installed unchanged.
Source/build currently include the diagnostic patch; do not mistake it for pristine.

After M521 timeout the original swapbuffers positive control again aborts134.
Only Xorg is restarted (5668 ->7618); same boot and amdgpu instance remain.
Candidate swapbuffers then pixel-passes/exit0. Exact same six names and runner
in replay009 yield five passes and one pixel-fail, not timeout. The final test
finishes in1.297s according to runner timestamps. Post-control also pixel-passes
without another Xorg reset. This supports the missing teardown wake hypothesis,
but a bounded-polling workaround is not a validated performance architecture.
Original timeout stays recorded; no passing full-profile result is claimed.

A separately compiled diagnostic copy of the multi-window test logs viewport
and read/draw buffers. All windows start with viewport0,0,50,50 and buffer0x405
(GL_BACK). A second diagnostic forces viewport50x50; both still fail black
pixel probes/exit1. Wrong viewport dimensions do not explain this observation.
Original test source is restored and rebuilt; installed suite is untouched.
Neither modified diagnostic test is counted as conformance coverage.

All workers terminal. Xorg7618 remains active. Next distinguish drawable/back
buffer lifetime from rendering loss for the shared Zink/RadeonSI pixel failure,
and replace/assess the timed-polling diagnostic before performance comparisons.
Full Linux/Windows piglit, Vulkan CTS and M12.1-M13.1 acceptance remain open.
