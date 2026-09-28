# M692 - Exact165 diagnostic deployment and native GPU control

Unit A, 2026-09-27. Source b83a594, SYS548E9D8D, transition f99c097.
The candidate derives from deployed933f383 with bounded VSync diagnostics and
version changes only. Four source files normalize CRLF; use --ignore-cr-at-eol
when reviewing the exact-source diff. Host ISR test passes832 checks; build
manifest is clean/eligible. Main-tree SYS3DF03564 was not installed.

Installation worker started20:13:39Z and exited0 at20:14:04Z. Exact0.7.165.1/A5
SYS hash matches. OS boot18:59:38.5Z and CPU DWM were retained. The independent
collector completed161 samples at20:16:40Z without reader timeout. All original
processes were checked terminal; transition and native012 tasks were removed.

Native012 reuses executable0821C9BD from native010/M663 in dirty-list mode.
Five GPU-copy cases and30 residency checks pass, final fence34; exit0 at20:16:57Z.
The runner verified unchanged OS/DWM across the control. This measures BC2S
copy content and residency, not desktop composition or absence of CPU copies.

Final20:19:05Z readback verifies exact165, baseline UMD8279/ICDCF39, health15,
guard0, experimental Present/CDD gates0, native1000MHz/VID116 and67C.
Exact164 rollback remains staged. No hosted DWM trial or G0 promotion occurred.
The spontaneous flip-timeout cause and effectiveness of diagnostics during a
future failure remain unproven; passing this control is not a stability claim.

Evidence files select result lines and technical fields from unchanged private
receipts at scratch/g0-hosted/kmd165-ops. Raw archives remain private; unrelated
metadata and device/process identifiers are omitted. Archive hashes are retained
in closure-summary.json. Selected result lines are copied without rewriting.
