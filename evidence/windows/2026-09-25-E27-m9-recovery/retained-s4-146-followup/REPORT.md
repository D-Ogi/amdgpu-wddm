# Post-S4 follow-up: stutter and loss of SSH execution

2026-09-25, unit A. Supplement to immutable M463 retained-s4-146.
The owner reports mouse stutter on the sign-in screen, not a stable unlocked
Windows desktop. The owner requested remote sign-in and authorized SYSTEM,
then cancelled the need for sign-in. No successful remote login is claimed.
The existing password was passed only through stdin/named pipe for attempted
WTS session connection (7045); never printed or written to helper files.
A later secure-desktop helper did not receive the password.

SSH command execution began timing out while the previously verified address
still had an open port. Pinned-host discovery did not find an executing Windows
endpoint. Successful earlier GPU/model content tests do not prove later system
or desktop stability. No causal attribution to IH, DWM, the helper or CPU load
is established by these timeouts alone.

After repeated execution timeouts made remote DWM restart unavailable, one
identified-plug recovery was performed: off01:26:03.654763Z,
on01:26:35.126465Z. This was recovery, not a second S4 acceptance test.
The lab's return and helper/task cleanup remain to be checked. The owner no
longer requests remote login. Preserve146 and diagnose input stutter.
