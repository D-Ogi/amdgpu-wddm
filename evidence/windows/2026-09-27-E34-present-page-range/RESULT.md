# Present CPU mapping validates every GPU page

2026-09-27. Source parent 608cada plus this change. Host validation only;
no lab deployment, no GPU Present or residency acceptance.

The diagnostic E26P CPU copy formerly compared only the translated first and
last bytes. That cannot prove physical contiguity when an interior page differs.
Both source and explicit destination now use the same page-walking helper.
Every covered 4 KiB page must translate to successive VRAM addresses. Missing
pages, system-memory pages, discontinuities and overflow are refused before the
corresponding CPU range is mapped. Output bounds are published only on success.
The helper also handles an unaligned first byte and a partial final page.

The host test includes a four-page range with matching outer pages but a foreign
interior page, plus missing/system interior pages, contiguous and unaligned
positive controls, overflow and invalid-argument cases. Fifteen new assertions
pass; the complete surface suite reports58275 checks with zero failures.
All13 mandatory quick gates pass, including WDDM compilation and static analysis.

This checks mapping arithmetic at the time of each walk. It does not pin pages,
establish device residency or replace execution/lifetime synchronization.
The old diagnostic copy remains a CPU path and cannot satisfy G0.
