# RADV RT node-address correction

mesa-wddm2-rt-node-address.patch applies to the owned Mesa05e6c962-based working tree. In build_node_to_addr, GFX9+ now sign-extends the reconstructed48-bit address instead of forcing upper16 bits to one. This matches the existing bvh_helpers.h node_to_addr helper. Pre-GFX9 behavior is unchanged. The change permits canonical low-half WDDM addresses while preserving canonical high-half reconstruction.

Build candidate:949669FF9194C6F5A63D4DCB175939AFE7AD0067A084417C0D2207A7CC0C8C0F. Previous C388 ICD and exact before/after source retained in scratch/m15/rt-icd001. The source tree contains prior local modifications; this is a one-file incremental build, not a clean upstream build.

Validation2026-09-28: DLL build succeeds; deterministic host integer check passes80040 tagged-node vectors (seed0x13fe), comparing reconstruction with original canonical addresses. The old expression disagrees for40544 low-half vectors and agrees on valid high-half vectors. This does not test generated NIR, shader execution or BVH traversal. Next gates are exact-candidate non-RT regression and bounded ray-query/RT-pipeline tests. No lab deployment or game RT acceptance is claimed.

PROVENANCE: Mesa (MIT); existing RADV bvh_helpers.h address representation and the owned fork's RT address fix.
