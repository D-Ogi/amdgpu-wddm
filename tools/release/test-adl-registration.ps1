# Host-only ADL manifest/registry contract. No driver install, HKLM or system DLL writes.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'installer\common.ps1')
$checks = 0
function Check([bool]$Ok, [string]$Name) {
    if (-not $Ok) { throw "FAIL $Name" }
    $script:checks++
    "PASS $Name"
}
function Fixture {
    @'
{"members":[{"member":"adl/atiadlxx.dll","arch":"x64"},{"member":"wow64/adl/atiadlxy.dll","arch":"x86"}],
 "subkey_registration":[
 {"subkey":"CopyToVmWhenNewer","name":"AmdgpuAdl","kind":"MultiString","value":["adl\\atiadlxx.dll","atiadlxx.dll"]},
 {"subkey":"CopyToVmWhenNewerWow64","name":"AmdgpuAdl","kind":"MultiString","value":["wow64\\adl\\atiadlxy.dll","atiadlxy.dll"]}]}
'@ | ConvertFrom-Json
}
Check (@(Get-DriverPackageSubkeyRegistration ([pscustomobject]@{members=@()})).Count -eq 0) 'legacy manifest remains supported'
$package = Fixture
Check (@(Get-DriverPackageSubkeyRegistration $package).Count -eq 2) 'two architecture-specific pairs'
foreach ($fault in 'missing','duplicate','absolute','traversal','redirect','wrong-arch','wrong-kind','wrong-name','extra') {
    $bad = Fixture
    switch ($fault) {
        missing { $bad.subkey_registration = @($bad.subkey_registration[0]) }
        duplicate { $bad.subkey_registration = @($bad.subkey_registration[0], $bad.subkey_registration[0]) }
        absolute { $bad.subkey_registration[0].value[0] = 'C:\foreign\atiadlxx.dll' }
        traversal { $bad.subkey_registration[0].value[0] = '..\atiadlxx.dll' }
        redirect { $bad.subkey_registration[1].value[1] = 'subdir\atiadlxy.dll' }
        wrong-arch { $bad.members[1].arch = 'x64' }
        wrong-kind { $bad.subkey_registration[0].kind = 'String' }
        wrong-name { $bad.subkey_registration[0].name = 'OtherVendor' }
        extra { $bad.subkey_registration += $bad.subkey_registration[0] }
    }
    $refused = $false
    try { Get-DriverPackageSubkeyRegistration $bad | Out-Null } catch { $refused = $true }
    Check $refused "reject $fault contract"
}
$key = 'HKCU:\Software\amdgpu-adl-registration-test-' + [Guid]::NewGuid().ToString('N')
try {
    foreach ($row in @(Get-DriverPackageSubkeyRegistration $package)) {
        $path = "$key\$($row.subkey)"
        [void](New-Item -Path $path -Force)
        New-ItemProperty -LiteralPath $path -Name $row.name -Value $row.value -PropertyType $row.kind | Out-Null
        New-ItemProperty -LiteralPath $path -Name ForeignRuntime -Value 'keep.dll' -PropertyType String | Out-Null
    }
    Check (@(Test-DriverPackageSubkeyRegistration $package $key).Count -eq 0) 'REG_MULTI_SZ round trip'
    Set-ItemProperty -LiteralPath "$key\CopyToVmWhenNewerWow64" -Name AmdgpuAdl -Value @('adl\atiadlxx.dll','atiadlxy.dll')
    $problems = @(Test-DriverPackageSubkeyRegistration $package $key)
    Check ($problems -contains 'CopyToVmWhenNewerWow64\AmdgpuAdl differs from the bound package INF') 'detect wrong persisted architecture'
    Remove-ItemProperty -LiteralPath "$key\CopyToVmWhenNewer" -Name AmdgpuAdl
    New-ItemProperty -LiteralPath "$key\CopyToVmWhenNewer" -Name AmdgpuAdl -Value 'adl\atiadlxx.dll' -PropertyType String | Out-Null
    Check (@(Test-DriverPackageSubkeyRegistration $package $key) -contains 'CopyToVmWhenNewer\AmdgpuAdl has the wrong registry type') 'detect wrong persisted registry type'
    Remove-ItemProperty -LiteralPath "$key\CopyToVmWhenNewer" -Name AmdgpuAdl
    Check (@(Test-DriverPackageSubkeyRegistration $package $key) -contains 'CopyToVmWhenNewer\AmdgpuAdl is missing') 'detect missing persisted registration'
    Check ((Get-ItemProperty -LiteralPath "$key\CopyToVmWhenNewer").ForeignRuntime -eq 'keep.dll') 'foreign registration untouched by verification'
} finally {
    if ($key -notmatch '^HKCU:\\Software\\amdgpu-adl-registration-test-[0-9a-f]{32}$') { throw 'test cleanup escaped scope' }
    if (Test-Path -LiteralPath $key) { Remove-Item -LiteralPath $key -Recurse -Force }
}
$base = Join-Path $env:windir 'System32\DriverStore\FileRepository'
$old = Join-Path $base 'bc250kmd.inf_amd64_0001\adl'
$fresh = Join-Path $base 'bc250kmd.inf_amd64_0002'
$entries = @(Get-AdlSearchPaths (Fixture) $fresh)
Check ($entries.Count -eq 2) 'both host ADL directories'
Check ($entries[1] -eq "$fresh\wow64\adl") 'x86 host directory'
$foreign = 'C:\Vendor\bin;%SystemRoot%\System32'
$next = Get-AdlMachinePathPlan -Current "$foreign;$old" -Previous @($old) -Desired $entries
Check ($next -ceq ($foreign + ';' + ($entries -join ';'))) 'upgrade replaces exact old entry and keeps vendor precedence'
Check ((Get-AdlMachinePathPlan -Current $next -Previous $entries -Desired $entries) -ceq $next) 'repair is idempotent'
Check ((Get-AdlMachinePathPlan -Current $next -Previous $entries) -ceq $foreign) 'uninstall preserves foreign PATH'
Check ((Get-AdlMachinePathPlan -Current "$foreign;$old" -Previous @($old) -Desired @()) -ceq $foreign) 'package without ADL removes old entry'
Check ((Get-AdlMachinePathPlan -Current "$foreign;" -Desired $entries).StartsWith($foreign)) 'environment expansion remains literal'
foreach ($bad in @('C:\Vendor\bin', "$fresh\adl\..\", "$fresh\d3d12", "$base\foreign.inf_1\adl", "$fresh\adl;C:\bad")) {
    $refused = $false
    try { Get-AdlMachinePathPlan -Current $foreign -Previous @($bad) | Out-Null } catch { $refused = $true }
    Check $refused "reject foreign or escaping PATH $bad"
}
"ADL registration: $checks checks passed"
