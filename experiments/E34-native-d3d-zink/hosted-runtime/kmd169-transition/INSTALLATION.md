# Replacing the forced downgrade path after M711

The existing UpdateDriverForPlugAndPlayDevices path started166 before Configure.
Do not repeat that assumption. The --inspect mode in select-driver.cpp is a read-only precursor: open
the exact devnode, build a compatible-driver list from one INF, require one node,
and report version plus CM status. It does not call DIF installation or change
device state. build-select-driver.ps1 builds with SDK26100 and /W4 /WX.

The candidate implementation direction is an explicit selected driver with
DI_DONOTCALLCONFIGMG on that device information set, followed by a class-installer
request through SetupDiCallClassInstaller(DIF_INSTALLDEVICE). The default install
handler is documented not to start the device with that flag. An application must
not directly call SetupDiInstallDevice, which is reserved for class installers.
The explicit --install-deferred mode now implements that request. Stage005 rejected both directions with E0000217 (M712); deferred installation is not validated.

After installation, require the expected package identity and a still-disabled
devnode before restoring saved configuration. Clear any process-local suppression
before a separate explicit enable. Do not call CM enable/disable/remove/re-enumerate
while DI_DONOTCALLCONFIGMG is in effect. Read-only CM status queries are separate.
If a class installer ignores/overrides the intended behavior, fail and preserve
its flags/status rather than claiming that suppression worked.

References checked2026-09-28:
- SDK26100 SetupAPI.h: DI_ENUMSINGLEINF/DI_DONOTCALLCONFIGMG declarations.
- [Device installation parameters](https://learn.microsoft.com/en-us/windows/win32/api/setupapi/ns-setupapi-sp_devinstall_params_a).
- [Default install handler and its start behavior](https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupdiinstalldevice).
- [DIF_INSTALLDEVICE](https://learn.microsoft.com/en-us/previous-versions/windows/drivers/install/dif-installdevice).

The local conceptual sources and headers were searched first; the API contract
pages above filled the missing per-function behavior. This is an implementation
plan, not evidence that the Windows22631 display class installer preserves the
flag or that hardware can restart successfully. The runner uses the replacement in both directions; a review and bounded lab
rehearsal remain required before treating the new behavior as established.

Admission: exact one compatible node, explicit expected four-component version,
CR_SUCCESS with DN_HAS_PROBLEM/CM_PROB_DISABLED and without DN_STARTED. Following
the class-installer call, require suppression retained, no restart-required bits
and the same disabled-state predicate. Nonzero exits preserve observations and
stop the phase. The next phase independently checks SYS/package identity before
restoring configuration. Destroying the process-local information set ends this
request; explicit enable is issued by a separate process after configuration.

Local validation: native /W4 /WX build and eight invalid-argument controls. These
prove build/admission behavior, not suppression by the display class installer.
Preparation now requires --selector and includes its binary in the stage manifest.

## Confirmation admission after M712

The runner now rejects confirmation before 60000 ms of readiness, with no completed work, or with a completion older than 15000 ms. Flags 7 alone do not admit confirmation. Generation and epoch are parsed as 64-bit values, matching the CLI. Candidate and restore acceptance are separated below. The current runner must not be dispatched as a repaired transition until the installer and recovery path have been validated. The deferred installer rejection E0000217 also remains unresolved.

## Read-only node location control (M713)

--inspect-store accepts a published/system INF path or a Driver Store INF path. It resolves the store location before constructing the list. It does not stage or install packages. Every mode now reports the actual node InfFileName and section, using SetupDiGetDriverInfoDetail static fields; ERROR_INSUFFICIENT_BUFFER is accepted only for its documented static-field guarantee.

[SetupGetInfDriverStoreLocation](https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupgetinfdriverstorelocationw) does not search by INF contents and must not be passed an arbitrary external INF as though it did. [SetupDiGetDriverInfoDetail](https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupdigetdriverinfodetailw) defines the static-field behavior. Local SDK26100 declarations were checked first.

M713 observes that a store-path input produces a published system INF node, not necessarily a node retaining the FileRepository path. Admission must compare registered package identity rather than assume string equality with the store path. Installation remains blocked pending a bounded same-package control and repaired recovery timing.

## Failed-install observations

Install now writes durable before/after observations of DriverVersion, ProblemCode, published INF, driver key and KMD service state/path. Each field carries readable/value/error, so a removed binding is distinct from problem 0. The before observation must show a stopped KMD service; otherwise no installer call is made. A native nonzero exit or invocation exception is interpreted only after recording the post-call observation. Queries remain inside the phase job deadline; a killed phase can lack the after receipt, which is unknown state, not evidence that the binding survived.

Local fault-injection controls cover native failure, invocation exception, loss of binding data and rejection of a running service. They do not prove recovery or deferred installation succeeds on the lab. The installed package and configuration still require independent verification before enable.

## Bounded confirmation schedule

Candidate Verify records candidate-ready-only: stable CPU module observations bracketed by the same KMD generation/epoch, exact ABI and fresh completed work. It does not invoke confirmation or claim a health-confirmed candidate. This is admission for a transition rehearsal, not GPU promotion or G0 acceptance.

Restore Verify waits for the unchanged generation/epoch to satisfy full confirmation admission, invokes checked confirmation if needed, then requires a fresh flags15 witness and strict guard0 CPU check. It can use up to 70 seconds inside the original global schedule, ending no later than T+160 seconds. Ten seconds remain for package cleanup under the T+170 work deadline; the outer task cap remains 180 seconds. No deadline is extended or restarted. A late restore start can leave too little time and must remain recovery-required, not accepted. The phase job bounds blocking query calls, while the polling helper also rejects a result returned past its own deadline.

Local tests cover age boundaries, stale work, no work, identity changes and late reads. They establish control behavior only; no new transition has validated this schedule on the lab.

## Registered-package admission

Capture requires exactly one published INF matching each manifest INF hash before any device disable. Package staging is a separate prerequisite, not an implicit operation in Install. No missing-package fallback to an external INF is permitted.

Install reselects the published INF by hash, runs the read-only store inspection, and checks both published and resolved-store INF hashes against the manifest. The native installation helper resolves the input through SetupGetInfDriverStoreLocation and SetupGetInfPublishedName before building its selected driver list. It therefore requests the registered node instead of the external package node observed in M712. Version, disabled-state and stopped-service checks still apply. No lab installation has validated this change yet; E0000217 causality remains unproven.

Local /W4 /WX build and argument/package selection controls pass. Local API source: ref/sdk-api-docs at a4fd3f7e, setupapi/nf-setupapi-setupgetinfpublishednamew.md and nf-setupapi-setupgetinfdriverstorelocationw.md. A bounded exact166-to-exact166 control with preserved verbose SetupAPI settings and a tested recovery route must precede another169 transition.

## Settled startup anchor

The readiness sampler now reads CPU DWM identity and ready health together. Two consecutive valid samples must match both the DWM PID/start set and the KMD generation/epoch, with fresh completed work. Startup escape failures, flags below ready and epoch transitions reset the pair and are retried only within the original deadline. The accepted pair becomes the confirmation anchor. Changes after that point still fail verification. This avoids treating normal mode/visibility invalidations during startup as failures of an already settled interval.

Regression controls cover unavailable escape, epoch3/4/5 startup, a failed sample between equal pairs, and DWM replacement with unchanged health. Existing readiness and confirmation deadline tests remain passing.

## SetupAPI logging support for the pending control

setup-log.ps1 provides read/write/restore of the 64-bit Setup LogLevel DWORD and before/after log-file byte offsets. Start requires a durable save callback before setting 0x0000FFFF; restoration preserves absent versus explicit zero and verifies readback. A conflicting later setting is reported rather than overwritten. No no-flush flag is introduced. Tests use an in-memory backend, not the host or lab registry.

The transition supervisor now runs this support in independent bounded children and restores from the saved receipt after a closed child failure or on success; it does not rely on a finally block in the killable installer. No lab control has exercised this integration yet. Missing after receipts or changed/truncated log files must be treated explicitly when extracting the interval. Source: ref/windows-driver-docs/windows-driver-docs-pr/install/setting-setupapi-logging-levels.md, pinned conceptual reference 110f60ea. No logging setting has been changed on the lab by this addition.

Logging setup has at most 10 seconds within the existing candidate schedule. Restoration has at most 5 seconds and ends by T+175, leaving the scheduled-task cap at T+180. Unknown closure of a logging child prevents another logging writer from starting and leaves recovery-required. A verified stopped child permits restore even if it failed. Restore records both log offsets and a checked final setting; a missing backup means Enable never passed its durable-save barrier. A malformed backup cannot be accepted. Abrupt termination of the entire supervisor still requires manual recovery from the saved receipt.

## Same-package control assembly

prepare.py --same-package-control uses the pinned rollback166 package for both package labels and writes a manifest-covered transition-policy.json. It does not replace the baseline hashes with user-supplied ones. Capture and phase checks remain required. The candidate arm stops after disabled deferred installation; it never enables a candidate. Restore runs Quiesce, Rebind, Configure, Enable and full Verify, without removing the active package.

A completed Install receipt plus exact166/problem22 permits binding reuse. Otherwise the recovery phase uses the previously exercised forced newdev exact166 rebind, records its result and any reboot request, then explicitly disables before Configure. Reboot requests and failed recovery are not accepted. Configure now writes and flushes the actual class registry key and checks both MultiString registrations before enable. The fallback can itself start the INF defaults briefly, as M711/M712 showed; this risk is retained and must be reviewed, not described as a deferred fallback.

This control mode has local syntax/admission validation only. It has not run on the lab. The inherited global deadlines remain unchanged; successful installation does not by itself close the control without confirmed CPU recovery and restored log settings.

## Readiness receipt projection after M714

CPU observations now project only EnableGpuPresentBlit, EnableCddDwmInterop and UnconfirmedStarts from Get-ItemProperty. Provider metadata (PSDrive/PSProvider and related object graphs) is not serialized. Missing values stay null and fail the existing baseline checks. M715 validates the corrected two-report path on CPU166 in1.535s; it does not retroactively turn M714's automatic verification timeout into a pass.
