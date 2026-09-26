# Replaying the hosted patches

Read and write source and patches as UTF-8 explicitly. Python's default locale
on the original Windows host was cp1250. Preserve raw before/after hashes; do
not infer a code change from newline conversion.

For M539 icd.patch, verify source-manifest.json raw inputs, normalize inputs to
LF, and use git -c core.autocrlf=false apply. Verify the after_lf_sha256 values
in icd-replay-lines.json. Restore each recorded after_newline convention and
verify the original source-manifest.json output byte hashes. Seven-file replay
is recorded in E34-patch-encoding. Later manifests with before_lf_sha256 support
LF replay directly. This procedure does not establish full-stack reproducibility.
