E14, 2026-09-21, unit A under the Alpine diagnostic stick (boot 4 of E13: warm restart out of Windows, stick mode readonly,
amdgpu loaded by session.sh load), kernel 6.18.52-0-lts, Mesa 26.1.6 (RADV), llama.cpp build 9564, all installed into RAM.
Procedure and results: experiments/E14-vulkan-compute-reference/README.md.

  compute/             vkcompute: vkcompute.txt (5 runs, GPU shared with the llama work), vkcompute-timing.txt (21 runs,
                       quiet GPU), negctl/ (a deliberately wrong shader must fail, and does), radv-debug/ and
                       radv-shaders.txt (RADV's dumps: NIR, SPIR-V, GFX10 machine code), amdgpu-events-fill.txt and
                       fill-traced.txt (amdgpu's events around one fill), spv/ (the SPIR-V binaries, unchanged)
  llama/               run.txt (the whole run), *.out (generated text), *.err (llama.cpp's log), bench*.txt,
                       amdgpu-events-llama260k.txt (amdgpu's events around one 16-token GPU run)
  vulkaninfo.txt, vulkaninfo-summary.txt   RADV on this unit: "AMD BC-250 (RADV GFX1013)", Vulkan 1.4
  radv-info.txt        RADV_DEBUG=info vulkaninfo --summary: the radeon_info Mesa derived here
  llama-install.txt    package versions and the models' sha256
Copied through experiments/E13-linux-reference-2/redact.py (it also replaced the UUID-like values of vulkaninfo; the
SPIR-V files were copied byte for byte). Nothing edited by hand.
