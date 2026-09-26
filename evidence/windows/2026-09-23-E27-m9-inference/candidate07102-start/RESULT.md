# M317: candidate07102 startup succeeds in one session

Installed0.7.102.1/oem76, SYS3B80D2EA3260CE053C11E888A274F909896E28D66D1CF6872B5213EDD5FFA760. Package25checks pass. One closed-gate installation and one traced full startup, no Windows reboot; boot19:27:05 unchanged. STOP clear, preflight1000MHz/VID116 and66C. Both host sessions terminal0. Independent final-identity.log re-verifies installed hash/version and FULL WDDM TABLE.

CP1 wrapper locks and all10cold KIQ checkpoints complete; active-queue recovery checkpoint absent. CP1rc0/27writes/9325us including diagnostic I/O. CP2..8rc0. Both engines ready at0.248s, attempted/completed0xF, no unwind/quarantine. Initial paging597/597,0timeouts/refusals,noTDR. This is a first GPU initialization in the recovered Windows boot, not warm GPU reentry after workload. The earlier07101 hang cause remains unproved; timing/IRQL and other source changes preclude attributing success to a single fix.

Latest persisted snapshot pulled unchanged: SHA5664B30FE10FE6369605E20DC26E97BFA82BA40FC81ED250C39C27C72375BA38; KeepStatus0. It contains CP1 checkpoints and subsequent stages through interrupt setup. Current registry EnableFullWddm0/counter2 while the loaded adapter uses FULL WDDM TABLE (one-shot gate behavior). Future restart needs deliberate guard/gate handling.

Cache observation: physical table queries return0xC0000141, so the accompanying type field does not prove an effective cache type. Retained/borrowed-view cache compatibility remains open. Flip/blit gates remain closed; this is not proof of accelerated desktop presentation.

Execution caveat: cloning UTF8-BOM scripts with the host's default text decoding corrupted the first line in install/start scripts. ErrorActionPreference assignment failed; subsequent explicit checks and operations ran. Preserve raw streams, including the error. Actual version/hash/PnP/boot and startup/paging evidence above are independently checked; SSHexit0 alone is not acceptance. ASCII-clean future copies prepared, no retry of the completed experiment. Scripts/source snapshots identify the worktree; no commit binding or broader M9 completion claim.
