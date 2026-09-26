# M383 - Two Vulkan buffer views of one allocation

Hypothesis: a shader write through one VkBuffer view is visible to the next
shader through a distinct VkBuffer bound to the same VkDeviceMemory at offset0,
with the existing global compute-write/read barrier. Reuse the E14 integer hash
shader and CPU oracle, 16 changing input rounds, type3 host-coherent memory.
Positive control: unchanged three-buffer path, then aliased intermediate path.
Negative control: bind the second reader to separately allocated poisoned memory;
it must fail the byte oracle immediately. Verify compatible requirements and
print binding identity relation, not native handles. Never double-free memory.

Build probe locally; preserve source/hash. Target installed119/full session,
SYS4374CB20..., 1000MHz/820mV, temp<85C and STOP absent. Run hidden Limited task,
actual cache-intent-v2 ICD loader and submit witnesses, before/after counters.
No PnP, OS reboot or power action. Stop on unexpected result and retain logs.
This tests Vulkan resource alias visibility within one memory allocation; it
cannot establish CPU mapping cache attributes or cross-page paging-copy alias
semantics, and is not full M9 acceptance.
