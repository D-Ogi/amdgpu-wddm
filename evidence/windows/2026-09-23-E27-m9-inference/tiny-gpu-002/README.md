# First completed GPU inference, run002

UnitA, boot01:57:26, KMD0.7.56.1, first engine bring-up after reset. Streamed gate/engine steps and gfx IB fence control complete. llama.cpp9564 stories260K16 tokens with ngl99 returns0, but generates repeated s characters after the prompt. This contradicts the Linux/CPU reference. Runtime and output logs show the native RADV WDDM path submitting command buffers; successful process exit is not an inference correctness pass. Display-only restored and UnconfirmedStarts0 at02:14:05.

Next hypothesis: a ggml operation or transfer path not exercised by M8 fails. Build test-backend-ops from the same b9564 source with runtime backend loading, use the unchanged release ggml-vulkan.dll, validate the CPU harness first, then compare individual GPU operators. No performance acceptance is claimed for this incorrect result.
