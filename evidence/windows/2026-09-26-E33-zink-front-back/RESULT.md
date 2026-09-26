# WGL front and back buffers require separate swapchain images

PROVENANCE: Mesa MIT; piglit MIT; Waffle BSD-2-Clause.

Unit A, KMD151 and RADV ICD8B5EC055. Diagnostic009 reproduces the upstream
fcc-front-buffer-distraction assertion and records images=1, requested=1,
max_acquires=1. The test clears the back buffer green and front buffer red,
then reads both before presentation. A single swapchain image cannot serve
both independently acquired buffers.

Kopper now requests one image above the surface minimum for Win32 when the
surface maximum allows it. The acquisition bound uses the queried surface
minimum, not the application's requested image count. This follows local
Vulkan-Docs revision01aaacd99480487bf63830959513c5ca8ceb996d,
chapters/VK_KHR_surface/wsi.adoc, swapchain-acquire-forward-progress:
the bound is S-M, where M is the surface capability minimum. No timeout,
assertion, test oracle or advertised Vulkan capability was weakened.

Diagnostic010 records images=2, requested=2, max_acquires=2 and the same
unmodified test passes. Final011 removes diagnostic output and passes again.
Final012 passes upstream fcc-blit-between-clears. Final013 passes all10
same-window resize transitions,7 exact nonzero-area readbacks and10 swaps,
including zero-client restoration. Loaded modules witness final Zink and RADV.
No reboot or GPU reset; baseline Vulkan registration restored after each trial.

Final libgallium_wgl SHA256:
1EC5E9B466FC706FFDCCB03440C6994141D583FEA58327D9D96EDFBA64BD7A01.
Standalone zink-wgl-front-back.patch replays exactly on Mesa05e6c962; its JSON
binds the source and DLL. The earlier zink-wgl-zero-client patch is also present.
This addresses the measured Win32 path with an unconstrained image maximum;
it does not establish correctness of other presentation backends or full piglit.

The full lab package changes only this DLL relative to M503, recorded by
piglit-package-revision002.json. The original manifest alone no longer describes
the live package; use it plus this delta. The previous DLL remains in package006
and the original archive. Runner PATH now includes the package bin directory
for wflinfo. Full quick run003 was launched without filters; its eventual result
is separate evidence. Linux parity and remaining M12.1-M13.1 gates remain open.
