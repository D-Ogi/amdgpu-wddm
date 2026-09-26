# Headless launch after control1 wrapper failure

Control1 has no native completion markers or GPU submission witness. Windows
Terminal Application Error1000 at14:34:08 reports0xc0000005; task result is
0xC000013A, no vkcompute remains, DWM4448 continues. No conclusion about
RADV compute correctness. Stop the waiting SSH observer after preservation.
Use an explicit headless conhost task action for control2 to isolate the
console-host failure. Same DLL, workloads and output checks; new output directory.
No OS/device/DWM reload or default console setting change. Preserve first trial.
