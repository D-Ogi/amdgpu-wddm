# Corrected ICD selection, full WDDM and inference
2026-09-23, unit A, KMD0.7.98.1/oem72.inf, unchanged binary from M251.
SYS492CB6B16B7607897D62F06AF9B21854B7AF1408E7C6C02DB4975CF095D0C1D5.
Quiet ICD4E05F1DF627CD6D9D64FE7F1ADDA29673D5B3B96B3750A08034A7EE88460C9EA matches M166.
Read-only loader comparison: elevated SSH ignores requested environment manifest and selects m8 registry ICD; Limited interactive task selects quiet-submit. No registry replacement.
The earlier M251/M252 harness omitted the Limited principal used in ops-gpu-016. Verbose legacy trace matches this configuration error. Do not attribute that run to the quiet ICD merely because its manifest was printed.
Driver guard acknowledged after crash evidence retained; controlled PnP start of same0798, boot13:18:44 unchanged.
Limited task runs Vulkan8tests x3, stories15M96tokens and TinyLlama64tokens, full offload.
All exit0; known-pattern Vulkan0mismatches. Both model outputs match local immutable Linux E14 reference after CRLF-to-LF only; independently checked on host in validation.json.
Final hardwareGFX2354/2354,SDMA24161/24161,zero timeouts/refusals,noTDR.
Temperature71.6..73.1C. Full WDDM initialized engines automatically, no post-start manual engine escapes.
One process per model here, not the full four-process acceptance series; no equivalent benchmark or pressure acceptance claim.
Raw outputs retained; PCI instance suffix redacted where present. Raw old evidence immutable, corrected interpretation here.
