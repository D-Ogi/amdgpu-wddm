# Pending release-notes lines: Vulkan applications present on the GPU (Vulkan ICD)

This file is not a release. It holds the tester-facing lines for the next tester release notes that ship the
Vulkan driver of branch `amdgpu-wddm/b26-vk-wsi-dxgi`, which is the shipped system Vulkan ICD line plus the
route.
The release step copies the lines into
`docs/testing/release-notes/<version>-tester.N.md` and deletes this file. The design is
`docs/design/vulkan-wsi-dxgi.md`.

## Changed

- A Vulkan application or game now shows its pictures through the GPU. Before this version, the driver copied
  each picture on the processor into the window. Now the GPU copies it, and the desktop shows it the same way
  as the picture of a DirectX 12 game. A fullscreen Vulkan game can therefore use the same direct path to the
  screen as a DirectX 12 game.

## Known behaviour

- If the new path cannot start for an application, the driver uses the old path for it, and the driver log
  says why. The application keeps its picture.
- The first window of a Vulkan application can open a little later, because the driver starts a second,
  DirectX 12 part for the pictures.
- The driver now offers Vulkan applications 10-bit colour, and HDR when the screen is in HDR mode. **We have not
  tested these yet.** No test picture of this release used them. An application that asks for 10-bit colour or HDR
  can therefore fail to open its window. If that happens, use the old path below and send us the support report.
  An application that asks for the usual 8-bit colour is not affected.
- A Vulkan application that needs three pictures in its window queue cannot use the new path. The driver offers
  two. Use the old path for such an application.
- A 32-bit Vulkan application keeps the old path in this release.

## If something goes wrong

- If a Vulkan application shows a wrong picture, a black window or stops, use the old path:
  - For one application, set the environment variable `AMDGPU_WDDM_VK_WSI` to `gdi` before you start it.
  - For all applications, set the text value `WsiRoute` to `gdi` under the registry key
    `HKEY_LOCAL_MACHINE\SOFTWARE\amdgpu-wddm\Vulkan`.
  Then open a GitHub issue with the support report. The driver log in the report names the path each window used.
