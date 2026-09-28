# BGP1 rejection diagnostics after M652

The original admission expression and failure status are unchanged. For the first
16 rejections per adapter, at IRQL<=DISPATCH_LEVEL, log context/fence/node/IRQL,
gate/UMD/root/private lengths, actual VA/size/match and six record words. Record
words are copied only when the reported capacity covers24 bytes; otherwise zero.
Four bounded lines avoid the160-byte log limit. Successful first16 BGP1 submissions
also record root and node alongside existing context/fence/VA/length witnesses.
This does not prove execution or identify M652's failing predicate retroactively.

All13 quick gates pass. The changed wddm.c compiles with real WDK10.0.26100,
MSVC14.44.35207 /kernel /W4 /WX. arguments.json is the actual compiler argv;
outputs are isolated under scratch/quality/gpu-present-reject001/kmd-compile.
No new KMD binary is deployed. Exact161/40F7916F remains on the lab, both gates0.
A clean isolated build/package and bounded runtime probe are still required.
