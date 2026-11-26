# Unit side, read only: route and driver state after the Linux session.
"secureboot $(try { Confirm-SecureBootUEFI } catch { 'n/a' })"
"testsigning $((bcdedit /enum '{current}' | Select-String testsigning).Line)"
foreach ($s in 'bc250kmd','tpmserv','HLKSvc','HlkCampaignGuard','sshd') { $x = Get-Service $s -ErrorAction SilentlyContinue; "svc $s $($x.Status)" }
$d = Get-PnpDevice | Where-Object { $_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*' } | Select-Object -First 1; "gpu $($d.Status) $($d.Problem)"
$svc = Get-CimInstance Win32_SystemDriver -Filter "Name='bc250kmd'"; "kmd $((Get-FileHash $svc.PathName).Hash.Substring(0,8))"
"tpm $((Get-Tpm).TpmPresent) $((Get-Tpm).TpmReady)"
"dmaguard $((Get-CimInstance -Namespace root\Microsoft\Windows\DeviceGuard Win32_DeviceGuard -ErrorAction SilentlyContinue).AvailableSecurityProperties -join ',')"
