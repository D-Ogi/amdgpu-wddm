# WGC on the GPU desktop route: the window item of an activated window and the monitor item pass

Date: 2026-11-02, 11:26-11:27Z, unit A, Windows 11 Pro 22631, the tester.35 release installed (kernel driver
40.7.216.107, `bc250kmd.sys` SHA-256 prefix 9D4E0C65), 24 CU, desktop composition on the GPU route. Defect BD-084,
criterion M15.13 of `docs/m15-reconciliation.md`. This run follows
[the capture cells of 2026-10-31](../2026-10-31-bd084-capture-cells/README.md) (fact M935), where the `wgc` cell
stopped at `IGraphicsCaptureItemInterop::CreateForWindow` with 0x80070057.

The question: is that refusal a property of the GPU desktop route, or of the test window?

## The client

`tools/win/capture-share` (`capshare.exe` and its copy `capshare-peer.exe`), SHA-256
`D73BF20927E77B7A5157339B596AE8BEF3072ECBF4EF1D70FC7F6F8855EFCD85`, built from main 63d25d6f with clang-cl. This build
adds two options to the client of 2026-10-31:

- `--producer-window app`: the producer window is an ordinary activated window with a taskbar button, in place of
  the `WS_POPUP` window with `WS_EX_TOPMOST | WS_EX_NOACTIVATE`.
- `--wgc-item monitor`: the consumer captures the monitor item (`CreateForMonitor`) and reads the window's region.

The three runs ran in the logged-on session through a one-shot scheduled task at the Limited level, one after the
other with no restart, each with a 100 s bound. The consumer is D3D11 on the gpu11 route, the producer is a D3D12 flip
swap chain, both at FL 12_1. The producer shows image A, then image B, and the consumer must find each image exactly.

## Results

| Run | Producer window | WGC item | Result | Detail |
| --- | --- | --- | --- | --- |
| 1 | no-activate popup (the 2026-10-31 window) | window | fail | `CreateForWindow` hr 0x80070057 "Could not capture the given window", before any frame |
| 2 | activated app window | window | pass | 4 frames, checks A and B pass |
| 3 | no-activate popup | monitor | pass | 3 frames, checks A and B pass |

`mismatched_frames` counts the frames captured while the producer changed from A to B. The gate is the exact match of
A and of B. The lines are in `lab-wgc-split.txt`, taken from the lab operator's run log. The kit's own summary
lines ("K2 cap FAIL verdict=missing") come from a parser defect of the kit, which did not read the
`CAPSHARE A <ms> VERDICT` prefix. They are left out.

## Reading

WGC reads the frames of a D3D12 producer under GPU desktop composition on 22631, both as a window item and as a
monitor item. The OS refuses the no-activate popup as a window item on this build, and the same window passes as part
of the monitor item, so the refusal of 2026-10-31 belongs to the window style of the test producer, not to the route.
With M935 (Desktop Duplication), both capture paths of M15.13 are measured on the GPU desktop route of the release.
