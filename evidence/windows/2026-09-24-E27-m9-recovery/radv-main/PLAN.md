# Current Mesa RADV/WDDM2 migration controls

Unit A, 2026-09-24. Retain KMD127, boot11:44:14 and M412 D3D desktop.
Hypothesis: porting the existing WDDM2 and BC250 integration onto Mesa main
f333dd6d1c85297ac41773eaeb9b02f16acf1919 preserves compute content and model outputs.
Upstream Collabora lfrb/wddm2 remains801c9763c6043f0de8408e905a5324eea06d81d7;
common ancestorb860e0132f97dfe0b30c63b51a78ae6beae99428. PROVENANCE: Mesa, MIT.

Separate source/build directories; current upstream AMD compiler/core retained.
Compiler ACO, LLVM disabled for this RADV build. LLVM23.1.2 remains the CPU
llvmpipe compiler for the independent D3D desktop.
First candidate DLL SHA256
749EDF6195161A3E601AFFE508A94E1657971D0846E97BE3380B412E2265B31A.
Native MSVC build succeeds in build-radv-main-5.log after API adjustments.

Select a separate radv-main-icd1 JSON only in the test process environment.
Keep default Vulkan registration and loaded DWM UMD unchanged. Check STOP,
1000MHz/820mV, installed KMD hash/version and temperature below85C. Preserve
pre/post counters and loader witnesses. No device reload, OS or AC reset planned.

Run existing8M8 CPU-hash shader controls (3runs each), then b9564 stories15M96
and TinyLlama64 deterministic GPU tokens, compare full text against E14 Linux
references and check full-layer offload. These are expected matches if the
port is correct. Any mismatch/native failure ends this control sequence and
is retained for diagnosis. The worker completion marker is authoritative;
scheduled-task Ready alone is not completion, after M413 wrapper anomaly.
The bounded observer allows300seconds; inspect unfinished work before retry.

After content success, run same M413 pp512/tg128,r3,t6,ngl99 benchmarks with
the current desktop and model/application hashes unchanged. Compare samples,
not just version labels. GPU residency readback may supplement content checks.
No Vulkan conformance or Windows-over-Linux advantage claimed from these controls.
