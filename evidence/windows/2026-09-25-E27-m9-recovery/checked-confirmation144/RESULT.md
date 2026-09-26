# M456 - Checked explicit confirmation on full 144

2026-09-25, unit A. No restart or KMD replacement. Automatic full-WDDM
confirmation remains unimplemented; this run is an explicit confirmation after
M455 content controls and the owner's physical image/cursor confirmation.

## Change and controls

CLI Confirm requires successful RegSetValueExW and RegFlushKey, requesting the
KEY_QUERY_VALUE right needed by flush. Monitor KmdRegistry.Confirm now checks
native RegFlushKey through SafeRegistryHandle and throws on nonzero status.
Existing automatic eligibility is unchanged. Actual-method host tests pass
12 native and four managed checks. Ignoring the flush result makes both
negative-control programs fail. Production CLI and monitor builds pass, along
with two CLI stage tests, four monitor Python tests and 34 inventory checks.
No host registry writes or development-PC UI were used.

CLI SHA256: 36C298D454A75C73483C610802A6D7A6E8940FB973FDAF7B1CABFF93BC219021.
Built monitor SHA256: 5C4FF0D1418C3DE725A520AC922BF0C0A5374D8AFD1AB6D658922F3920AFAAA3.
Only CLI was staged under C:/BC250/m9/checked-confirm456 and executed on the lab.
The built monitor was not installed or launched; the existing overlay stays.

At 01:21:57 local time, STOP=false, the staged CLI hash matches, existing budget
is zero and full-table policy is2. Confirm returns success with checked flush.
Readback stays budget0/policy2. Windows boot00:53:18, DWM1720/start00:54:20 and
monitor4228/start00:54:21 are retained. Preflight observes unchanged KMD144 SHA
EBA6C25DCE0F9C5362321B9D4468AD956A16385BB018274D538654372BC90ADA,
native1000MHz/VID116, ready1 and66.25C. No GPU test or display observer was run.

## Correction to M455 evidence interpretation

M455's candidate144-confirm-durable.ps1 invoked RegistryKey.Flush(). Microsoft
.NET Framework reference source ignores the native RegFlushKey return in that
method. A normal return therefore did not establish checked persistence. It
also does not prove that the earlier flush failed. The immutable M455 record
is preserved; its zero-budget durability claim is superseded by this checked
01:21:57 call. M455's KMD-side durable admission proof is unaffected: kernel
ZwFlushKey status was explicitly checked before POST/MMIO.

See agent-source-review.md for exact Microsoft sources and test scope. The
initial private preflight used an absent System32/drivers path and stopped
before mutation; preflight.log instead resolves the actual service ImagePath.
Logs here contain no device UUID/address; no redaction was needed.

Full health automation needs a new device-start identity and completed-primary
witness; docs/design/wddm-start-confirmation.md specifies the acceptance. This
run does not validate a new automatic-start interval, AC-cold entry, power
resume, a soak, or the remaining M9 DMA contracts.
