param(
    [Parameter(Mandatory)][string]$Root,
    [Parameter(Mandatory)][string]$Out,
    [Parameter(Mandatory)][string]$Rom,
    [string]$Kits = '',
    [switch]$OldClockPolicy,
    [switch]$StrictValues,
    [switch]$ExpectFailure
)
$ErrorActionPreference = 'Stop'
if (!$Kits) { $Kits = Join-Path $Root 'toolchain/nuget' }
$repo = (Resolve-Path "$PSScriptRoot/../../..").Path
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp/c'
$sdklib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64/c'
$version = '10.0.26100.0'
$vs = & "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC/Tools/MSVC') -Directory | Sort-Object Name -Descending | Select-Object -First 1
$cl = "$($msvc.FullName)/bin/Hostx64/x64/cl.exe"
$oldPath, $oldInclude, $oldLib = $env:PATH, $env:INCLUDE, $env:LIB
$oldTemp,$oldTmp=$env:TEMP,$env:TMP
try {
    $env:TEMP="$Root/scratch/tmp";$env:TMP=$env:TEMP
    $env:PATH = "$(Split-Path $cl);$oldPath"
    $env:INCLUDE = ''; $env:LIB = ''
    $obj = Join-Path $Out 'link-active-off-obj'
    New-Item -ItemType Directory -Force $obj | Out-Null
    $argsCl = @('/nologo','/TC','/W4','/WX','/O2','/MT',
        "/I$($msvc.FullName)/include", "/I$sdk/Include/$version/ucrt", "/Fo$obj\", "/Fe$Out/link_active_off_test.exe",
        "$PSScriptRoot/link_active_off_test.c", "$repo/driver/shim/bc250_link_atom.c",
        "$repo/driver/shim/bc250_atom.c", "$repo/driver/shim/bc250_atom_image.c",
        "$repo/driver/shim/bc250_atom_preflight.c", "$repo/driver/shim/bc250_link_regs.c",
        "$repo/driver/shim/bc250_hdmi_link.c",
        "/I$repo/driver/shim", "/I$repo/driver/kmd", "/I$repo/third_party/linux-amdgpu",
        "/I$repo/driver/amdgpu-import/reference/connector", '/link',
        "/LIBPATH:$($msvc.FullName)/lib/x64", "/LIBPATH:$sdklib/ucrt/x64", "/LIBPATH:$sdklib/um/x64")
    & $cl @argsCl
    if ($LASTEXITCODE -ne 0) { throw 'Active TX_OFF host build failed' }
    Write-Output "Private ROM SHA256 $((Get-FileHash -LiteralPath $Rom -Algorithm SHA256).Hash)"
    $runArgs=@($Rom)
    if($OldClockPolicy -and $StrictValues){throw 'Select one negative control'}
    if($OldClockPolicy){$runArgs+='old-clock-policy'}
    if($StrictValues){$runArgs+='strict-values'}
    $result=& "$Out/link_active_off_test.exe" @runArgs
    $code=$LASTEXITCODE
    $result | Set-Content "$Out/result.log"
    $result | Write-Output
    if($ExpectFailure) {
        if((!$OldClockPolicy -and !$StrictValues) -or $code -ne 1 -or !($result -match '^FAIL line ')) {
            throw 'Old clock policy did not produce a CHECK failure'
        }
        if($StrictValues -and !($result -match 'table=35 offset=46053')) { throw 'Strict-value control missed retained-ROM refusal' }
        Write-Output 'Selected negative control: compiled CHECK failure verified'
    } elseif($code -ne 0) { throw 'Active TX_OFF interpreter regression failed' }
} finally {
    $env:PATH=$oldPath; $env:INCLUDE=$oldInclude; $env:LIB=$oldLib
    $env:TEMP=$oldTemp;$env:TMP=$oldTmp
}
