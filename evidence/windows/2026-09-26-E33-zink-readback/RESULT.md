# Zink same-window resize/readback and first piglit control

PROVENANCE: Mesa MIT; Waffle BSD-2-Clause; piglit MIT.

Unit A KMD151, Zink libgalliumDFC11A14 from M501, Vulkan ICD8B5EC055.
Control007 starts with a zero-area Win32 window, bootstraps a legacy context,
then creates an actual4.6 core context on the same window. Ten subsequent
sizes include320x240,0x0,1x1,640x360,0x0,640x360,1x1,0x0,1x1,320x240.
At all seven nonzero stages, clear/readback RGB bytes match the expected
endpoint colors exactly. All ten SwapBuffers calls return success; GL reports
no error. Zero-area readback is deliberately not claimed. Same retained-size
restoration is included. Exit0 and intended Zink/ICD modules are witnessed.
This proves backbuffer readback and API presentation completion, not external
monitor pixel equivalence or complex shader correctness.

Control008 runs upstream piglit0cc014230c0701d7a61bf240009ddd0711462d4e,
fast_color_clear@fcc-blit-between-clears, unchanged executable, -auto and
PIGLIT_PLATFORM=wgl. It verifies a color clear after an intervening blit;
upstream output is PIGLIT result pass, exit0. Required stack modules witnessed.
This is one control from45037 quick cases, not completion of the full profile.

No reset/reboot; GPU session retained and Vulkan registration restored after
both runs. App-local OpenGL packages007/008 share the unchanged final M501 DLL.
No system OpenGL registration. Linux parity and all remaining M12 gates remain
open. Next: portable official piglit runner/dependencies and full quick profile,
with per-case timeout, failure evidence and matched Linux results.
