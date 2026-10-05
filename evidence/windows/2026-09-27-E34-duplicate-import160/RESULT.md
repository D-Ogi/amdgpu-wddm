# Duplicate-import lifecycle on KMD160

Unit A,2026-09-27. KMD160/SYS8E676C81. Test binary54D24107,
source4e692cd, as in M633. CPU126 and hosted GPU127 both pass101 exchanges,
two allocation generations and three independent D3D devices across two processes.
Each test checks207248 parent pixels,216240 child pixels and29376 owner-exit
survivor pixels (452864 total), with zero mismatches.
At iterations0 and100 the child closes one import, writes through the retained
device and reopens the first. Normal owner exit retains usable shared storage
and permits further writes. These runs do not test abrupt owner exit.

GPU127 loads UMD5C74BF98 and direct hosted ICD3508416F with bootstrap7A9970CA
and routerB2748CF0. Its stdout identifies the Zink module; hosted-selected.txt
preserves runtime callback witnesses. Every test serializes producer completion
with EVENT queries and CPU IPC; staging readback is the image oracle.
This does not prove asynchronous GPU cross-device ordering or CPU-copy-free
desktop presentation.

Initial127 launch stops before task creation on a stale done123.json reference
in the launcher. Preserved failure log/script records it. The corrected launcher
requires done126.json exit0, still rejects an existing127 task or receipt, then
starts the only GPU127 workload. No library replacement occurs in the failed
preflight.

CPU UMD8279AC7F and registered ICD93B1D1FD are restored. Closure08:45:18Z
and independent08:45:27Z snapshot show health15/guard0,1000MHz/VID116,66.625C,
CPU DWM4596 and the same OS boot. Both tasks removed; no active test processes.
Summary logs report no TDR. Interop0 and GPU Present0 remain; all BGP1 counts0.
Full G0 and actual CDD/BGP1 admission remain open.

Raw stdout/JSON/scripts are copied unchanged. Selected driver/runtime text
preserves relevant lines after encoding conversion; full archives stay in
scratch/g0-hosted/duplicate-runtime002. No owner visual verdict was requested.
