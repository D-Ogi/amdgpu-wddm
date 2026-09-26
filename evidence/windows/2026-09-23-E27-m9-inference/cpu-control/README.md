# M9 CPU harness control

UnitA, Windows, llama.cpp9564 (3b3da01dc), official Vulkan archive; no ICD exposed and ngl0. Both public model SHA256 values equal E14. cpu-control.cmd runs E14 prompts/options with stdinNUL and native stdout/stderr file redirection. Both processes returned0. stories260K and stories15M output matches Linux CPU byte-for-byte after CRLF-to-LF conversion; raw bytes differ only in line endings. comparison.json records both hashes. This is a CPU harness control, not GPU inference or M9 completion.

Earlier Start-Process-based invocation produced incomplete output for stories260K, without the final performance log. Waiting for output drain did not change it. Switching to explicit cmd redirection/stdinNUL resolves the mismatch. The precise cause of that invocation difference is not isolated; cpu-control.cmd is the canonical harness for subsequent comparisons.
