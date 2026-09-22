E24 run 004, 2026-09-22 11:11-11:15: bc250kmd 0.7.25 (5dcf020) on unit A, owner at the monitor, first
bring-up of the 11:06 boot (the freshness guard of M78 passed).

Two questions in one run, the cheap control first: does the IH ring deliver anything at all in a device start
where the engines are up (M103 left that unproven), and does the paging node ever get real work if VidMm is
actually short of memory (M102: an idle desktop never asks). Order: the eight stages with `-PagingNode 1
-VidPnFlip 1`, then `fence gfx x2`, then `kmtprobe --size 512M --hold 20`.

Result, control: the ring delivers. `ih state` went from 0 interrupts, 0 vectors, rptr 0x0 wptr 0x0 to 202
interrupts, 203 vectors consumed, rptr = wptr = 0x1960, and the vectors printed are `client 4 source 87`, the
VUPDATE_NO_LOCK identity of M88 - not the CP fence's own (M79). That is M106, and it takes M98's and M103's
"nothing reaches the IH" off the table: the display interrupt does arrive once the ring is up.

Result, pressure: VidMm took the bait and the driver killed the machine. Bugcheck 0x50
PAGE_FAULT_IN_NONPAGED_AREA at `nt!_chkstk+0x36`, on `dxgmms2!VidMmWorkerThreadProc`, through
`PageInOneAllocation` -> `CommitResource` -> `TransferToSegment` -> `FillAllocationUsingGpuVa` ->
`DdiBuildPagingBuffer` -> `Bc250WddmBuildPagingBuffer` -> `GfxPagingBuild` -> `bc250_sdma_paging_fill`. The
stack probe walked off the bottom of a 24 KB kernel stack because that function declared a `struct
amdgpu_device` - 0x5B00 bytes - as a local. M104 and M105. Nobody is to blame but the host tests' 1 MB stack:
`run_paging.ps1` had been green on the same code for a day. Mądry Polak po szkodzie (a Pole is wise after the
damage).

Fixed in 0.7.26: the live `adev` is passed in, and `tools/win/stackbudget.py` now walks the built driver's
unwind data at every build and fails it at 4096 bytes - 23,576 for those two functions in this build, 872 as
the whole driver's largest in the next one.

Files: `run-004-console.txt` (the console, recovered from the session transcript - the machine died before
anything could be collected from it), `run-004-script.sh` (the script as it ran), `kd-analyze.txt` (the whole
debugger session on the minidump: `!analyze -v`, the faulting frame's registers, `!thread`,
`sizeof(struct amdgpu_device)`, and the raw stack walk that recovered the dxgmms2 frames the bugcheck's own
stack text had lost). The dumps themselves stay out of the repo: `C:\Windows\MEMORY.DMP` (817,096,158 bytes)
was left on the target, `scratch/dumps/092226-58796-01.dmp` (1,632,348 bytes) is the minidump this analysis
ran on.
