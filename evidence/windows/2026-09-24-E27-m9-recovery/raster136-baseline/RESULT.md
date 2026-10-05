# M444 - Hardware raster API baseline on deployed136

Unit A,2026-09-24 22:39:17; retained Windows boot21:14:48 and full KMD136,
SYS1C93F3578BC53FFA4DF0C32B2B12C93201671A54754D4B59C27EDE1613C1517D.
Read-only probe, no driver/device/DWM/OS transition.

The probe opens the sole active display's adapter LUID and source from Windows
QueryDisplayConfig, then queries D3DKMTGetScanLine1024 times. All calls succeed;
all1024 report InVerticalBlank=1 and ScanLine=0. The final sample is15.974733s
after the first-query start. This matches the known stale software-timer branch
in deployed136. No corrected-driver result or physical beam position is claimed.

Probe SHA256 AA96C9E9140A7B0EB6BAA8189C2E3C1C6709766A6661840EC3F395924602696F.
It runs in the lab interactive session via headless conhost; never on the dev PC.
Initial build used the wrong QDC constant spelling; SDK wingdi.h specifies
QDC_ONLY_ACTIVE_PATHS. Corrected /W4 /WX build passes; both logs preserved.
Only device-interface identity is redacted. Raw samples remain otherwise unchanged.

M443 source changes await integrated hardware acceptance. A frozen137 package
was built (SYS CC1A0EDAA0402D6E45CE2BD0D76AAEFEAD081BCD9D877F829719D2B98CBE3923)
but not installed: the next batch includes display handover changes that should
be integrated before another PnP transition. There is no inference of improvement
from source tests or this baseline alone.
