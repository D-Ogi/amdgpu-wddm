# Shared router startup log controls (M668)

Host preparation, 2026-09-27, following29c4da3/M667. DWM031 is not staged or run.

The actual old CRT _wfreopen_s writable mode refuses a reader even when that reader
offers FILE_SHARE_READ|WRITE|DELETE, returning ERROR_SHARING_VIOLATION. This negative
control exposes the limitation of the earlier cooperating-writer wrapper test.

The successor router uses RedirectSharedLog from shared-log.h: _wfsopen with
_SH_DENYNO, checked _dup2 to stderr, closes the temporary stream, then makes stderr
unbuffered. The exact same helper is compiled into test-shared-log.cpp. Its positive
control reads the actual CreateDevice marker while stderr remains open and admits
a second logging stream. Both CRT controls pass. Reader sharing from M667 remains
necessary as well. No product UMD, ICD or KMD binary has changed.

DWM031 router, composition control and CRT control compile with /W4 /WX. All prepared
PowerShell scripts parse on the host and source control-byte checks pass. The runtime
manifest records20 files; shared readiness helpers are the corrected M667 versions.
The stage verifier now checks the exact163 rollback artifact for KMD164. First
successful CreateDevice observation UTC is retained in the readiness receipt;
this is an observation time, not an exact driver call timestamp.

Next validation requires target PS5/staging checks, bounded runtime and ETW device
latency correlation. No desktop image verdict or G0 acceptance follows from these
host controls. G0 still requires GPU content, dwm.exe ownership and exclusion of
whole-frame CPU copies.
