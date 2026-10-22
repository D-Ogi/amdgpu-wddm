# The ring VM flush on every job against a same-root skip: Cyberpunk 2077 (2026-10-22)

Unit A, Windows 11 22631, 40 CU, the KMD DPM with its 2000 MHz ceiling. The installed package is the tester.32
release package (0.7.216.104-tester.32, `oem3.inf` 40.7.216.104, state catalog identity `f9c68061`).
The game is Cyberpunk 2077 2.31 (GOG) with its built-in benchmark at 1920x1200 in exclusive full screen.
Every advanced option is at its High preset value, with anisotropy 8x and crowd density Medium.
Ray tracing, the upscaler and frame generation are off.

Each run started at a Tctl of 54 C or less, with the lab overlay hidden. The game owned the foreground in 98 % of
the 2 s samples, and PresentMon 2.6.0 recorded the presents. Two camera frames of each run from a1 to b2 show the
benchmark in full screen on the monitor. The camera frames are not kept here.

## The kernel drivers

| Runs | Kernel driver | SHA256 | Ring VM flush |
|---|---|---|---|
| r0 | the release kernel driver of the package | `CDE214D499379D9B0488E3D294A8F8DEB5DB78E47648C7108B86A0938B070D51` | on every job |
| a1, a2 | lab image A | `4DBC18F5FA9C99E2F08623DC4A9B82515A308D7105633E969C8302377A81E694` | only when the VMID does not hold the root for the process of the job |
| b1, b2 | lab image B | `1A872C89358C01CDBFFC73C53CBA6020A0FFF64E35FBB7EDCAAD780FA8D53260` | on every job |

Images A and B have one source: main `8b30b6f6` with `vm-flush-decision.patch`, then two lab changes that
`scripts/lab-tree.ps1` makes. The first change puts back the state catalog files of `8b30b6f6`. The patch adds a
catalog entry, and the KMD refuses the settings of a package with another catalog identity. The second change
replaces the one read of `EnableVmFlushEveryJob` with a constant: `FALSE` in A, `TRUE` in B. So A and B differ only
in that constant. Both images are test-signed. `scratch/kmd-lab-kit/swap-sys.ps1` put each image in the driver store
folder of the installed package, and a restart loaded it. The run order was r0, A, A, B, B, then the release kernel
driver again.

The test of image A (`Bc250VmidRingFlushNeeded` in the patch) follows `amdgpu_vmid_grab_used` of Linux v6.18. A job
gets the flush when its VMID is new to it, when the VMID holds another root or another process, or when the job
names an explicit VMID.

## Results

| Run | Benchmark FPS | 1 % low | 0.1 % low | GPU busy per frame, first 50 s | Node-0 jobs of the run | Jobs with the ring flush |
|---|---|---|---|---|---|---|
| r0 | 56.14 | 37.67 | 14.40 | 16.95 ms | 102862 | 102862 |
| a1 | 56.27 | 36.39 | 19.19 | 16.90 ms | 103454 | 2 |
| a2 | 55.95 | 36.54 | 16.35 | 16.90 ms | 102416 | 1 |
| b1 | 55.95 | 34.99 | 13.89 | 16.84 ms | 102783 | 102783 |
| b2 | 55.37 | 36.50 | 19.27 | 17.09 ms | 102779 | 102779 |

The FPS and the lows come from the frame log of the game (`frames/<run>.csv`, `stats.txt`). The GPU busy per frame
is the mean PresentMon `MsGPUBusy`, with the PresentMon rows aligned to the frame log (`gpu-0-50.txt`). The job
counts are for the whole run from the game start to its exit, about 145 s (`counters.txt`). The first 50 s of the
benchmark ran at 2000 MHz in every run. After that, the KMD thermal policy lowered the clock at 84-85 C (the
`clock:` lines of `cp-<run>.log`).

The skip changes the frame rate and the GPU time per frame by less than the spread between runs. The mean GPU busy
per frame is 16.90 ms with the skip and 16.96 ms with the flush on every job (r0, b1, b2). The two B runs alone
differ by 0.26 ms. No run had a GPU fault, a fence timeout, a TDR or a refused flip.

## Why the skip saves little

The paging stream also invalidates the VMID of the game. Each `DXGK_OPERATION_FLUSH_TLB` becomes invalidations of
all 14 pool VMIDs (`docs/design/gfx-submit-root-serialization.md`, section "FLUSH_TLB"). In these runs VidMm sent
269-290 `FLUSH_TLB` a second, 0.39-0.41 for each gfx job (`counters.txt`). So the flush on every job takes the
invalidations of a job from about 0.4 to about 1.4, not from 0 to 1.

## What a skip also needs

No inspected source says that VidMm follows `DXGK_OPERATION_COPY_PAGE_TABLE_ENTRIES` or a tile unmap with a
`DXGK_OPERATION_FLUSH_TLB`. The WDK 10.0.26100 `d3dkmddi.h` declares the copy and the flush as separate operations.
The tiled resource pages of the driver documentation describe the update without a flush. With the flush on every
job, the next job drops a stale translation. With the skip, a stale translation can stay across the jobs of one
VMID, root and process. So a skip also needs an invalidation after each page table copy. The measured gain does not
pay for that work, and the change is not merged.

## Files

| File | What it is |
|---|---|
| `cp-<run>.log` | the host log of each run: the start checks, the cool-down, the KMD summary lines before and after, the run notes |
| `frames/<run>.csv` | the frame log of the benchmark of each run, as the game wrote it |
| `stats.txt` | the FPS, the lows and the 10 s windows from the frame logs (`scripts/arm-stats.py`) |
| `gpu-0-50.txt`, `gpu-0-70.txt` | the PresentMon GPU busy per frame over the benchmark seconds 0-50 and 0-70 (`scripts/arm-gpu.py`) |
| `pmbins.txt` | the PresentMon values for each 10 s of the benchmark (`scripts/pmbins.py`) |
| `counters.txt` | the KMD counter changes of each run (`scripts/arm-counters.ps1`) |
| `vm-flush-decision.patch` | the source change of images A and B against main `8b30b6f6` |
| `scripts/` | the run, lab tree, collection and analysis scripts, as they ran in the lab workspace |
| `sha256.txt` | the hashes of these files |
