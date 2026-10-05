# Vulkan overlay source validation

Source-only, 2026-09-24. No lab actions, Vulkan processes, windows, resident
processes or hotkeys were started by this sub-agent.

- New cached VulkanInventoryProvider and typed schema 1 model. Refresh reads a
  JSON file every 5 seconds, only reparses changed files. Explicit capture age,
  partial/error/unsupported states, unknown features/counts, and changed KMD.
- Requested manifest identity and observed library identity remain separate.
  IcdVerified plus IcdLibraryPath means a loader-observed library, never proof
  that the requested override was honored. Full details stay in the adjacent report.
- Program registers the provider beside the unchanged graphics pipeline provider.
- OverlayForm uses measured whole-panel column flow through OverlayLayout;
  existing font sizes, input handling and hotkeys remain unchanged.
- README explains schema semantics, synthetic fixtures and cache path override.
- build.ps1 runs the headless provider/layout regression before compilation.

Validation:

1. test-vulkan-inventory.ps1: 34 checks passed. Happy path, partial timeout with
   a different observed ICD, unknown/null features and counts, error/unsupported,
   stale KMD, schema rejection, missing/deleted/invalid/replaced cache.
2. Measured fixture panel: 203 logical pixels high. Representative existing
   panels, inventory, nine log lines and footer: 2 columns, 920x1047 logical
   pixels, all within 1920x1200 at 96 dpi. This is host geometry, not a lab screenshot.
3. Full .NET Framework C# 7.3 /warnaserror+ build passed. Existing Python checks:
   4 passed. Output scratch/build/vulkan-overlay/monitor/bc250mon.exe (77824 bytes).
   Control DLL supplied from scratch/build/smu136-client/bc250control.dll.

No deployment or hardware capability claim. The root agent owns actual Vulkan
capture/normalization, artifacts/hashes, overlay restart and screenshot validation.
The synthetic fixture is not evidence of any device capability. Extreme remote
panels or working areas too small for one full panel can still exceed the available
height after all available columns are used; current target geometry must be
checked with the actual screenshot.
Actual partial capture replay completed against
scratch/m9/vulkan-inventory-01/vulkan-inventory.json and overlay-before.json.
35 checks passed including actual data geometry: one column, 460x1118 logical
pixels at 1920x1200/96 dpi; Vulkan panel 293 pixels. Actual partial panel JSON and
explicitly labelled headless render are in actual/. This is not a lab screenshot.
Observed old system ICD DLL shown independently from requested override. Selected
features use the label Advertised; Scope says ICD declarations, not a rendering test.
Final rebuilt executable SHA256: 78EBFD9C844E7B27B50E0788D04F46C1B9021A8DA2EFA24C3FF5B43CB2D2A0E1.
Packed KMD milestone/revision is compared with DriverVer before claiming a change;
the CLI documents the high/low 16-bit split. The DDI does not expose the fourth
DriverVer package component. Parent owns deploy.
