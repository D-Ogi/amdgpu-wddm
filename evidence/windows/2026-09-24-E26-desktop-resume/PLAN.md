# M13.1 resumed - isolate visible presentation on127

Owner prioritized display after M405. Baseline full127 uses stub UMD with
PresentBlit/VidPnFlip closed. Capture actual HUBP scanout, registrations,
DWM events and KMD counters. Enable only diagnostic PresentBlit via existing
PnP lifecycle, retain current GPU startup policy and hardware display-write
gates closed. Restart DWM once; capture before/after scanout and Present logs.
If black/noise persists, compare packet construction/submission and source/
destination identity before changing DCN or registering Mesa softpipe.
CPU copies are diagnostic, not GPU acceleration. No routine OS/AC reset.
Sources: current127/M404 package; E26/M149, ADR0011, local consolidated Microsoft
DDI documentation and WDK26100. Evidence immutable; no M13.1 closure without
visible positive controls and30minute stability acceptance.
