# Research notes

Desk research done by sub-agents for a coming milestone, kept as delivered. Everything in here is derived from
source code and documentation (kernel, libdrm, WDK headers, Microsoft Learn), with citations; **nothing in here is a
measurement on our hardware**. What turns out to hold on unit A moves to `docs/facts.md` with its evidence; decisions
taken from these notes become ADRs.

| File | For | State |
|---|---|---|
| `m6-compute-dispatch.md` | The compute dispatch that closes M6: libdrm's gfx10 memset shader and its 18 PM4 packets | Input to the shim's dispatch emitter |
| `m7-full-wddm-miniport.md` | M7: from the display-only miniport to a full WDDM miniport that a Vulkan ICD can submit to; staged plan that keeps the failsafe | Input to the M7 ADR, not decided yet |
