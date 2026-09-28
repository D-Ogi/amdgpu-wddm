# M762: emulated ray query and ACO path comparison

Same exact9496 ICD, CTS triangle flow case and direct-library harness as M760. rt-emulate001 with RADV_EXPERIMENTAL=emulate_rt passes1/1 in3.541s; supervisor20.794s,66.9C. This is software intersection executed by GPU shaders, not CPU rendering.

Fresh compiler-output runs disable the disk shader cache (MESA_SHADER_CACHE_DISABLE=true) and use RADV_DEBUG=info,shaders:

- rt-isa001, default: Pass;6.540s case/23.608s supervisor,67C. The final ACO dump contains two image_bvh64_intersect_ray operations.
- rt-isa002, emulate_rt: Pass;10.259s case/27.242s supervisor,67C. The nonempty final ACO dump contains zero such operations.

Despite the run names and output heading disasm, these are not binary disassemblies. Both explicitly report LLVM unavailable and fall back to aco_print_program after lowering to hardware instructions (aco_interface.cpp:get_disasm_string). The preserved excerpts begin with that disclaimer; raw logs are hashed in receipt.json and remain in scratch/m15. This establishes distinct compiled ACO paths alongside passing content tests. A witness decoded from emitted machine bytes remains open; shader dump/cache timings are not RT performance measurements.

All three runs observe the exact candidate module, close their Jobs, and preserve CPU171 boot/DWM, driver hashes/registrations, generation, epoch and health flags. No deployment or restart. Every process has20s and its supervisor170s bounds. QPA files are original; compiler excerpts omit surrounding allocation-handle diagnostics and unrelated adapter fields.

These results cover one triangle query and its two intersection paths, not full CTS, RT pipeline emulation, FL12_1/native D3D12 or Witcher 3 acceptance.
