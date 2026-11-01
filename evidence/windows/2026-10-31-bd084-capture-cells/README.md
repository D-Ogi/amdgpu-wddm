# Capture cells on the GPU desktop route: Desktop Duplication passes, the WGC window item is refused

Date: 2026-10-31, unit A, Windows 11 Pro 22631, the tester.35 release installed (kernel driver 40.7.216.107,
`bc250kmd.sys` SHA-256 prefix 9D4E0C65), 24 CU, desktop composition on the GPU route. Defect BD-084, criterion
M15.13 of `docs/m15-reconciliation.md`.

The question: do DXGI Desktop Duplication and Windows.Graphics.Capture (WGC) read a window that a D3D12 swap chain
of our driver presents, under GPU desktop composition?

## The client

`tools/win/capture-share` (`capshare.exe` and its copy `capshare-peer.exe`), SHA-256
`FA186A67752EAF9903254BB840943A8D2648A81AC48C35E9BB8D2D878F8C4162`, built from main at 21:42Z with
`tools/win/capture-share/build.ps1`. The parent process is the D3D11 consumer. The peer process is the producer:
a 256x256 `WS_POPUP` window with `WS_EX_TOPMOST | WS_EX_NOACTIVATE` at 48,48, drawn by a D3D12 flip swap chain. The
producer shows image A, then image B, and the consumer must find each image exactly in the window's region.

The cells show a window and read the screen, so they ran in the logged-on session: `lab-capshare-run.ps1` started
through a one-shot scheduled task with an interactive principal at the Limited level, each with a 100 s bound. The
copy of `lab-capshare-run.ps1` here is the later form with `-Extra` and the client hash line; the runs of this
directory used the same script without them.

## Results

| Run | Cell | Result | Detail |
|---|---|---|---|
| `lab-dda` 21:46:27Z | `dda` | pass | Consumer D3D11 on the gpu11 route at FL 12_1, producer D3D12 at FL 12_1. 5 frames, image A and image B both found, decided at 938 ms. `dda_system_memory=0`, no access lost, no wait timeout. |
| `lab-wgc` 21:46:53Z | `wgc` | fail | Stage `wgc-init`: `IGraphicsCaptureItemInterop::CreateForWindow` returned `0x80070057` ("Could not capture the given window") before any frame. The producer window was up and READY with 0 occluders. |
| `devpc-wgc` 21:50Z | `wgc` | pass | Positive control on a development PC: Windows 11 26200, another vendor's driver, the same client bytes and the same window style. WGC item 256x256, 2 frames. |

The `--stderr` file of the lab `wgc` run was empty and is not kept.

## What the results show

- Desktop Duplication reads a window of our D3D12 swap chain under GPU desktop composition on 22631. The duplicated
  desktop image is in video memory.
- The client's window is capturable through the WGC window item on 26200 with another driver. On unit A the same
  call refuses the window. The two runs differ in the OS build and in the driver, so these runs do not separate the
  cause.

## Next runs

`capshare.exe` from the commit that adds `--producer-window` and `--wgc-item` splits the `wgc` cell in one session,
with no restart (`tools/win/capture-share/README.md`, section Status):

1. `wgc` as above: repeats the failure.
2. `wgc --producer-window app`: an activated taskbar window. A pass here and a failure in run 1 put the cause on the
   window rule of the OS build.
3. `wgc --wgc-item monitor`: the monitor item, read at the window's place. A pass shows that the compositor's capture
   path works on this route.

## Files

| File | What it is |
|---|---|
| `lab-dda.txt`, `lab-dda.json` | The `dda` run on unit A: the log of both processes and the verdict. |
| `lab-wgc.txt`, `lab-wgc.json` | The `wgc` run on unit A. |
| `devpc-wgc.txt`, `devpc-wgc.json` | The `wgc` positive control on the development PC. |
| `lab-capshare-run.ps1` | The lab-side script that runs one cell in the interactive session. |
