param([Parameter(Mandatory)][string]$Out)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\state-package.ps1"
$repo=Split-Path (Split-Path $PSScriptRoot)
$source=Join-Path $repo 'driver\state\amdgpu-state.bin'
$pkg=Join-Path $Out 'package'
New-Item -ItemType Directory -Force "$pkg\payload\kmd","$pkg\payload\tools" | Out-Null
Copy-Item -LiteralPath $source -Destination "$pkg\payload\kmd\amdgpu-state.bin"
# The runner is a fixture; neither fake executable is launched.
[IO.File]::WriteAllText("$pkg\payload\tools\bc250kmd_cli.exe",'fixture')
[IO.File]::WriteAllText("$pkg\payload\tools\amdgpu-state-installer.exe",'fixture')
$bytes=[IO.File]::ReadAllBytes($source)
$count=[BitConverter]::ToUInt32($bytes,12)
$reply=@{abi=1;schema=2;error=0;catalog_id=[Text.Encoding]::ASCII.GetString($bytes,16,64);items=@(1..$count)}
$workerReply=@{abi=1;schema=2;error=0;catalog_id=$reply.catalog_id;count=$count}
$run={param($exe,$arguments)
    if([IO.Path]::GetFileName($exe) -eq 'amdgpu-state-installer.exe') {
        if(($arguments -join ' ') -cne '--catalog'){throw 'unsafe installer command in package test'}
        return @{code=0;text=($workerReply | ConvertTo-Json -Depth 5)}
    }
    if(($arguments -join ' ') -cne 'state catalog'){throw 'unexpected CLI command in package test'}
    return @{code=0;text=($reply | ConvertTo-Json -Depth 5)}
}
$checks=0
function Reject([scriptblock]$Body,[string]$Name) {
    $failed=$false;try {& $Body | Out-Null}catch {$failed=$true}
    if(-not $failed){throw "accepted invalid package: $Name"};$script:checks++
}
[void](Test-DriverStatePackage $pkg $source $run);$checks++
$reply.catalog_id='0'*64
Reject {Test-DriverStatePackage $pkg $source $run} 'stale CLI'
$reply.catalog_id=[Text.Encoding]::ASCII.GetString($bytes,16,64)
$reply.schema=1;Reject {Test-DriverStatePackage $pkg $source $run} 'old schema';$reply.schema=2
$reply.error=5;Reject {Test-DriverStatePackage $pkg $source $run} 'error';$reply.error=0
$reply.items=@(1);Reject {Test-DriverStatePackage $pkg $source $run} 'incomplete catalog';$reply.items=@(1..$count)
Reject {Test-DriverStatePackage $pkg $source { @{code=3;text='failed'} }} 'CLI exit'
Reject {Test-DriverStatePackage $pkg $source { @{code=0;text='not JSON'} }} 'CLI protocol'
$workerReply.catalog_id='0'*64;Reject {Test-DriverStatePackage $pkg $source $run} 'stale SYSTEM worker'
$workerReply.catalog_id=$reply.catalog_id
$workerReply.error=5;Reject {Test-DriverStatePackage $pkg $source $run} 'worker catalog error';$workerReply.error=0
$workerReply.count=1;Reject {Test-DriverStatePackage $pkg $source $run} 'worker catalog count';$workerReply.count=$count
$bytes[$bytes.Length-1]=$bytes[$bytes.Length-1] -bxor 1
[IO.File]::WriteAllBytes("$pkg\payload\kmd\amdgpu-state.bin",$bytes)
Reject {Test-DriverStatePackage $pkg $source $run} 'altered default bytes'
Copy-Item -LiteralPath $source -Destination "$pkg\payload\kmd\amdgpu-state.bin" -Force
Move-Item -LiteralPath "$pkg\payload\tools\amdgpu-state-installer.exe" -Destination "$pkg\payload\tools\held-worker.exe" -Force
Reject {Test-DriverStatePackage $pkg $source $run} 'missing SYSTEM worker'
"state package: $checks checks PASS"
