param([Parameter(Mandatory=$true)][string]$Root,[Parameter(Mandatory=$true)][string]$Out,
 [ValidateSet('none','admission','cleanup','poison')][string]$Mutation='none')
$ErrorActionPreference='Stop'
$repo=(Resolve-Path "$PSScriptRoot/../../..").Path
New-Item -ItemType Directory -Force $Out | Out-Null
$Out=(Resolve-Path $Out).Path;$env:TEMP=$Out;$env:TMP=$Out
$pnp=Get-Content -Raw "$repo/driver/kmd/pnp.c"
function FunctionBody([string]$Name) {
 $start=$pnp.IndexOf("NTSTATUS $Name(");if($start -lt 0){throw "Missing $Name"}
 $brace=$pnp.IndexOf('{',$start);$level=1;$end=$brace+1
 while($level -gt 0 -and $end -lt $pnp.Length){if($pnp[$end] -eq '{'){$level++};if($pnp[$end] -eq '}'){$level--};$end++}
 if($level){throw 'Unbalanced function'}
 return $pnp.Substring($start,$end-$start)
}
# Entry-point placement controls complement execution of actual Add/Stop/Remove.
foreach($name in @('Bc250StartDevice','Bc250StopDevice','Bc250RemoveDevice','Bc250SetPowerState','Bc250StopDeviceAndReleasePostDisplayOwnership')) {
 $body=FunctionBody $name
 if($body.IndexOf('AdapterOwnerIs') -lt 0){throw "Missing owner admission in $name"}
}
$startBody=FunctionBody 'Bc250StartDevice'
if($startBody.IndexOf('AdapterOwnerIs') -gt $startBody.IndexOf('BoardMemoryIdentityClear')){throw 'Start mutates before admission'}
$owner=(Get-Content -Raw "$repo/driver/kmd/adapter_owner.c").Replace('#include <ntddk.h>','')
$actual=(FunctionBody 'Bc250AddDevice')+"`n"+(FunctionBody 'Bc250StopDevice')+"`n"+(FunctionBody 'Bc250RemoveDevice')
switch($Mutation) {
 'admission' {$actual=$actual.Replace('if (!AdapterOwnerClaim(PhysicalDeviceObject)) return STATUS_DEVICE_BUSY;', '(void)AdapterOwnerClaim(PhysicalDeviceObject);')}
 'cleanup' {$actual=$actual.Replace('if (!device || !AdapterOwnerIs(device->PhysicalDeviceObject)) return STATUS_SUCCESS;', 'if (!device) return STATUS_SUCCESS;')}
 'poison' {$owner=$owner.Replace('uncertain ? &AdapterPoison : NULL','(uncertain && 0) ? &AdapterPoison : NULL')}
}
$template=Get-Content -Raw "$PSScriptRoot/adapter_owner_test.c"
Set-Content -Encoding ascii "$Out/actual.c" $template.Replace('/* ACTUAL_OWNER */',$owner).Replace('/* ACTUAL_PNP */',$actual)
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk="$Root\toolchain\nuget\microsoft.windows.sdk.cpp\c";$libs="$Root\toolchain\nuget\microsoft.windows.sdk.cpp.x64\c"
& "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /TC /W4 /WX /O2 /MT "/I$repo\driver\kmd" "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/I$sdk\Include\10.0.26100.0\um" "/I$sdk\Include\10.0.26100.0\shared" "/Fo$Out\" "/Fe$Out\adapter_owner_test.exe" "$Out\actual.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64" kernel32.lib
if($LASTEXITCODE -ne 0){throw 'Adapter owner test compile failed'}
$lines=@(& "$Out/adapter_owner_test.exe");$code=$LASTEXITCODE;$lines | Tee-Object "$Out/test.log"
$hashes=@{};foreach($path in @('driver/kmd/adapter_owner.c','driver/kmd/adapter_owner.h','driver/kmd/pnp.c','driver/kmd/test/adapter_owner_test.c')){$hashes[$path]=(Get-FileHash "$repo/$path").Hash}
@{Sources=$hashes;Mutation=$Mutation;ExitCode=$code;Output=$lines} | ConvertTo-Json -Depth 4 | Set-Content "$Out/record.json"
if($Mutation -eq 'none'){exit $code}
if($code -ne 1 -or -not ($lines -match '^FAIL CHECK')){throw 'Mutation did not fail runtime checks'}
exit 0
