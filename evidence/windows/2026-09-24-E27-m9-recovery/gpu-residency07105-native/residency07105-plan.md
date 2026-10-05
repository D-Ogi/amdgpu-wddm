# M337 - Current reservation implementation at 1GiB scale

Hypothesis: deployed105 preserves every source word over three explicit Windows eviction/restore cycles at1GiB, while admitted capture storage serves the workload without heap fallback.

Use the unchanged M320 native-file probe E9566E495A4FB5BB0F37B8A2583AB10F573D0F9ECEE2B84D94E264242529E9C3,300s watchdog and original64KiB control followed by1GiB. Same full GPU session as M336; no restart. Verify exact installed105 SYS, loaded revision, STOP,1000MHz/820mV and temperature<85C. Preserve before/after summaries, native stdout/stderr/exits and process handles. Read every word through GPU DMA_DATA to sentinel-initialized chunks, with fences; residency transitions must be3/2->1 for each cycle. Independent validator checks four full readbacks and all three cycles, completion counts and absence of TDR.

Record reserved/heap counter deltas. An observed fallback does not invalidate bytes but leaves reservation coverage incomplete. This is explicit eviction plus competing allocation demand, not proof of automatic oversubscription, arbitrary physical PFN relocation, shader cache policy or OS concurrency. No repeated initialization trial is authorized by this plan; preserve an interrupted process and inspect before rerun.
