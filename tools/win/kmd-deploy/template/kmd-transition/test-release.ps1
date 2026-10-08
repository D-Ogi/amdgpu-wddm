param([Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\release.ps1"
if (Test-Path $Out) { throw 'Use a fresh output directory' }
[void](New-Item -ItemType Directory $Out)
$cases = 0
function Check([string]$Name, [bool]$Ok, [string]$Detail = '') { $script:cases++; if (!$Ok) { throw "FAIL: $Name $Detail" } }
function Refused([string]$Name, [scriptblock]$Action, [string]$Expect) {
 try { $r = & $Action; Check $Name $false "returned $r" } catch { Check $Name ($_.Exception.Message.Contains($Expect)) $_.Exception.Message }
}
# Stand-in clients: the usage of the release client the lab runs, and the older one without the health and clock forms.
$full = "@echo off`r`necho usage: bc250kmd_cli info ^| list ^| stages ^| confirm 1>&2`r`necho        bc250kmd_cli health read ^| health confirm ^<generation^> ^<epoch^> 1>&2`r`necho        bc250kmd_cli clock read ^| clock set ^<MHz^> ^<mV^> 1>&2`r`necho        bc250kmd_cli log ^| log summary 1>&2`r`nexit /b 2`r`n"
$old = "@echo off`r`necho usage: bc250kmd_cli info ^| list ^| stages ^| confirm 1>&2`r`necho        bc250kmd_cli dpm [count [interval ms]] ^| dpm confirm   (clock governor) 1>&2`r`nexit /b 2`r`n"
# Counts its own runs, so the usage cache is measured and not assumed.
$counting = "@echo off`r`necho x>>`"%~dp0runs.txt`"`r`necho usage: bc250kmd_cli health read 1>&2`r`nexit /b 2`r`n"
[IO.File]::WriteAllText("$Out\full.cmd", $full)
[IO.File]::WriteAllText("$Out\old.cmd", $old)
[IO.File]::WriteAllText("$Out\counting.cmd", $counting)

# The release client answers the forms its usage lists, and the path comes back unchanged.
Check 'a listing release client answers health read' ((Resolve-KmdClient "$Out\full.cmd" 'health read') -ceq "$Out\full.cmd")
Check 'one call checks several forms' ((Resolve-KmdClient "$Out\full.cmd" @('info', 'clock read', 'log summary')) -ceq "$Out\full.cmd")
Check 'no form asked: the path only' ((Resolve-KmdClient "$Out\full.cmd") -ceq "$Out\full.cmd")
Check 'called under Stop, $ErrorActionPreference unchanged' ($ErrorActionPreference -eq 'Stop')

# No fallback: a client without the form, a missing client and an empty path are refused, each with the cause.
Refused 'an older client refuses health read, and the form is named' { Resolve-KmdClient "$Out\old.cmd" 'health read' } "does not list 'health read'"
Refused 'the governor text is not the clock form' { Resolve-KmdClient "$Out\old.cmd" 'clock read' } "does not list 'clock read'"
Refused 'one missing form in a list refuses' { Resolve-KmdClient "$Out\full.cmd" @('info', 'dpm confirm') } "does not list 'dpm confirm'"
Refused 'a missing client is refused' { Resolve-KmdClient "$Out\none.exe" 'health read' } 'No release KMD client'
Refused 'an empty path is refused' { Resolve-KmdClient '' 'health read' } 'No release KMD client'
Refused 'a directory is not a client' { Resolve-KmdClient $Out 'health read' } 'No release KMD client'
Check 'refused under Stop, $ErrorActionPreference unchanged' ($ErrorActionPreference -eq 'Stop')

# The usage is read once per client: a phase that resolves the client for several queries costs one run of it.
foreach ($i in 1..3) { [void](Resolve-KmdClient "$Out\counting.cmd" 'health read') }
$runs = @(Get-Content -LiteralPath "$Out\runs.txt")
Check 'the usage is read once for three resolutions' ($runs.Count -eq 1) "ran $($runs.Count) times"

# The client path is <InstallRoot>\tools\bc250kmd_cli.exe, and an absent Release key says so.
Check 'the client path joins the release root' ((Get-KmdReleaseClientPath -Root 'C:\Program Files\amdgpu-wddm') -ceq 'C:\Program Files\amdgpu-wddm\tools\bc250kmd_cli.exe')
$absent = 'HKCU:\Software\bc250kmd-deploy-no-such-release-key'
Check 'the test key does not exist' (!(Test-Path -LiteralPath $absent))
Refused 'no Release key: the key is named' { Get-KmdReleaseRoot -Key $absent } 'has no InstallRoot'
Refused 'no Release key: the client path refuses too' { Get-KmdReleaseClientPath -Root ([string](Get-KmdReleaseRoot -Key $absent)) } 'has no InstallRoot'

# check-offline.py's 'legacy lab paths' check holds the other half of the rule: no file of the kit names an old lab
# directory or a KMD client by path, so no caller can get a client this resolver did not give it.
"PASS: release client resolver, $cases checks (release root, client path, usage forms, one usage read, refusals)"
