# DWM027: BGP1 consumed private span is24 bytes

Exact162 final002/58189FA6 source68fc8f2, runner7f86111/manifest718821f,
hosted UMD5C74BF98/direct ICD3508416F. CPU baseline passes8000/8000 on attempt1.
GPU Present calls10/records10/rotate6/refused0; submits0/rejected4/failed0.
No CPU Blt in the GPU-gated generation. Automatic rejection rollback worked:
last measured7.6217534s, done.success=false, GPU DWM13816 -> CPU8936.

For all four rejections, the same-context SetRoot callback resolved1 to physical
46DFFF000 before submit. At rejection: gate1, umd0, node0, IRQL0, root46DFFF000,
private capacity2184, DmaBufferUmdPrivateDataSize24, actual IB4096, record match1.
All six BGP1 words match header/version/record length/IB length/address. The sole
failing admission predicate was the requirement that UMD-private length be zero.
The root hypothesis is refuted for these four packets. Build-time root0 precedes
the first SetRoot and is not a submission-time root failure. admission-analysis.json
independently joins every reject to the same resolved context in the unmodified
startup log. Raw archive/ETL remain private; hashes are retained.

The producer advances pDmaBufferPrivateData by24. In this observed non-UMD Present
path dxgkrnl reports those used bytes in DmaBufferUmdPrivateDataSize, despite its
name. Prior CPU E26P behavior did not establish this new producer's used length.
The local d3dkmddi reference describes UMD SubmitCommandCb usage but does not
explicitly describe this KMD-generated Present case; do not generalize this
measurement to arbitrary submissions.

Fix: Bc250GfxPresentSubmitMatches requires exactly24 consumed private bytes plus
all existing record/capacity/VA/IB-size checks. Only the non-UMD BGP1 branch uses
it; gate/context/root/node/IRQL and genuine engine-fence completion stay enforced.
No arbitrary nonzero length or skip-completion fallback is admitted. Host replay
uses capacity2184/used24/IB4096/VA11000, including repeated validation and negatives
for zero/23/25/full-capacity lengths, short capacity, wrong VA/size, NULL, and each
mutated record byte. All13 quality gates and real WDK wddm.c compilation pass.
The fix has not yet been built into a deployed KMD or validated on hardware.

Collector5652/start10:38:18.927Z terminated10:41:20.5417296Z,160 samples, no reader
timeout. Initial cleanup refused while that original process was still alive;
archive ran after termination and cleanup was then repeated successfully. The
trial itself was never rerun. All027 tasks removed. Snapshot10:41:43.6061573Z:
exact162 health15/guard0,1000MHz/VID116,66.75C,no test processes. Gates0, baseline
UMD8279AC7F/ICD93B1D1FD restored. CPU8936, same OS boot. No owner visual verdict;
this is a failed BGP1 probe with a measured cause, not G0 acceptance.
