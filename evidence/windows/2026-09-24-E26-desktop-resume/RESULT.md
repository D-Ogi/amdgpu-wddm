# M406 - Full-WDDM display becomes visible with Mesa and hardware flip

Unit A, KMD 0.7.127.1 unchanged, Windows boot 2026-09-24T11:44:14.
The compute configuration registered the stub D3D UMD and closed both Blt
and hardware flip gates. HUBP scanout was entirely black. Enabling CPU Blt
alone executed copies but retained black scanout and DWM 0x8898008d exits.
Changing UMD registration with only a DWM restart did not load Mesa; after
PnP reload the loaded-module witness shows the exact E26 branch-labels DLL.
DWM then presented Flip|FlipWithNoWait (0xC) and scanout stayed black while
hardware flip remained closed. Enabling the existing EnableDcnWrite and
EnableVidPnFlip path produced a visible desktop, confirmed by the owner and
actual HUBP framebuffer dumps. No new KMD or register sequence was needed.

The owner observed repeated descending bands in the overlay. That configuration
is not accepted as correct. Follow-up M407 addresses resource identity rotation.
No Windows restart or AC cycle occurred. Generic EnableMmioWrite remained0.

Raw scanout BMPs stay in workspace scratch/m13 because the overlay displays
private network details. scanout-analysis.json records hashes and RGB extrema;
public previews crop to the left1400 pixels and are not full-frame proofs.
Text logs redact interface/PCI instance identity only; UTF16 logs are decoded
without altering remaining line endings. Source package is M404's exact127.
