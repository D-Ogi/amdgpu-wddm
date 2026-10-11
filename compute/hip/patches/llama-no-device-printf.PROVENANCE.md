PROVENANCE: llama.cpp, MIT.

Optional consumer patch for the retained step3 source snapshot. Define BC250_HIP_NO_DEVICE_PRINTF=1 to omit only the diagnostic text in the existing HIP no_device_code unsupported-specialization path. The following __trap remains unchanged. General device printf remains unsupported and refused at link time.

Original common.cuh SHA256: 12d3be71f050e9101ba5e720974f5f3694e9f2ad588d9f499d163d905f30bbc1
Patched common.cuh SHA256: f629b2ccefdc29799dd0b538d6da32f3914cb7ecbb8f74a3491c83266968ff2e

Build recipe: apply the patch to a separate llama.cpp checkout with the exact original common.cuh hash above. Assemble the ROCm-shaped root using compute/hip/tools/make-rocm-root.py and the reviewed HIP headers, existing device bitcode and import libraries. Configure the same backend options as the pinned consumer build, adding -DCMAKE_CXX_FLAGS=-DBC250_HIP_NO_DEVICE_PRINTF=1, then build target ggml-hip with at most four jobs. The macro is consumed only by this patched diagnostic. It does not replace printf elsewhere. Record the complete resulting DLL and code-object hashes. This adaptation is not an implementation of unsupported MMA specializations.

Upstream base: llama.cpp b86d2f07542b29ab099aed34fd6b6d1b2fd4b81c. The input snapshot already adds gfx1013 to RDNA1 and guards the RDNA1 dp4a assembly with BC250_HIP_NO_ASM_DP4A. This patch is additional to those two existing consumer changes.
