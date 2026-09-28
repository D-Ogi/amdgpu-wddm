# M721 - DWM049 deferred-report correlation

[Sample timeline](deferred-timeline.json), derived by the included script from immutable M719 startup summaries and the ETW FILETIME origin. Receipts timestamp the start of a summary read, not individual counter updates. The summaries are not atomic.

At sample25 (trace29.484005s) deferred=1,old-buffer=1. Sample26 starts at30.659015s and reads deferred=2,old-buffer=1. The sole ETW missing-report gap is30.655600..30.688985s. Later snapshots retain the difference1; the later third deferral also increments old-buffer and does not add a missing report. This is a coarse correlation with the non-reporting fallback in exact KMD169/5985a416, not a direct branch trace.

WddmDcnVsync consumes ACK before fallback. Its four suppressed-report exits are odd primary generation, scanout-read failure, scanout equal to the requested address while completion was not proven, and changed generation. The existing counters cannot identify which exit occurred. No speculative early flip completion was introduced.

Diagnostic KMD170 source d1fd796 on isolated g0/kmd170-vsync-defer-reasons adds four counters and four last interrupt-time stamps at these existing exits. It preserves conditions, MMIO and notification semantics. ABI000700AA/package0.7.170.1; signed SYS CE56F40CFEB8683F11D8F0136DE1B5523BEEFDD7185087F857FC9F8512AD4358. Build gates and source/artifact capture pass. Existing extracted-code VSync vector test passes coalescing, repeat/idempotence, disabled and fault controls. Build receipt remains scratch/g0-hosted/kmd170-final001/source-manifest.json; build log kmd170-build001.log. These checks do not prove runtime branch coverage.

KMD170 is not deployed. Lab remains confirmed CPU169. Any change that retries a consumed ACK must address the producer/consumer handoff and lost-wakeup races rather than simply queueing unlimited DPCs. G0 remains open.
