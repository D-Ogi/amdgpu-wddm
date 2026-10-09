# Pending release-notes lines: the GPU path for Vulkan windows, as a switch (Vulkan ICD)

This file is not a release. It holds the tester-facing lines for the next tester release notes that ship the
Vulkan driver of branch `amdgpu-wddm/b27-vk-wsi-dxgi`, which is the shipped system Vulkan ICD line plus the
route.
The release step copies the lines into
`docs/testing/release-notes/<version>-tester.N.md` and deletes this file. The design is
`docs/design/vulkan-wsi-dxgi.md`.

**State of the work, for the writer of the release notes, not for the tester:** the new path has never shown
a picture on the lab machine. In the first trial every Vulkan program that used it stopped before its first
picture (BD-105). The path is therefore off unless somebody turns it on, and these lines say so. They must not
promise the direct screen path for Vulkan games: that promise waits for a lab run that shows a picture.

## Changed

- Nothing changes for a Vulkan application or game in this release. Vulkan windows keep the path they had.

## New, and off by default

- The driver now has a second way to show the pictures of a Vulkan program: the GPU copies each picture into
  the window, as it does for a DirectX 12 game, instead of the processor. It is a test path. It is **off**, and
  a program uses it only if you turn it on for that program:
  - Set the environment variable `AMDGPU_WDDM_VK_WSI` to `dxgi` before you start the program.
  - To turn it on for every program, set the text value `WsiRoute` to `dxgi` under the registry key
    `HKEY_LOCAL_MACHINE\SOFTWARE\amdgpu-wddm\Vulkan`. Delete the value, or set it to `gdi`, to go back.
- **What to expect if you turn it on**: in our own test run, the programs that used this path showed no
  picture at all and had to be closed. The driver now watches the two waits it owns on this path: if the
  program stops in one of them, the driver gives up after two seconds and the next window takes the old path,
  so the program keeps a usable window. If it stops anywhere else on the new path, it still has to be closed.
  Either way the driver log names the step the new path stopped at. Please send that log with a GitHub issue if
  you try it. We would like to know what your machine does here.
- If you do not set either value, nothing of this reaches your programs.

## If something goes wrong

- A Vulkan program that shows a wrong picture, a black window or stops: delete `AMDGPU_WDDM_VK_WSI` and the
  `WsiRoute` value, or set them to `gdi`, and start the program again. That is the path of every earlier
  release. Then open a GitHub issue with the support report. The driver log in the report names the path each
  window used.
