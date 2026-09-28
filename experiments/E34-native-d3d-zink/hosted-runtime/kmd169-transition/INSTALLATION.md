# Replacing the forced downgrade path after M711

The existing UpdateDriverForPlugAndPlayDevices path started166 before Configure.
Do not repeat that assumption. select-driver.cpp is a read-only precursor: open
the exact devnode, build a compatible-driver list from one INF, require one node,
and report version plus CM status. It does not call DIF installation or change
device state. build-select-driver.ps1 builds with SDK26100 and /W4 /WX.

The candidate implementation direction is an explicit selected driver with
DI_DONOTCALLCONFIGMG on that device information set, followed by a class-installer
request through SetupDiCallClassInstaller(DIF_INSTALLDEVICE). The default install
handler is documented not to start the device with that flag. An application must
not directly call SetupDiInstallDevice, which is reserved for class installers.
No deferred-install mutation has been implemented or validated yet.

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
flag or that hardware can restart successfully. The runner remains unfit for a
new transition until the replacement is implemented and checked.
