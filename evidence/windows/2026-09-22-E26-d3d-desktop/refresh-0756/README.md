# E26 allocation refresh consistency

KMD0.7.56.1 (oem65.inf), early-context Mesa unchanged, unit A.
SYS SHA256 55191752E4B46F6BFBD15589B0C2BF169863281740BBD5E4C1A398A2AD12A3CF.
Boot00:53:51, test01:18-01:19 local, 2026-09-23.
DescribeAllocation now returns60000/1000 matching the full VidPn mode instead
of NOTSPECIFIED. Direct3D11 ETW SetDisplayMode errors fall from239
STATUS_GRAPHICS_PRESENT_MODE_CHANGED to0 (before: ../setmode-0755.json).
Fresh DWM PID1532 proceeds into shader/resource setup and DrawIndexed;
its trace ends there. Scanout captured during the test is all black.
Render/shared controls pass, task0. Display-only restored, DWM restarted,
UnconfirmedStarts0. Full desktop still not achieved. No GPU engine bring-up.
Next investigation should inspect the blocked DrawIndexed stack without live
kernel debugging or another invasive DWM debugger attachment.
