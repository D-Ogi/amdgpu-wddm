# Next input-to-present control proposal

Current ETW is not a continuous-input experiment. Regular vsync and idle
WaitForWork gaps cannot establish either cursor smoothness or input loss.
The driver advertises no hardware pointer; software cursor updates depend
on the input, composition and presentation path.

Before another S4, verify a capture configuration on the current cold boot
that contains a known stream of mouse movements and a matching visual oracle.
Prefer physical mouse movement with a timestamped owner interval; an automated
synthetic input control may distinguish USB/input delivery from DWM, but does
not replace the physical control. No credential entry or login is needed.
Do not run input injection on the development PC or silently change desktop.

Capture only the required providers plus kernel scheduling, at bounded duration.
Prove that the selected Win32k/HID input events actually carry the movement
samples before relying on them. Preserve event sequence/time, DWM work/present
and visible pointer updates; absence of a provider event alone is not input loss.
Then repeat matched capture after S4 using the same146 binary identities and
monitor setting. Separate active capture from WPR setup/rundown. If the needed
input oracle cannot be captured at sign-in, state that limitation instead of
interpreting idle gaps as a stall.

No new lab run has been started by this proposal.
