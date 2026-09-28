# M445: runtime firmware metadata and explicit SMU policy

2026-09-24. Source and host controls only; lab remains candidate136.

BD-025 replaces historical firmware replies with cached metadata from the exact
successfully loaded PSP image buffers and the native owner's GetSmuVersion reply.
The private query does no hardware or file I/O and refuses unready state. The
historical measured hardware template and 64-byte firmware ABI remain unchanged.

30773 native-owner, 34 metadata/query and 32 preflight checks pass, as does the
full WDK build. Two stale-version mutations fail two checks each. Detailed source,
logs, source hashes and scope are in [the review report](review/RESULT.md).

BD-023 is resolved as an explicit documented operation policy for the existing
measured telemetry getters. QueryGfxclk has no production caller; the retired raw
reader refuses its old IOCTL in M441. Header presence alone does not authorize
other messages. The transport does not implement a numeric message filter.
See [review](review/BD-023-TRIAGE.md) and [policy snapshot](hardware-policy.md).
No new hardware operation was performed for this policy clarification.

Main reviewed the parser/load publication, cached accessors and private query.
Changes were frozen into candidate138; neither this developer build nor the
combined candidate has been installed. Actual query-time startup ordering and
versions on unit A still require hardware acceptance. Sources/logs are copied
unaltered; firmware bytes, credentials and proprietary binaries are excluded.
