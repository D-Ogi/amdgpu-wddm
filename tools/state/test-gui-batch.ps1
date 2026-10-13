param([Parameter(Mandatory=$true)][string]$Out,[Parameter(Mandatory=$true)][string]$Kits,[switch]$BuildOnly)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path "$PSScriptRoot\..\..").Path
$gui=Join-Path $repo 'tools\win\amdgpu_wddm_control'
New-Item -ItemType Directory -Force "$Out\tmp" | Out-Null
$env:TEMP="$Out\tmp";$env:TMP=$env:TEMP
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk=Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$libs=Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'
$env:INCLUDE="$($msvc.FullName)\include;$sdk\Include\10.0.26100.0\ucrt;$sdk\Include\10.0.26100.0\shared;$sdk\Include\10.0.26100.0\um"
$env:LIB="$($msvc.FullName)\lib\x64;$libs\ucrt\x64;$libs\um\x64"
& "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /TC /W4 /WX /O2 /MT /LD /DAMDGPU_STATE_ADAPTER_TEST `
    /DAMDGPU_STATE_GUI_FIXTURE /DAMDGPU_CLIENT_EXPORTS "/Fo$Out\" "/Fe$Out\bc250control.dll" `
    "$repo\driver\state\state.c" "$repo\driver\state\transfer.c" "$repo\driver\state\windows_lock.c" `
    "$repo\driver\state\windows.c" "$repo\driver\state\client.c" "$repo\driver\state\windows_test.c" `
    /link advapi32.lib ole32.lib onecore.lib "/IMPLIB:$Out\bc250control.lib"
if($LASTEXITCODE){throw 'GUI batch native fixture compile failed'}
$production=[IO.File]::ReadAllText("$gui\src\RecoveryActions.cs")
$log='try { File.AppendAllText(RecoveryProbe.ActionsLog, line + Environment.NewLine); } catch (Exception) { }'
if(([regex]::Matches($production,[regex]::Escape($log))).Count -ne 1){throw 'GUI log sink changed; review fixture isolation'}
# All backup/read/write/rollback code is compiled unchanged. Only the log file
# sink is removed: this fixture emits stdout and cannot write ProgramData.
[IO.File]::WriteAllText("$Out\RecoveryActions.fixture.cs",$production.Replace($log,'// Fixture: stdout only.'))
$sources=@((Get-ChildItem "$gui\src\*.cs").FullName | Where-Object { [IO.Path]::GetFileName($_) -ne 'RecoveryActions.cs' })
$fx="$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319"
$refs='mscorlib.dll','System.dll','System.Core.dll','System.Drawing.dll','System.Windows.Forms.dll','System.IO.Compression.dll','System.Management.dll','System.Web.Extensions.dll' |
    ForEach-Object {"/reference:$fx\$_"}
& "$vs\MSBuild\Current\Bin\Roslyn\csc.exe" /nologo /noconfig /nostdlib+ @refs /target:exe /platform:x64 /warnaserror+ `
    /langversion:7.3 /deterministic+ /main:GuiBatchTest "/out:$Out\gui-batch-test.exe" @sources `
    "$Out\RecoveryActions.fixture.cs" "$PSScriptRoot\gui_batch_test.cs"
if($LASTEXITCODE){throw 'GUI batch managed fixture compile failed'}
if(!$BuildOnly) {
    & "$Out\gui-batch-test.exe" "$repo\driver\state\amdgpu-state.bin"
    if($LASTEXITCODE){throw 'GUI batch fixture failed'}
}
