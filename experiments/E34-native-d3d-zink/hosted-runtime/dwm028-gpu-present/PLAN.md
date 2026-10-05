# DWM028: exact private-span correction on real GPU Present

Exact163/B602D9A0 source2c3849a, rollback exact162 final002/58189FA6. Hosted
UMD5C74BF98/direct ICD3508416F unchanged. BGP1 requires exactly24 consumed
private bytes; all other admission and genuine engine-fence rules unchanged.

CPU baseline8000/8000 precedes mutation. Collector starts before gates1 transition.
First startup sample requires submits>0/rejected0/failed0, otherwise immediate
rollback. Maximum180-second measured animation,300-second independent watchdog,
270-second producer lifetime. Preserve ETW, startup context/root/IB/fence records,
image captures, CPU-copy counters and mappings. No unrelated GPU workloads.

After the animation interval only, create freeze marker. Control updates pending
paints and calls DwmFlush once successfully; record frozen=true/result0 and exact
moving-client rectangle. Require acknowledgement within50 polls100ms, then take
final primary/GDI captures. This does not serialize rendering during measurement.
It removes moving-window changes during final capture; unrelated desktop activity
is not frozen. Dynamic captures retain their original non-atomic limitations.

Require actual BGP1 hardware completion and correct content; no-op or CPU fallback
is not acceptable. Do not infer full G0 from healthy DWM or startup counters alone.
Archive/cleanup only after original collector terminal; never restart on timeout.
