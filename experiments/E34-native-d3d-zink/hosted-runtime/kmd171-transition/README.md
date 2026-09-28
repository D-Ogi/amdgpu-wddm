# Exact170 to M14 contract171 transition

Derived from the measured170 transition supervisor, retaining its original time budgets (including the170-second restoration acceptance limit), process-tree closure, SetupAPI logging restoration and package hash admission. No lab run is represented by this directory.

Candidate171: source75b8ab6f, SYS65172CA1058A11335F1132644DA844748758E9BE017D5192B5CE1E9B424FEEDF, version0.7.171.1, ABI000700AB. Rollback170: a3c2d2b/SYS67F02415. Both complete signed packages are pinned in package-hashes.json. CPU UMD8279/ICDCF39 remain the baseline.

Default mode tests the candidate then restores170. --deploy-candidate retains171 only after fresh checked CPU health and at least60 seconds of readiness; failed admission attempts170 restoration within the180-second envelope. A late or unsuccessful restore is recovery-unverified, never silently marked restored. No system M14 UMD registration or GPU workload is part of this transition.

prepare.py requires committed runner sources and a fresh destination. Stage and execute only after tests and exact baseline preflight. Use the existing kmd168-transition bounded-child helper and the selector built from select-driver.cpp. All helper files are bound into the final stage manifest.
