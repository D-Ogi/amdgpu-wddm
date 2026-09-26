# Piglit result comparison

`compare_piglit.py CASES WINDOWS LINUX OUTPUT.json` compares upstream results
against the exact shared case inventory. Inputs may be complete JSON/JSON.bz2
results or a tar containing an interrupted backend's `tests/*.json` files.
Archives are read without extracting files. Duplicate cases or JSON keys are
rejected instead of overwriting an earlier result.

The report includes input SHA256 hashes, per-platform counts, missing and
unexpected cases, incomplete execution, failures and subtest differences.
Exit 0 requires complete matching pass/skip results. Identical failures and
matching dry-runs cannot pass. Platform skips still need capability review;
this tool does not verify source revisions, GPU identity, image tolerances,
performance or formal conformance eligibility.

For the current quick profile, the retained inventory is
`evidence/windows/2026-09-26-E33-piglit-full-stage/cases.txt`.
Use matching upstream piglit/Mesa revisions on Linux and Windows. Keep the raw
inputs and the evidence establishing the selected device alongside the report.
Do not relabel a Windows input as Linux reference evidence.

Run `python experiments/E33-m12-applications/test_compare_piglit.py` from the
repository with `BC250_TEST_TMP` pointing to workspace scratch. The controls
cover complete results, equal failures, missing/unexpected/notrun cases,
subtest differences, compressed/partial readers, duplicate rejection, and the
retained interrupted437-case run with44600 cases missing from full quick.
