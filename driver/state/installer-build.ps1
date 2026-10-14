param([Parameter(Mandatory=$true)][string]$Out,[Parameter(Mandatory=$true)][string]$Kits,[switch]$Test,
      # BD-166: the version resource this build writes into the program it ships, as the step after the link.
      # -Version is the package version, which the release passes to every component build it runs; -Flavour is
      # that release's flavour. Without -Version nothing is stamped and the build says so: a host build
      # and a quality gate are not a release, and the release reads every resource back before it ships.
      [string]$Version='',[ValidateSet('release','lab')][string]$Flavour='release')
$ErrorActionPreference='Stop'
$Out=[IO.Path]::GetFullPath($Out)
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
New-Item -ItemType Directory -Force "$Out\tmp" | Out-Null
$env:TEMP="$Out\tmp"; $env:TMP=$env:TEMP
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk=Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$libs=Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'
$env:INCLUDE="$($msvc.FullName)\include;$sdk\Include\10.0.26100.0\ucrt;$sdk\Include\10.0.26100.0\shared;$sdk\Include\10.0.26100.0\um"
$env:LIB="$($msvc.FullName)\lib\x64;$libs\ucrt\x64;$libs\um\x64"
$inputs=@('state','transfer','identity','windows','windows_lock','windows_migration','windows_handoff','installer_rollback','installer_store','installer_worker') |
    ForEach-Object { Join-Path $PSScriptRoot "$_.c" }
# BD-167: the release flavour compiles the shipped program with NDEBUG, so an assert() added later is gone from the
# release and live in the lab build.
$releaseDefine=@(if($Flavour -ne 'lab'){'/DNDEBUG'})
& "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /TC /W4 /WX /O2 /MT /Brepro /FC @releaseDefine "/d1trimfile:$repo\" "/Fo$Out\" "/Fe$Out\amdgpu-state-installer.exe" @inputs `
    /link /Brepro '/PDBALTPATH:%_PDB%' /DYNAMICBASE /NXCOMPAT onecore.lib advapi32.lib setupapi.lib cfgmgr32.lib uuid.lib bcrypt.lib newdev.lib
if($LASTEXITCODE){throw 'Installer state worker compile failed'}
# BD-166: the release ships this program (payload\tools\amdgpu-state-installer.exe) and rewrites no PE file, so
# the version resource is written here, by the build that produced it.
& pwsh -NoProfile -File (Join-Path $repo 'tools\build\version-stamp.ps1') -Version $Version -Flavour $Flavour "$Out\amdgpu-state-installer.exe"
if($LASTEXITCODE){throw 'the version stamp of amdgpu-state-installer.exe failed'}
# Only --catalog is permitted in host gates. --settings-snapshot reads the target
# device and must not run on the development PC. Service verbs change SCM too.
if($Test) {
    $testInputs=@('state','transfer','installer_rollback','installer_worker','installer_worker_test') |
        ForEach-Object {Join-Path $PSScriptRoot "$_.c"}
    & "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /TC /W4 /WX /O2 /MT /DAMDGPU_INSTALLER_TEST `
        "/Fo$Out\" "/Fe$Out\installer_worker_test.exe" @testInputs
    if($LASTEXITCODE){throw 'Installer worker fixture compile failed'}
    & "$Out\installer_worker_test.exe"
    if($LASTEXITCODE){throw 'Installer worker fixture tests failed'}
    & "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /TC /W4 /WX /O2 /MT `
        "/Fo$Out\" "/Fe$Out\installer_store_test.exe" "$PSScriptRoot\state.c" "$PSScriptRoot\transfer.c" `
        "$PSScriptRoot\installer_rollback.c" "$PSScriptRoot\installer_store_test.c" /link advapi32.lib bcrypt.lib
    if($LASTEXITCODE){throw 'Installer store fixture compile failed'}
    & "$Out\installer_store_test.exe"
    if($LASTEXITCODE){throw 'Installer store fixture tests failed'}
    & "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /TC /W4 /WX /O2 /MT `
        "/Fo$Out\" "/Fe$Out\installer_legacy_test.exe" "$PSScriptRoot\state.c" "$PSScriptRoot\transfer.c" `
        "$PSScriptRoot\installer_rollback.c" "$PSScriptRoot\installer_legacy_test.c" /link advapi32.lib
    if($LASTEXITCODE){throw 'Legacy rollback adapter fixture compile failed'}
    & "$Out\installer_legacy_test.exe"
    if($LASTEXITCODE){throw 'Legacy rollback adapter fixture tests failed'}
}
