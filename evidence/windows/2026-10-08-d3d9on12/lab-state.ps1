# Read-only (2026-10-08, D3D9On12 x64/x86 evidence run): the D3D9 slot and the Wow entries of our display class key,
# the installed x64 and x86 D3D12 shell files, the staged d3d9probe binaries and the app-route runner, DWM and the
# KMD start confirmation.
$ErrorActionPreference = 'Continue'
$cls = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}'
$key = Get-ChildItem $cls -ErrorAction SilentlyContinue | Where-Object { (Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue).UserModeDriverName -match 'bc250|amdgpu' } | Select-Object -First 1
$p = Get-ItemProperty $key.PSPath
'class key ' + $key.PSChildName
'UserModeDriverName    = [' + (($p.UserModeDriverName | ForEach-Object { "'$_'" }) -join ', ') + ']'
'UserModeDriverNameWow = [' + (($p.UserModeDriverNameWow | ForEach-Object { "'$_'" }) -join ', ') + ']'
foreach ($f in @($p.UserModeDriverName) + @($p.UserModeDriverNameWow)) {
    if ($f -and (Test-Path -LiteralPath $f)) { 'file {0} {1}' -f $f, (Get-FileHash -LiteralPath $f).Hash.Substring(0, 8) }
    elseif ($f) { "file $f ABSENT" }
}
Get-ChildItem 'C:\Program Files\amdgpu-wddm' -Recurse -File -Include '*.dll' -ErrorAction SilentlyContinue |
    Where-Object { $_.DirectoryName -match 'x86|wow|32' } | ForEach-Object { 'x86 dir file {0} {1}' -f $_.FullName, (Get-FileHash -LiteralPath $_.FullName).Hash.Substring(0, 8) }
foreach ($f in 'C:\BC250\tmp\d3d9probe\d3d9probe.exe', 'C:\BC250\tmp\d3d9probe\x86\d3d9probe.exe', 'C:\BC250\m14\app-route-001\ops\app-run.ps1') {
    if (Test-Path -LiteralPath $f) { 'staged {0} {1}' -f $f, (Get-FileHash -LiteralPath $f).Hash.Substring(0, 8) } else { "staged $f ABSENT" }
}
"dwm $((Get-Process dwm -ErrorAction SilentlyContinue | ForEach-Object { $_.Id }) -join ',')"
"boot $((Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o'))"
& 'C:\Program Files\amdgpu-wddm\tools\bc250kmd_cli.exe' health read 2>&1 | Select-Object -First 1
"d3d9on12.dll x64 $(Test-Path C:\Windows\System32\d3d9on12.dll) x86 $(Test-Path C:\Windows\SysWOW64\d3d9on12.dll)"
