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

The runner now rejects confirmation before 60000 ms of readiness, with no completed work, or with a completion older than 15000 ms. Flags 7 alone do not admit confirmation. Generation and epoch are parsed as 64-bit values, matching the CLI. This is a fail-closed guard, not a completed timing redesign: candidate readiness and restored-baseline confirmation still need separate budgeting before another trial. The current runner must not be dispatched as a repaired transition. The deferred installer rejection E0000217 also remains unresolved.

## Read-only node location control (M713)

--inspect-store accepts a published/system INF path or a Driver Store INF path. It resolves the store location before constructing the list. It does not stage or install packages. Every mode now reports the actual node InfFileName and section, using SetupDiGetDriverInfoDetail static fields; ERROR_INSUFFICIENT_BUFFER is accepted only for its documented static-field guarantee.

[SetupGetInfDriverStoreLocation](https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupgetinfdriverstorelocationw) does not search by INF contents and must not be passed an arbitrary external INF as though it did. [SetupDiGetDriverInfoDetail](https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupdigetdriverinfodetailw) defines the static-field behavior. Local SDK26100 declarations were checked first.

M713 observes that a store-path input produces a published system INF node, not necessarily a node retaining the FileRepository path. Admission must compare registered package identity rather than assume string equality with the store path. Installation remains blocked pending a bounded same-package control and repaired recovery timing.

## Failed-install observations

Install now writes durable before/after observations of DriverVersion, ProblemCode, published INF, driver key and KMD service state/path. Each field carries readable/value/error, so a removed binding is distinct from problem 0. The before observation must show a stopped KMD service; otherwise no installer call is made. A native nonzero exit or invocation exception is interpreted only after recording the post-call observation. Queries remain inside the phase job deadline; a killed phase can lack the after receipt, which is unknown state, not evidence that the binding survived.

Local fault-injection controls cover native failure, invocation exception, loss of binding data and rejection of a running service. They do not prove recovery or deferred installation succeeds on the lab. The installed package and configuration still require independent verification before enable.
