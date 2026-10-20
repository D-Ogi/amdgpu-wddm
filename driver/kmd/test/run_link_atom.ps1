param(
    [Parameter(Mandatory)][string]$Root,
    [Parameter(Mandatory)][string]$Out,
    [string]$Kits = '',
    [string]$Rom = '',
    [switch]$OldScope,
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
$oldTemp,$oldTmp=$env:TEMP,$env:TMP
$oldPath, $oldInclude, $oldLib = $env:PATH, $env:INCLUDE, $env:LIB
try {
    $env:TEMP="$Root/scratch/tmp";$env:TMP=$env:TEMP
    $env:PATH = "$(Split-Path $cl);$oldPath"
    $env:INCLUDE = ''; $env:LIB = ''
    $obj = Join-Path $Out 'link-atom-obj'
    New-Item -ItemType Directory -Force $obj | Out-Null
    $argsCl = @('/nologo','/TC','/W4','/WX','/O2','/MT',
        "/I$($msvc.FullName)/include", "/I$sdk/Include/$version/ucrt", "/Fo$obj\", "/Fe$Out/link_atom_test.exe",
        "$PSScriptRoot/link_atom_test.c", "$repo/driver/shim/bc250_link_atom.c", '/link',
        "/LIBPATH:$($msvc.FullName)/lib/x64", "/LIBPATH:$sdklib/ucrt/x64", "/LIBPATH:$sdklib/um/x64")
    & $cl @argsCl
    if ($LASTEXITCODE -ne 0) { throw 'Link ATOM host build failed' }
    & "$Out/link_atom_test.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Link ATOM parameter tests failed' }
    if($Rom) {
        Write-Output "ROM SHA256 $((Get-FileHash -LiteralPath $Rom -Algorithm SHA256).Hash)"
        $sources=@('bc250_link_atom.c','bc250_atom.c','bc250_atom_image.c','bc250_atom_preflight.c','bc250_link_regs.c','bc250_hdmi_link.c') | ForEach-Object { "$repo/driver/shim/$_" }
        $actual=@('/nologo','/TC','/W4','/WX','/O2','/MT',
            "/I$($msvc.FullName)/include", "/I$sdk/Include/$version/ucrt",
            "/I$repo/driver/shim","/I$repo/driver/kmd","/I$repo/third_party/linux-amdgpu",
            "/I$repo/driver/amdgpu-import/reference/connector",
            "/Fo$obj\", "/Fe$Out/link_clock_reinit_test.exe", "$PSScriptRoot/link_clock_reinit_test.c") + $sources + @('/link',
            "/LIBPATH:$($msvc.FullName)/lib/x64", "/LIBPATH:$sdklib/ucrt/x64", "/LIBPATH:$sdklib/um/x64")
        & $cl @actual
        if($LASTEXITCODE){throw 'Clock replay build failed'}
        # Both callers of the PHY power-up table: HDMI SetPixelClock over four mode
        # changes, and DP transmitter enable after a re-plug (BD-195).
        $replays=@(@{Name='clock';Args=@($Rom,'3','0','112','148500')},
            @{Name='dp-tx-enable';Args=@($Rom,'0','0','76','270000')})
        foreach($replay in $replays) {
            $replayArgs=$replay.Args
            if($OldScope){$replayArgs+='old-scope'}
            $result=& "$Out/link_clock_reinit_test.exe" @replayArgs
            $exit=$LASTEXITCODE
            $result | Set-Content "$Out/reinit-$($replay.Name).log"
            $result | Write-Output
            if($ExpectFailure) {
                if(!$OldScope -or $exit -ne 1 -or !($result -match '^FAIL CHECK') -or
                    !($result -match 'offset=?50172')) {throw "Old scoped policy did not fail the real ROM CHECK ($($replay.Name))"}
                Write-Output "Old scope: explicit compiled CHECK failure verified ($($replay.Name))"
            } elseif($exit) {throw "Real ROM replay failed ($($replay.Name))"}
        }
    } elseif($OldScope -or $ExpectFailure) {throw 'Negative replay requires ROM'}

} finally {
    $env:TEMP=$oldTemp;$env:TMP=$oldTmp
    $env:PATH=$oldPath; $env:INCLUDE=$oldInclude; $env:LIB=$oldLib
}
