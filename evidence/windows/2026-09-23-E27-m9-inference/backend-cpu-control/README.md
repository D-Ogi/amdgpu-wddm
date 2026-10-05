# Operator test harness CPU control

Official llama.cpp b9564 source archive; build-backend-test.cmd configures MSVC Release, GGML_BACKEND_DL on, native tuning off, OpenMP off, Vulkan compilation off. The original release ggml-vulkan.dll is copied unchanged next to the generated test/core DLLs for subsequent dynamic loading. No claim of GPU plugin compatibility is made until a Vulkan device is successfully exercised.

On unitA in display-only mode, with no Vulkan ICD exposed, test-backend-ops test -b CPU -o CPY -p type_src=f32,type_dst=f32 executes64 copying/layout cases;64/64 pass, exit0. This checks the CPU-side harness and selected cases, not the GPU. The planned GPU run additionally checks that Vulkan0 is named and that the executed test count is nonzero, preventing all-skipped output from passing. Built artifacts remain outside the repository; manifest records hashes.
