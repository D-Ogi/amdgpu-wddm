# Audit client004: completed descriptor and staging store spans

Prepared, not run. Clone of client003 with exact KMD166 and UMD3484e1f4.
Hosted ICD C0CE and the UpdateSubresource control remain fixed. Desktop gates
stay off; router selects only this exact executable. CPU DWM must retain its
identity. Existing 45-second client deadline and independent restoration apply.

Hypothesis: descriptor stores and initial 4/16-byte staging writes have bounded,
paired witnesses; every checkpoint reconciles store begin/end counts. The
intentional image uploads remain visible to the DDI audit. This small control
cannot establish G0, desktop correctness, or absence of all uninstrumented writers.

Acceptance: two pixel controls, exact loaded modules, eight phase markers,
strict lifetime/store/DDI validation, completed staging-copy witnesses, measured
record rate and maximum log size. Any invalid span, sequence loss, missing
checkpoint count or missing completed copy leaves the audit unverified.
Restore UMD8279/ICDCF39, remove terminal tasks, preserve receipts. No reset or
KMD/registry change planned. A failed control is not retried under this run ID.
