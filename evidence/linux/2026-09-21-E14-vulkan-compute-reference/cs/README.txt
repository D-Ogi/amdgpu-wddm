E14, the command stream: what RADV (Mesa 26.1.6) submits for one compute dispatch (vkcompute --only fill_g1 --runs 1),
captured with RADV_DEBUG=dumpibs (experiments/E14-vulkan-compute-reference/run-cs.sh), same session, same boot.

  ib-chained.raw/.txt      RADV's own dump (ac_debug) as submitted: a 24-dword preamble IB that points at a 168-dword
                           state IB, and the 64-dword main IB with the dispatch. Both on the GFX ring.
  ib-unchained.raw/.txt    the same with noibchaining: the state IB's body inline
  ib-allbos.raw/.txt       the same with allbos
  decoded-*.txt            decode_cs.py's listing (opcode names from Mesa's sid.h or the kernel's nvd.h, register names
                           through tools/regcalc); -kernel-names: the nvd.h variant
  ring_gfx_0.0.0.txt, ring_comp_1.0.0.txt, decoded-ring_*.txt   the rings afterwards and their decode
  run-*.out                vkcompute's output of each run (the hash matches in all three)
No BO list: RADV prints none, and amdgpu_vm_info reads empty on this kernel; the trace events of compute/ carry it.
Copied through redact.py, nothing edited by hand.
