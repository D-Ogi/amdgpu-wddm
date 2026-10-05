# Cache147 shared pixel probe

Source/build only, 2026-09-25. No development-PC GUI or target execution.

Files: render_probe_cache147.cpp/.exe, prepare-control.py, build-control.ps1,
run-control.ps1, build-control.log. Derived from the existing E26 render_probe.cpp.
No production changes.

Final EXE SHA256: D4876E666ECA953E087A0B045828154E0486DEB00401E494B11F7DBB4A83BAAF
Final C++ SHA256: 581E739CBDDE9F3345CD87A11AAEDDC22131F0772964C8163F99965DB3C79888
The earlier EEDE... build is superseded before deployment.

Copy EXE and runner into C:\BC250\m13\cache147-control on the target, then invoke:

    .\run-control.ps1 -InteractiveUser <verified-existing-interactive-user> -ExpectedUmdSha256 <full-new-DLL-SHA256>

Caller must supply an existing interactive user identity, not infer credentials or
create a login. Task uses InteractiveToken; no token means launch failure. A locked
Default desktop is allowed, but logical pixels are not proof of physical visibility.
The output path is fixed to C:\BC250\m13\cache147-control\render-probe.txt. Runner
refuses an existing log rather than overwrite evidence. It uses a unique task name,
30 s execution limit, 35 s caller bound and removes its own task afterward.
No registry changes, resets, MMIO dumps, log-summary polls or input injection.

The probe uses the currently registered hardware UMD, never LoadLibrary with a
historical path. It logs GetModuleHandle/GetModuleFileName for bc250d3d.dll after
D3D device creation; runner hashes that path and compares ExpectedUmdSha256.
This proves the loaded module path plus on-disk identity at check time; it is not
an in-memory image digest. Do not replace the DLL concurrently with this check.

Content controls: shared64x32 allocation opened on a second device, A-to-B red and
B-to-A blue, all2048 pixels checked each way; green640x480 backbuffer readback with
all307200 pixels checked; 60 windowed presents. Query EVENT completion is checked
after each producer clear before cross-device read, and after the green staging
copy. Errors and >5 s poll intervals fail. Runner requires all positive markers,
process exit0, device-removed status0 and matching loaded DLL file hash.

Source contract review: D3D10UMD Query.cpp maps EVENT to PIPE_QUERY_GPU_FINISHED.
Its current QueryGetData has wait=!!(Flags & DO_NOT_FLUSH), so this probe deliberately
passes GetData flags0 after explicit End+Flush. That yields wait=false in this UMD;
llvmpipe_get_query_result returns false until the actual scene fence is signalled.
Using DONOTFLUSH here would cause an internally blocking wait. On a failed query,
query destruction itself can wait on its fence; the outer30s task limit remains
necessary. No claim that arbitrary broken kernel calls can be interrupted is made.

Build: MSVC /W4 /WX /O2 /MT /EHsc, SDK10.0.26100 including WRL; passed. PowerShell
runner parser reports0 errors. EXE not run locally because it opens a window and
D3D device. No live shared/color acceptance result is claimed in this report.
