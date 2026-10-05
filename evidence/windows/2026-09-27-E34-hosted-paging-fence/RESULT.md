# Hosted RADV paging-fence correction

The hosted ICD3508416F is preserved. Its source baseline is upstream05e6c962
plus local changes, not a clean upstream build. The retained local source/diff
inventory is scratch/g0-hosted/hosted-fence001; only radv_wddm2_bo.c changed
relative to the preserved28-file modified/untracked content manifest.

The three-site fix from fork20562c4d keeps the maximum paging fence across
mapping and residency during allocation and import, and across sparse alias
mapping chunks. The resulting hosted ICD is C0CE5DCD (full hashes in receipt).

The exact BO object linked into that DLL passes all13 scripted dispatch cases:
map fence larger/equal/smaller than residency; residency S_OK with zero;
completed map with pending residency; sparse final-zero/smaller/increasing values.
The retained negative-control report for hosted3508416F had6 failures of13.
This run reuses the same harness in a separate directory and preserves old output.

This is host contract validation only. No lab deployment, no measured GPU race,
no IN_USE explanation and no G0 completion is claimed. Candidate promotion needs
hosted-client content/sharing/fence validation and a bounded desktop trial.

PROVENANCE: Mesa MIT; patch20562c4d and the local compiled-object harness.
