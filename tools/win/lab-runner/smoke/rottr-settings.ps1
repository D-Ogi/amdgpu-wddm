# LAB (elevated SSH, read-only unless -SetDx12): prints the value names and values of Rise of the Tomb Raider's
# own graphics settings key for the logged-on lab user (HKU\<SID of bc250>), the test game only. -SetDx12 1 turns the
# game's DirectX 12 option on (value EnableDX12, as the lead's listing of 2026-10-01 shows it; DX12 as a fallback),
# with a backup of the previous value printed first.
param([int]$SetDx12 = -1)
$ErrorActionPreference = 'Stop'
$sid = (New-Object System.Security.Principal.NTAccount('bc250')).Translate([System.Security.Principal.SecurityIdentifier]).Value
if (-not (Test-Path "Registry::HKEY_USERS\$sid")) { "user hive not loaded (bc250 not logged on?)"; exit 2 }
$base = "Registry::HKEY_USERS\$sid\Software\Crystal Dynamics\Rise of the Tomb Raider"
if (-not (Test-Path $base)) { "no settings key yet (the game never ran for bc250)"; exit 0 }
Get-ChildItem $base -Recurse | ForEach-Object {
    $k = $_
    "[{0}]" -f ($k.Name -replace '^HKEY_USERS\\[^\\]+\\', 'HKU\bc250\')
    foreach ($n in $k.GetValueNames()) { "  {0} = {1}" -f $n, $k.GetValue($n) }
}
if ($SetDx12 -ge 0) {
    $g = $null; $v = $null
    foreach ($n in 'EnableDX12', 'DX12') {
        $g = Get-ChildItem $base -Recurse | Where-Object { $_.GetValueNames() -contains $n } | Select-Object -First 1
        if ($g) { $v = $n; break }
    }
    if (-not $g) { "no EnableDX12 or DX12 value found: nothing changed"; exit 3 }
    $path = 'Registry::' + $g.Name
    "before {0} = {1} at {2}" -f $v, $g.GetValue($v), ($g.Name -replace '^HKEY_USERS\\[^\\]+\\', 'HKU\bc250\')
    Set-ItemProperty -Path $path -Name $v -Value $SetDx12 -Type DWord
    "after {0} = {1}" -f $v, (Get-ItemProperty -Path $path -Name $v).$v
}
