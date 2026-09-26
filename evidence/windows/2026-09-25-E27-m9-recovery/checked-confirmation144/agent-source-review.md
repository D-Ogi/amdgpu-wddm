# Durable explicit successful-start confirmation

Source changes only; no lab access, no host registry writes, no window/hotkey,
no automatic confirmation policy change, no commits or metadata changes.

Changed existing methods:
- tools/win/bc250kmd_cli/bc250kmd_cli.c: Confirm opens Parameters with
  KEY_SET_VALUE | KEY_QUERY_VALUE. It checks RegSetValueExW, then RegFlushKey
  before closing the handle; only both-success prints the confirmation and
  returns0. Write/flush errors return1 with separate diagnostics.
- tools/win/bc250mon/src/KmdProvider.cs: KmdRegistry.Confirm uses existing
  writable CreateSubKey (read/write access includes query), then calls checked
  RegFlushKey through SafeRegistryHandle. Nonzero native status throws
  Win32Exception; using scopes still dispose both keys. No real registry access
  occurs in tests. Existing automatic eligibility/health rules are untouched.

Why not RegistryKey.Flush:
- Microsoft .NET Framework reference source, registrykey.cs lines241-248:
  https://raw.githubusercontent.com/microsoft/referencesource/main/mscorlib/microsoft/win32/registrykey.cs
  Flush calls Win32Native.RegFlushKey(hkey) at245 without checking its result.
  This is a static implementation finding for the Framework reference code.
  It does not prove that any previous particular native flush failed. It does
  mean the managed method returning normally is not positive return-code evidence.
- Official RegFlushKey contract (local Win32 DDI page absent):
  https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-regflushkey
  Handle needs KEY_QUERY_VALUE; return0 is success, nonzero gives the error;
  success waits for hive changes to reach storage. Readback/close alone does
  not establish that guarantee.

Tests:
- tools/win/bc250kmd_cli/test-confirm.ps1 extracts the actual two methods.
  Native and managed registry fakes keep cached and durable values separate.
- test/confirm_test.c:12checks PASS, including success, write and flush failure,
  missing service, open/create failures, correct rights and handle closure.
- bc250mon/test/ConfirmTest.cs:4checks PASS, including success, write failure,
  checked native flush failure with exact error code, missing service.
- -IgnoreFlushFailure mutation: expected CLI1failure and managed1failure.
  Tests never invoke real RegFlushKey or modify host registry.
- Production CLI/control DLL built /W4 /WX. Existing CLI stage tests2PASS.
- Production monitor built /warnaserror+; existing Python4PASS and Vulkan
  provider/layout34checks PASS. Executables were not launched.

Artifacts:
- scratch/build/confirm144-client/bc250kmd_cli.exe
  SHA25636C298D454A75C73483C610802A6D7A6E8940FB973FDAF7B1CABFF93BC219021
- scratch/build/confirm144-monitor/bc250mon.exe (hash in parent handoff).
- host-positive.log, host-negative.log, cli-build.log, monitor-build.log here.

Remaining: root owns deployment and actual checked confirmation against stable144.
This does not fix the separate full-WDDM auto-confirm eligibility gap or add its
planned typed health handshake. Kernel durable count-before-MMIO remains intact.
