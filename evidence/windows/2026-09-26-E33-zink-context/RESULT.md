# OpenGL 4.6 core context on Zink/RADV

PROVENANCE: Mesa MIT; Waffle BSD-2-Clause.

Unit A KMD151, Vulkan ICD8B5EC055. Original Zink package001 crashes during
unmodified wflinfo WGL bootstrap: vk_image asserts extent.width >0.
Resource-template instrumentation does not find a zero template. Kopper log
in004 instead proves requested1x1, capabilities0x0, selected swapchain0x0.
Waffle wgl_display_create_window creates a zero-size hidden bootstrap window.
This is not a Vulkan image size to accept or an assertion to disable.

The WGL frontend now uses ordinary offscreen attachments while its Win32
client area is zero. It tracks that allocation mode and invalidates attachments
on a minimized-state transition even if retained dimensions match. Nonzero
windows continue through Kopper. Only two WGL source files change. No Waffle
change, GL version override, or capability relaxation was used.

Diagnostic005 then creates an actual320x240 swapchain. Final006 removes both
diagnostic log additions, completes exit0, and reports OpenGL4.6 Core, GLSL4.60,
Zink Vulkan1.4 on AMD BC-250/RADV, Mesa26.3.0-devel05e6c962. Refreshed process
module enumeration witnesses package006 libgallium_wgl and intended8B5EC055 ICD.
Early probes cached the .NET Modules property and are not module-witness proof.
Final libgallium SHA256DFC11A1421C4DDDB95AB348B1CCFACAC2FD8459E1E80E0F81D4D4C89DB239D24.

The final patch replays exactly on upstream05e6c962 with LF normalization;
experiments/E33-m12-applications/zink-wgl-zero-client.json records the binding.
Package metadata retains the historical base build plus final file hashes.
The diagnostic003 accidentally repeated002 after instrumentation preparation
failed; its raw files remain in scratch and add no independent evidence.

Health retains GPU generation560773206/epoch5. Registry restored after each
trial; no reset/reboot. App-local OpenGL only, no system OpenGL registration.
This establishes context creation, not GL conformance or image correctness.
Same-window zero/nonzero restoration and presentation need regression coverage;
full45037-case piglit quick and matched Linux comparison are still pending.
Full M12/M12.1 remains open, including Vulkan CTS, sparse and OpenCL acceptance.
