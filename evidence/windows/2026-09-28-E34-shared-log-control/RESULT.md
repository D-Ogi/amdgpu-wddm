# M700 - default UCRT stderr keeps records visible with lower control cost

Control source: experiments/E34-native-d3d-zink/hosted-runtime/test-shared-log.cpp
and test-shared-log.py, with dwm045-store-census/shared-log.h. MSVC17.14,
/O2 /MD /W4 /WX, same CRT linkage as the router. Exact hashes in build.json.
Build from the output directory with:
`cl /nologo /O2 /MD /W4 /WX test-shared-log.cpp /Fe:test-shared-log.exe`
Copy the exact header beside the source first. Run test-shared-log.py with the
executable path and a new output directory. The lab PS5 runner executes the
same four modes, checks return codes, every sequence and unchanged DWM identity.

Each of1000 fprintf records is read immediately through an independent handle
and compared byte-for-byte before the next record. No fflush(stderr) occurs.
The process then calls TerminateProcess on itself, skipping CRT/DLL teardown.
Legacy/default/reopened-stream modes all preserve the same199780 bytes and hash;
full buffering fails on record0 and leaves zero bytes after forced termination.
This is visibility in the OS file cache, not a power-loss durability claim.

Host control: legacy727610.8us, default6886.9us, reopened6856.8us.
Lab control: legacy2734053.4us, default31187us, reopened28597us.
Times include each independent read/compare. This single synthetic control
measures neither DWM frame time nor overall driver performance. Lab CPU DWM
identity was unchanged. No GPU trial, driver replacement or reset occurred.

An earlier invalid control used _close beneath a still-open FILE and triggered
CRT fast-fail; it was replaced by fclose before _wfreopen. An intermediate
ExitProcess test allowed DLL teardown; the accepted test uses TerminateProcess.
Those earlier controls are not the evidence above. All raw controls retained in
scratch/g0-hosted/dwm045; exported JSON contains only test results and hashes.

DWM045 removes explicit _IONBF while keeping UCRT's default stderr mode. It
retains the4s deadline and all rollback/acceptance gates. The GPU run is prepared,
not measured; G0 and the causal effect on DWM checkpoint timing remain open.
