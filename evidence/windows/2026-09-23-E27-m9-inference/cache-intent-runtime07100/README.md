# Paired cache-intent runtime control, unit A, 2026-09-23

Same KMD0.7.100.1/device session as M271; no restart between controls. Limited interactive tasks selected exact ICD manifests and loader traces verify the actual path plus submit progress. Old quiet ICD SHA4E05F1DF627CD6D9D64FE7F1ADDA29673D5B3B96B3750A08034A7EE88460C9EA; isolated v2 ICD6B589A8686DF6FFBB2BE447845222A3607E7E162B89923FA5976FCD9FD412754. Global quiet ICD remains unchanged.

Each ICD passes96positive rounds (types2,3,5 each16+16) plus3stale-input controls retaining previous-round GPU hashes. All18native exits expected; independent validator passes both complete result sets. Probe SHA96381FCF0C6CC95F24872BD74ECD4F66F96CD98AD12A629D34D7B285036740DB.

v1: GFX0->102 all complete; paging3058->9605 all complete. System leaf encoding coherent0->18, noncoherent164->63034, snoop mismatches0.
v2: GFX102->204 all complete; paging10864->16284 all complete. System leaf encoding coherent18->23076 (+23058), noncoherent63102->102872, snoop mismatches0. All summaries show zero timeouts/refusals/noTDR. Encoding counters include repeated logical updates, not unique pages or independent GPU completion counts. Background OS paging also contributes.

1000MHz/VID116. During v1 temperature71.5..72.2C; v2 71.0..71.9C. Scheduled tasks finished and were removed by their harnesses. Runtime pairing and shader visibility are demonstrated. Actual CPU PAT/WB mappings, borrowed CPU_VIRTUAL alias attributes, eviction+shader and repeated hardware start remain unproven. Allocation diagnostic lines are capped, so this capture does not attribute every Cached flag to an individual v2 buffer. Do not infer a cache mapping type solely from a Vulkan property.

Validator now accepts --icd-dir cache-intent-v2 while retaining exact expected path checks. Raw output decoded to UTF8 and adapter instance suffixes redacted; no shader output/counters edited.
