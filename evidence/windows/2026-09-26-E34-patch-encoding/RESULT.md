# UTF-8 patch replay correction

BD-040 confirmed: the original M539 ICD patch decoded UTF-8 files through the
host's cp1250 default and then encoded the diff as UTF-8. Copyright text was
therefore re-encoded. Regenerated icd.patch from snapshots matching the original
source-manifest.json input/output hashes, using explicit UTF-8. The original
manifest is unchanged. Only patch representation changed; no DLL rebuilt.

Seven files replay with git -c core.autocrlf=false apply on LF-normalized input.
Restoring each recorded output newline convention reproduces every original
manifest byte hash. icd-replay-lines.json records both normalized hashes and
LF/CRLF output conventions. The existing trailing whitespace is preserved to
match those historical bytes; it is not an encoding/replay failure.

The same audit corrected derived before_lf_sha256 fields in present-manifest;
raw input/output hashes and patch bytes were unchanged. Exact corrections are
listed in lf-hash-corrections.json. Older evidence remains immutable.

This is historical incremental patch replay, not a full clean-stack build.
