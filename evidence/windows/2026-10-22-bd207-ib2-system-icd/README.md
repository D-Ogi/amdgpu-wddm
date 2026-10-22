# The system Vulkan ICD with IB2 calls: Cyberpunk 2077 on the Vulkan route and Quake II RTX (2026-10-22)

Unit A, Windows 11 22631, 40 CU, the KMD DPM with its 2000 MHz ceiling. The installed package is the tester.32
release candidate (0.7.216.104-tester.32, `oem3.inf` 40.7.216.104, kernel driver `CDE214D4`). The package stayed
installed for all runs. A run with another Vulkan ICD gave that ICD to the game process only, through
`VK_DRIVER_FILES` (`pt-run.ps1 -SetEnv`).

## The Vulkan ICDs

| Name | Source (mesa-wddm) | SHA256 | A submission of several IBs |
|---|---|---|---|
| installed | `ec70ed15`, the system ICD of the installed package (signed copy) | `01B584A8D3E553866C7BF5BCF745177EE2E53C8153E8820669601C4603E5B7F8` | the winsys copies every IB into one gather IB1 |
| fix | `4dd76d77` on `29d95fd7` (branch fix/bd207-ib2-system) | `B8F66C0277586A55D65B95680288E121EAF2BA6D175378C0DCADD0CC21F5B2AA` | the gather IB1 calls each IB as an IB2 |
| r2 | the tree of `8f4f33a6` (the same branch, which also refuses an IB outside its BO on both submit paths) | `F475658C37BB4103644D74F04D0B0227AC244E1A04E50C78D2FFF7B0001E1A05` | as fix |
| tester.32 | `055dbb9f` on release/tester32-radv-system: `ec70ed15` with the two commits of the branch | `81438ED06EDB2A3DE3D753C8C15797E11D17ACADB91D46E9B43DFEA1C76822EF` | as fix |

All four are release-flavour builds. `gate/icd-gitstr.txt` shows the commit string in the installed DLL and in the
tester.32 DLL. A build of the committed `8f4f33a6` differs from r2 only in the link time stamp, the commit string and
the PDB identity (`route/bytediff-r2-r2c-release.txt`). In code, the tester.32 ICD differs from r2 only in the destroy
path of a swapchain image (`84c1b4fb`) and in comments.

## Cyberpunk 2077 on the Vulkan route

The game is Cyberpunk 2077 2.31 (GOG) with its built-in benchmark at 1920x1200 in exclusive full screen. The settings
are the same as in the ring VM flush evidence of the same day: every advanced option at its High preset value,
anisotropy 8x, crowd density Medium. Ray tracing, the upscaler and frame generation are off. For the Vulkan route,
`scripts/applocal-dll.ps1` puts the vkd3d-proton 3.0.1 `d3d12.dll` and `d3d12core.dll` and the DXVK `dxgi.dll` in the
game folder. The game then runs on the system Vulkan ICD and not on our D3D12 driver. After the runs, the same script
removed these DLLs and the pipeline cache files of vkd3d-proton from the game folder (`route/restore*.txt`,
`gate/restore-cp.txt`).

Each run started at a Tctl of 55 C or less, with the lab overlay hidden. In the runs with IB2 calls, the game owned
the foreground in 98-99 % of the 2 s samples, and the camera frames show the game in full screen on the monitor.
PresentMon 2.6.0 recorded the presents. The camera frames are not kept here.

### The installed ICD loses the device

Run vk0 used the installed ICD. The winsys refused one submission of 1101808 dwords before the kernel. The IB_SIZE
field of the INDIRECT_BUFFER packet holds at most 1048575 dwords. The game log shows
`bc250: gather exceeds IB_SIZE field: 1101808 dwords` and then `native submit failed NTSTATUS=0xC000000D`
(`vk0/vk0-stderr-30120.txt`). vkd3d-proton then marked the device lost (`VK_ERROR_DEVICE_LOST`). The KMD counted no
timeout, no refused job and no flip fault (`vk0/cp-vk0.log`). The game stopped with an assert of its 120 s watchdog
(`vk0/crash/`) and the exit code 0x80000003 (`vk0/vk0-files.txt`). In this run the game owned the foreground in 12 of
73 samples only (`vk0/pt-vk0.log`).

### The ICDs with IB2 calls keep the device

| Run | ICD | First run of its ICD build | Benchmark FPS | 1 % low | 0.1 % low | 10 s windows, FPS |
|---|---|---|---|---|---|---|
| w0 | fix | yes | 50.44 | 29.70 | 16.46 | 53.29 56.09 63.50 56.58 46.10 34.12 34.37 |
| m1 | fix | no | 52.79 | 40.62 | 22.42 | 47.06 49.06 60.94 56.22 49.75 52.85 54.81 |
| m2 | fix | no | 51.90 | 38.55 | 17.32 | 46.60 48.28 60.10 54.56 48.58 52.04 54.76 |
| m3 | r2 | yes | 52.17 | 33.91 | 16.65 | 53.14 56.21 64.13 57.17 44.28 40.87 45.93 |
| tester.32 | tester.32 | yes | 51.60 | 31.69 | 15.34 | 50.38 54.34 62.31 55.93 46.88 42.58 44.93 |

The numbers come from the frame log of the game (`*/frames/*.csv`, `stats.txt`, `scripts/frame-stats.py`). No run
lost its device. The KMD counted no timeout, no refused job and no flip fault (the `after:` lines of
`route/cp-*.log` and `gate/trial-cp-vk-rel32.log`). In the tester.32 run, 19095 presents used independent flip and 534
were composed (`gate/pt-cp-vk-rel32.log`).

The runs make two groups. The first run of an ICD build (w0, m3, tester.32) is fast in the first 40 s of the
benchmark and slow at its end. m1 and m2 ran after w0 with the same build. They are slower in the first 30 s and
faster at the end. In the first-run group, the KMD thermal policy lowered the clock about 128 s after the start, at
85.75-86.75 C. In w0 the clock went down to 900 MHz (the `clock:` lines of `route/pt-*.log` and
`gate/pt-cp-vk-rel32.log`). The tester.32 run and m3 have a similar clock course in these 10 s samples.

Inside the first-run group, the tester.32 ICD (51.60 FPS) is between fix (50.44) and r2 (52.17). In the first 40 s of
the benchmark it is 0.7-2.9 FPS below both. One run for each build does not show if this difference comes from the
build or from the spread between runs. `gate/pmbins-cp-vk-rel32.txt` and `route/pmbins-vk.txt` give the PresentMon
GPU busy time per frame for each 10 s. In the first 40 s, it is 14.80-17.51 ms in the tester.32 run and 14.65-17.59
ms in m3.

## Quake II RTX

Quake II RTX ran its timedemo (631 frames) at 1280x720 in a window, with no dynamic resolution, no FSR and no VSync.
`ray_tracing_api pipeline` uses `VK_KHR_ray_tracing_pipeline`, and `ray_tracing_api query` uses `VK_KHR_ray_query`.
The four runs ran on one boot. Each run started at a Tctl of 55.2 C or less.

| Ray tracing API | tester.32 ICD, FPS | installed ICD, FPS |
|---|---|---|
| pipeline | 90.03 | 91.38 |
| query | 96.16 | 96.06 |

The clock sample 12 s after the start shows 1000 MHz in the pipeline run of the tester.32 ICD and 2000 MHz in the
pipeline run of the installed ICD. The samples come every 10 s and the timedemo takes 7 s, so the samples do not
show the clock during the timedemo. In the runs of the tester.32 ICD, the VK_LOADER_DEBUG output names the staged
ICD (`gate/icd-check-q2p-rel32.txt`, the `loader:` lines of `gate/pt-q2q-rel32.log`).

The photo configuration of the lab kit plays the demo for 450 frames and pauses it. Then the photo mode of the game
adds path tracing samples to the paused view for a fixed number of frames, and the game writes one screenshot. The
accumulation was not complete at the screenshot. Two runs used each ICD. `scripts/imgcmp.py` compared the four
screenshots in pairs (`gate/imgcmp.txt`):

| Pair | PSNR, whole frame | PSNR, box (0,0)-(520,700) |
|---|---|---|
| tester.32 against tester.32 | 29.26 dB | 36.68 dB |
| installed against installed | 29.09 dB | 38.19 dB |
| tester.32 against installed, four pairs | 28.08-29.34 dB | 36.36-38.13 dB |

The path tracing samples are random, so two runs with one ICD do not give the same image. The pairs of different
ICDs differ from each other no more than the pairs of one ICD. The mean luma in the box is 47.03-47.29 in all four
screenshots. The box holds the left part of the view, with the crates and the floor. The screenshots are not kept
here.

## Result

With the IB2 calls, the gather IB1 holds a call of four dwords for each IB. So the IB_SIZE field limits each IB, not
the sum of the IBs of a submission. The winsys still copies a stream that calls an IB2 itself. The tester.32 ICD runs
Cyberpunk 2077 on the Vulkan route without the device loss. In Quake II RTX, its frame rate is within 1.5 % of the
installed ICD in both ray tracing APIs, and its picture differs no more than two runs of one ICD.

## Files

| File | What it is |
|---|---|
| `vk0/` | the run with the installed ICD: the host logs, the file list and an extract of the game log, the crash report of the game |
| `route/` | the runs w0, m1, m2 (fix) and m3 (r2): the host logs, the stage and restore records, the frame logs, the PresentMon bins |
| `gate/` | the runs of the tester.32 ICD and the installed ICD: Cyberpunk 2077, the Quake II RTX timedemos and photos, the image comparison |
| `stats.txt` | the FPS, the lows and the 10 s windows of the five Cyberpunk 2077 runs with IB2 calls |
| `scripts/` | the run, staging, check and analysis scripts, as they ran in the lab workspace |
