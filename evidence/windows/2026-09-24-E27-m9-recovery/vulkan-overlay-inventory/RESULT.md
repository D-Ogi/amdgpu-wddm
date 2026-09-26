# M449 - cached Vulkan inventory in the lab overlay

Unit A, 2026-09-24. The overlay was replaced at 21:36:03 UTC; Windows boot,
DWM PID 2040 and KMD 0.7.136.1 were retained. New monitor PID 1296, SHA256
`78EBFD9C844E7B27B50E0788D04F46C1B9021A8DA2EFA24C3FF5B43CB2D2A0E1`.
The same typed control DLL was verified loaded. Previous overlay is backed up
at `C:\BC250\mon\inventory-deploy-01\before`. No driver deployment or OS restart.

The provider reads cached JSON every five seconds, never starts Vulkan. It shows
capture age, device/API/driver, observed ICD, extensions/layers, formats and four
advertised features, separately from DWM's CPU llvmpipe renderer. Missing fields
are unknown, not unsupported. Full report path and partial/error status remain
visible. Current source supports additional columns without changing font/input.

## Actual capture limitation

The single `vulkaninfo 1.4.335.0 --text --show-formats` capture reached its 40 s
limit and its owned process tree was terminated. Output is truncated during
format group 22. No further Vulkan workload was run. The conhost task returned
zero despite the worker's failure; capture.json, not the wrapper status, is the
result. The parent script now checks that status; collector telemetry is saved
before its timeout branch. These collector changes have not been retried.

The loader explicitly reports using `C:\BC250\m8\vulkan_radeon.dll`, SHA256
`71CB633DD40BB3FF54E4989B077726C27C8948FB1F7286829FF9B6AEA0934AA8`.
It ignored the requested RADV-main override in this elevated capture. The cause
has not been established. This is the older system ICD, not the compute launcher
ICD. The panel accurately reports Mesa 26.2.0-devel git-801c9763c6, API 1.4.348,
16 instance/211 device extensions and zero enumerated layers. Format counts remain
null because the report is incomplete. Sparse=true is this ICD's declaration;
it is neither proof of working sparse memory nor the newer ICD's capability.

## Validation

- Full monitor build passed with warnings as errors; 34 provider/cache/layout
  checks and four existing Python checks passed. Actual-data replay: 35 checks.
- Root parser checks passed on the actual partial report: loaded/requested ICD
  separation, exact observed counts, unknown truncated formats, missing report
  and conflicting feature values. No UUID exported to the normalized cache.
- API after replacement exposes the actual Vulkan panel and expected control DLL.
- Actual 1920x1200 desktop screenshot inspected: complete overlay and Vulkan panel
  visible in one column, taskbar unobscured, DWM pipeline remains separately shown.
  Screenshots and full pre/post system panels stay in private scratch (addresses).
  A still screenshot does not prove mouse smoothness or frame pacing.
- After capture: SSH available, DWM/monitor responding, no vulkaninfo process left.
  This is not a GPU-reset or post-timeout compute acceptance result. Candidate139
  remains built but not staged/deployed; subsequent GPU tests require recovery
  and their own positive control.

Raw report redactions: UUID/LUID identifier lines only. Raw originals, screenshot
and deployment backup remain private. `capture-executed.ps1` is the original
worker; `source/capture-vulkan-inventory.ps1` is the subsequent telemetry fix.

The owner-proposed Open Test Pack was also saved in
`docs/design/bc250-open-test-pack.md` and linked from the roadmap. No vkcube,
CTS or benchmark was run as part of this overlay change.
