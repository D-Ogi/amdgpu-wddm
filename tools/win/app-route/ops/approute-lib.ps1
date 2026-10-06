# Application routing (M14.1) lab helpers: pure functions, dot-sourced by the ops scripts and tested on the
# development PC by tests\test-approute-lib.ps1 on scratch files. Windows PowerShell 5.1. durable.ps1 (the GPU DWM
# kit's byte copy) must be dot-sourced first.

$script:AppExeNamePattern = '^[A-Za-z0-9_.-]+\.exe$'
$script:AppModes = @('cpu','allowlist','gpu-default')

function Get-AppFileSha {
 param([Parameter(Mandatory)][string]$Path)
 if (Test-Path -LiteralPath $Path -PathType Leaf) { return (Get-FileHash -LiteralPath $Path).Hash }
 return ''
}

# app-route.json of the package: schema 1, every hash upper-case hex, every name a plain file name.
function Read-AppRouteManifest {
 param([Parameter(Mandatory)][string]$Root)
 $m = Get-Content -LiteralPath (Join-Path $Root 'app-route.json') -Raw | ConvertFrom-Json
 if ($m.schema -ne 1) { throw 'app-route.json schema' }
 foreach ($h in @($m.router.sha256, $m.router.baseline_sha256)) { if ([string]$h -cnotmatch '^[0-9A-F]{64}$') { throw 'app-route.json router hash' } }
 if ($m.router.sha256 -eq $m.router.baseline_sha256) { throw 'app-route.json router equals its baseline' }
 if ([string]$m.router.active_path -notmatch '^[A-Za-z]:\\.+\.dll$') { throw 'app-route.json active_path' }
 if ([string]$m.cpu_umd_path -notmatch '^[A-Za-z]:\\.+\.dll$' -or [string]$m.cpu_umd_sha256 -cnotmatch '^[0-9A-F]{64}$') { throw 'app-route.json CPU UMD' }
 foreach ($p in @($m.gpu.files.PSObject.Properties) + @($m.clients.PSObject.Properties)) {
  if ($p.Name -notmatch '^[A-Za-z0-9_.-]+$' -or [string]$p.Value -cnotmatch '^[0-9A-F]{64}$') { throw "app-route.json entry $($p.Name)" }
 }
 if (!$m.gpu.files.PSObject.Properties[[string]$m.gpu.shell]) { throw 'app-route.json shell not among the gpu files' }
 foreach ($n in @($m.deny_always)) { if ([string]$n -notmatch $script:AppExeNamePattern) { throw "app-route.json deny_always $n" } }
 if ([string]$m.client_task -match 'BC250|DWM|G0|WSI') { throw 'client task name would trip the competing-task gates' }
 return $m
}

# SHA256SUMS.txt ("<hash> *<relative path>", forward slashes) against the files: every listed file present and equal.
function Test-AppRoutePackage {
 param([Parameter(Mandatory)][string]$Root)
 $bad = @(); $n = 0
 foreach ($line in Get-Content -LiteralPath (Join-Path $Root 'SHA256SUMS.txt')) {
  if ($line -notmatch '^([0-9a-fA-F]{64}) \*(.+)$') { throw "SHA256SUMS.txt line: $line" }
  $n++
  $path = Join-Path $Root ($Matches[2] -replace '/','\')
  if ((Get-AppFileSha $path) -ne $Matches[1].ToUpperInvariant()) { $bad += $Matches[2] }
 }
 return @{files=$n; mismatched=$bad}
}

# "a.exe,b.exe" (as target.py passes it through -File) -> validated names, duplicates dropped case-insensitively.
function ConvertTo-AppList {
 param([string]$Text)
 $out = New-Object System.Collections.Generic.List[string]
 foreach ($x in @(([string]$Text) -split ',')) {
  $n = $x.Trim()
  if (!$n) { continue }
  if ($n -notmatch $script:AppExeNamePattern) { throw "Not an image base name: $n" }
  if (!@($out | Where-Object { $_ -ieq $n }).Count) { $out.Add($n) }
 }
 return ,([string[]]$out.ToArray())
}

# The AppRouter values a mode writes. cpu: Mode alone (the application kill switch; every other value stays).
# allowlist and gpu-default: everything, exactly; Deny always carries the package's deny_always names.
function Get-AppPolicyDesired {
 param([Parameter(Mandatory)][string]$Mode,[string[]]$Allow=@(),[string[]]$Deny=@(),[Parameter(Mandatory)]$Manifest,[Parameter(Mandatory)][string]$Root)
 if ($Mode -cnotin $script:AppModes) { throw "Unknown mode $Mode" }
 $v = [ordered]@{}
 $v['Mode'] = @{kind='String'; value=$Mode}
 if ($Mode -eq 'cpu') { return @{partial=$true; values=$v} }
 $deny = ConvertTo-AppList ((@($Deny) + @($Manifest.deny_always)) -join ',')
 $allow = ConvertTo-AppList (@($Allow) -join ',')
 if ($Mode -eq 'allowlist' -and !$allow.Count) { throw 'allowlist needs at least one -Allow name' }
 $v['GpuUmdPath'] = @{kind='String'; value=(Join-Path (Join-Path $Root $Manifest.gpu.dir) $Manifest.gpu.shell)}
 $v['RouteLogDirectory'] = @{kind='String'; value=(Join-Path $Root 'logs')}
 if ($allow.Count) { $v['Allow'] = @{kind='MultiString'; value=$allow} }
 if ($deny.Count) { $v['Deny'] = @{kind='MultiString'; value=$deny} }
 return @{partial=$false; values=$v}
}

# name -> {kind, value} of an open registry key.
function Read-AppKeyValues {
 param($Key)
 $r = [ordered]@{}
 if (!$Key) { return $r }
 foreach ($n in @($Key.GetValueNames() | Sort-Object)) {
  $kind = [string]$Key.GetValueKind($n)
  $r[$n] = @{kind=$kind; value=$Key.GetValue($n,$null,[Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)}
 }
 return $r
}

function Test-AppValueEqual {
 param($A,$B)
 if ([string]$A.kind -cne [string]$B.kind) { return $false }
 if ($A.kind -ceq 'MultiString') {
  $x = @($A.value); $y = @($B.value)
  if ($x.Count -ne $y.Count) { return $false }
  for ($i = 0; $i -lt $x.Count; $i++) { if ([string]$x[$i] -cne [string]$y[$i]) { return $false } }
  return $true
 }
 return [string]$A.value -ceq [string]$B.value
}

# The values a key must hold after a write of Desired over Before (partial: Before's other values stay).
function Get-AppPolicyExpected {
 param([Parameter(Mandatory)]$Desired,$Before)
 $e = [ordered]@{}
 if ($Desired.partial -and $Before) { foreach ($n in $Before.Keys) { $e[$n] = $Before[$n] } }
 foreach ($n in $Desired.values.Keys) { $e[$n] = $Desired.values[$n] }
 return $e
}

function Test-AppPolicyEqual {
 param($Expected,$Actual)
 $a = @($Expected.Keys | Sort-Object); $b = @($Actual.Keys | Sort-Object)
 if (($a -join '|') -cne ($b -join '|')) { return $false }
 foreach ($n in $a) { if (!(Test-AppValueEqual $Expected[$n] $Actual[$n])) { return $false } }
 return $true
}

# ---------------------------------------------------------------- router file swap (same path, no registration change)
# The proven file-route pattern (scratch\m14\fl12\fl12native002\package\file-routing.ps1): never a partially copied
# DLL under the active path; a loaded DLL is renamed aside (held), never deleted; a foreign active file is preserved.

function Install-AppRouter {
 param([Parameter(Mandatory)][string]$Active,[Parameter(Mandatory)][string]$Source,[Parameter(Mandatory)][string]$Baseline,
       [Parameter(Mandatory)][string]$Candidate,[Parameter(Mandatory)][string]$Stamp)
 if ((Get-AppFileSha $Source) -ne $Candidate) { throw 'Package router hash mismatch' }
 $current = Get-AppFileSha $Active
 if ($current -eq $Candidate) { return @{changed=$false; held=$null} }
 if ($current -ne $Baseline) { throw "Active router is neither the baseline nor the candidate ($current); preserved" }
 $prepared = $Active + '.app-route-candidate'
 if (Test-Path -LiteralPath $prepared) {
  if ((Get-AppFileSha $prepared) -ne $Candidate) { throw 'Foreign prepared file; inspect' }
  Remove-Item -LiteralPath $prepared -Force
 }
 Copy-VerifiedDurable $Source $prepared $Candidate
 $held = $Active + '.app-route-held-' + $Stamp
 if (Test-Path -LiteralPath $held) { throw 'Held path exists' }
 Move-Item -LiteralPath $Active -Destination $held
 try { Move-Item -LiteralPath $prepared -Destination $Active }
 catch { Move-Item -LiteralPath $held -Destination $Active; throw }
 if ((Get-AppFileSha $Active) -ne $Candidate) { throw 'Installed router mismatch' }
 return @{changed=$true; held=$held}
}

function Restore-AppRouter {
 param([Parameter(Mandatory)][string]$Active,[Parameter(Mandatory)][string]$BaselineSource,[Parameter(Mandatory)][string]$Baseline,
       [Parameter(Mandatory)][string]$Candidate,[Parameter(Mandatory)][string]$Stamp)
 $current = Get-AppFileSha $Active
 if ($current -eq $Baseline) { return @{changed=$false; held=$null} }
 if ($current -and $current -ne $Candidate) { throw "Active router is foreign ($current); preserved" }
 $prepared = $Active + '.app-route-restore-' + $Stamp
 Copy-VerifiedDurable $BaselineSource $prepared $Baseline
 $held = $null
 if ($current) {
  $held = $Active + '.app-route-held-' + $Stamp
  if (Test-Path -LiteralPath $held) { throw 'Held path exists' }
  Move-Item -LiteralPath $Active -Destination $held
 }
 try { Move-Item -LiteralPath $prepared -Destination $Active }
 catch { if ($held) { Move-Item -LiteralPath $held -Destination $Active }; throw }
 if ((Get-AppFileSha $Active) -ne $Baseline) { throw 'Restored router mismatch' }
 return @{changed=$true; held=$held}
}

# Held and prepared leftovers next to the active router: removed when no process maps them any more.
function Remove-AppRouterLeftovers {
 param([Parameter(Mandatory)][string]$Active)
 $dir = Split-Path -Parent $Active; $leaf = Split-Path -Leaf $Active
 $r = @()
 foreach ($f in @(Get-ChildItem -LiteralPath $dir -File | Where-Object { $_.Name -like ($leaf + '.app-route-*') })) {
  try { Remove-Item -LiteralPath $f.FullName -Force -ErrorAction Stop; $r += @{name=$f.Name; removed=$true} }
  catch { $r += @{name=$f.Name; removed=$false; error=[string]$_} }
 }
 return $r
}
