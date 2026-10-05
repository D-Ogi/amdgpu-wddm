# DWM mode-change diagnostic continuation

KMD 0.7.54.1, aperture Mesa build, same boot as the previous continuation.
The attached mode-0754/mode-debug.log is a user-mode DWM trace captured on the lab.
No live kernel debugger was used. CDB detached immediately after its first matching
DXGI journal event. The matching local dxgi.dll/PDB resolves the stack as:
CModule::RecordJournalImpl, ScenarioEnterOrLeaveFullscreen catch$26,
ScenarioEnterOrLeaveFullscreen (return address after CreateFullscreenObjects),
SetFullscreenState, InitializeSwapChainCreationStateFullScreen.
The exception has already unwound; this does not identify its original throw site.

The next attempted capture, mode-throw, requests first-chance C++ exception stack
capture and immediate detach. SSH stopped responding during this attempt. Its
execution and final display-only restoration are not yet verified. The preceding
modestack probe confirmed display-only restoration at 00:44:12 local time.
Further remote evidence collection failed because SSH was unavailable.
