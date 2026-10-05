# Paired headless inference benchmark

Control3 passes8shader CPU hashes and both E14 model references, native0.
Run new ICD then old cache-intent-v2, each in fresh processes using the same
headless console worker and native completion markers. KMD127/M412 desktop,
clock, application/model hashes and pp512/tg128,r3,t6,ngl99 settings unchanged.
This additional old run controls the launch-method difference and termination
anomaly in M413. Retain M413 separately; it is not replaced. One pair with three
internal repetitions describes this session, not a broad performance guarantee.
New ICD retains unsupported sparse disabled. No OS/device/DWM reload.
