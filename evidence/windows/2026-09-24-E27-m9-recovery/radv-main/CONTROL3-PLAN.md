# Control3 - negotiate sparse capability with current compiler requirements

Control2 completed six shaders with CPU hash matches; all18submissions completed
(GFX10387/10387) and SDMA23615/23615, no TDR/timeouts. The next MLP pipeline
asserted in ac_nir_fixup_smem_loads_null_prt.c:103: control_bit>=32. A limited
noninvasive stack plus local PDB symbolization identifies this path. No full
process memory dump. Shader compilation had no valid PRT alias control bit.
Current Linux obtains that bit and implements mirrored sparse VA mappings;
old WDDM2 lacks that policy. Advertising sparse on affected hardware is invalid.

Gate sparse support when hardware requires the NULL-PRT SMEM workaround but
no address_prt_wa_control_bit is supplied. Preserve the compiler assertion and
workaround. Ordinary committed buffers remain supported. Full sparse support
requires a later WDDM mapping implementation; this port does not claim it.
After preserving stacks/counters, terminate the exact stalled vkcompute1868;
native exit-1 and worker1 are failures, not successful controls. DWM4448 survives.
Repeat the full8shader and model sequence with a new directory and headless
console; no kernel/DWM/OS reset. Candidate2 SHA256:
DB886B8D53E6BEE89665287AF5EE19A1868E5874868C795F6B11472FBB4A3986
