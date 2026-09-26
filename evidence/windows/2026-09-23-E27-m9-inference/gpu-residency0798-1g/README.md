# GPU1GiB control and inconclusive eviction, unit A, 2026-09-23

Same probe binary/source as gpu-residency0798-small, SHA256 BE3F5F8C95A70EA40164F7ABC641556870B738B91B577918CEA0457895AB60E4. Raw output unedited. Native exit1 captured by cmd wrapper. Initial1GiB GPU copy/readback passes every dword,1024 submissions/fence values. First Evict returns SUCCESS but residency remains1 for5s, so test exits before pressure allocation and before any post-eviction GPU read. This does not establish physical relocation or a driver defect. No full pressure acceptance.

Local MS d3dkmthk.md:12491 says Evict decrements residency reference count and removes from device residency list at zero; it does not promise immediate physical transfer. Revised harness must dirty competing memory before requiring departure. Source revision for that improvement is separate from this measured binary.
