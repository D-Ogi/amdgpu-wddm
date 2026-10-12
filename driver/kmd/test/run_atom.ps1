param(
    [Parameter(Mandatory)][string]$Root,
    [Parameter(Mandatory)][string]$Out,
    [string]$Kits = "$Root\toolchain\nuget",
    [string]$PrivateRom = ''
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot\..\..\..").Path
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdklib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'
$kitVersion = '10.0.26100.0'
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name -Descending | Select-Object -First 1
$cl = "$($msvc.FullName)\bin\Hostx64\x64\cl.exe"
$env:PATH = "$(Split-Path $cl);$env:PATH"
$env:INCLUDE = ''; $env:LIB = ''
$obj = Join-Path $Out 'obj'
New-Item -ItemType Directory -Force $obj | Out-Null
& python -B "$repo\tools\import\extract_hdmi_policy.py" --check
if ($LASTEXITCODE -ne 0) { throw 'HDMI field policy is stale or incomplete' }
& python -B "$repo\tools\import\extract_atom.py" --check
if ($LASTEXITCODE -ne 0) { throw 'ATOM layouts are stale' }
$sources = @("$PSScriptRoot\atom_image_test.c", "$repo\driver\shim\bc250_atom_image.c", "$repo\driver\shim\bc250_atom_preflight.c", "$repo\driver\shim\bc250_atom.c", "$repo\driver\shim\bc250_atom_firmware.c")
$clArgs = @('/nologo','/TC','/W4','/WX','/O2','/MT',
    "/I$($msvc.FullName)\include", "/I$sdk\Include\$kitVersion\ucrt", "/Fo$obj\", "/Fe$Out\atom_image_test.exe") + $sources + @(
    '/link', "/LIBPATH:$($msvc.FullName)\lib\x64", "/LIBPATH:$sdklib\ucrt\x64", "/LIBPATH:$sdklib\um\x64")
& $cl @clArgs
if ($LASTEXITCODE -ne 0) { throw 'ATOM image test build failed' }
& "$Out\atom_image_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'ATOM image tests failed' }
if ($PrivateRom) {
    & "$Out\atom_image_test.exe" $PrivateRom
    if ($LASTEXITCODE -ne 0) { throw 'ATOM private image tests failed' }
}
& python -B "$repo\tools\import\extract_hdmi_link.py" --check
if ($LASTEXITCODE -ne 0) { throw 'HDMI stream extracts are stale' }
$linkSources = @("$PSScriptRoot\hdmi_link_test.c", "$repo\driver\shim\bc250_hdmi_link.c",
    "$repo\driver\shim\bc250_link_atom.c", "$repo\driver\shim\bc250_link_regs.c",
    "$repo\driver\shim\bc250_atom_image.c", "$repo\driver\shim\bc250_atom_preflight.c", "$repo\driver\shim\bc250_atom.c")
$linkArgs = @('/nologo','/TC','/W4','/WX','/O2','/MT',
    "/I$repo\third_party\linux-amdgpu", "/I$($msvc.FullName)\include", "/I$sdk\Include\$kitVersion\ucrt",
    "/Fo$obj\", "/Fe$Out\hdmi_link_test.exe") + $linkSources + @('/link',
    "/LIBPATH:$($msvc.FullName)\lib\x64", "/LIBPATH:$sdklib\ucrt\x64", "/LIBPATH:$sdklib\um\x64")
& $cl @linkArgs
if ($LASTEXITCODE -ne 0) { throw 'HDMI link test build failed' }
& "$Out\hdmi_link_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'HDMI link tests failed' }
if ($PrivateRom) {
    & "$Out\hdmi_link_test.exe" $PrivateRom
    if ($LASTEXITCODE -ne 0) { throw 'HDMI private image tests failed' }
    # Negative control: the actual production function replaced by the former
    # full-word verification rule must fail on the same private-ROM model.
    $source = Get-Content "$repo\driver\shim\bc250_hdmi_link.c" -Raw
    $needle = 'return p?p->Configuration:0;'
    if (($source.Split(@($needle),[StringSplitOptions]::None)).Count -ne 2) { throw 'Verifier mutation is not unique' }
    $mutant = Join-Path $Out 'hdmi_link_fullword_mutant.c'
    [IO.File]::WriteAllText($mutant,$source.Replace($needle,'(void)p;return UINT32_MAX;'))
    $mutantArgs = @($linkArgs | ForEach-Object {
        if ($_ -eq "$repo\driver\shim\bc250_hdmi_link.c") { $mutant }
        elseif ($_ -eq "/Fe$Out\hdmi_link_test.exe") { "/Fe$Out\hdmi_link_fullword_mutant.exe" }
        else { $_ }
    })
    $mutantArgs = @("/I$repo\driver\shim") + $mutantArgs
    & $cl @mutantArgs
    if ($LASTEXITCODE -ne 0) { throw 'Verifier negative control did not compile' }
    & "$Out\hdmi_link_fullword_mutant.exe" $PrivateRom --positive-only > "$Out\fullword-mutant.txt"
    if ($LASTEXITCODE -ne 1 -or !(Select-String -LiteralPath "$Out\fullword-mutant.txt" -Pattern 'Positive-only: [1-9][0-9]* failures' -Quiet)) {
        throw 'Full-word verifier negative control did not fail as expected'
    }
    Write-Output 'Full-word verifier mutant: rejected as expected'
}
# Executable negative controls: restore the unsupported data-echo assumption,
# omit the commit, or omit packet stop before returning clocks/power. Each must
# compile and execute a meaningful failure in the actual production harness.
$source = Get-Content "$repo\driver\shim\bc250_hdmi_link.c" -Raw
$controls = @(
    @{
        Name = 'packet-echo'; Needle = 'return 0; // Packet data callback accepted, not readback-verified.'
        Replacement = 'if(l->Io.Read(l->Io.Context,reg,&after) || after!=v) { l->FailureRegister=reg;l->FailureOperation=4;l->FailureWanted=v;l->FailureReadback=after;l->FailureMask=UINT32_MAX;return l->Error=-1; } return 0;'
        Pattern = 'Packet lifecycle: [0-9]+ checks, [1-9][0-9]* failures'
    },
    @{
        Name = 'packet-no-commit'; Needle = 'v &= ~policy->Status;'
        Replacement = 'if(reg==R(AFMT_VBI_PACKET_CONTROL1))return 0; v &= ~policy->Status;'
        Pattern = 'Packet lifecycle: [0-9]+ checks, [1-9][0-9]* failures'
    },
    @{
        Name = 'packet-no-stop'; Needle = 'if(l->PacketDirty && stopPacket0(l))result=-1;'
        Replacement = 'if(!l->PacketDirty && stopPacket0(l))result=-1;'
        Pattern = 'Restore protocol: [0-9]+ checks, [1-9][0-9]* failures'
    },
    @{
        Name = 'packet-no-retire'; Needle = 'if(retirePacket0(l))return streamResult(l);'
        Replacement = 'if(l->Dirty && retirePacket0(l))return streamResult(l);'
        Pattern = 'Restart packet lifecycle: [0-9]+ checks, [1-9][0-9]* failures'
    }
)
foreach ($control in $controls) {
    if (($source.Split(@($control.Needle),[StringSplitOptions]::None)).Count -ne 2) {
        throw "Negative control is not unique: $($control.Name)"
    }
    $mutant = Join-Path $Out "$($control.Name)-mutant.c"
    [IO.File]::WriteAllText($mutant,$source.Replace($control.Needle,$control.Replacement))
    $mutantArgs = @($linkArgs | ForEach-Object {
        if ($_ -eq "$repo\driver\shim\bc250_hdmi_link.c") { $mutant }
        elseif ($_ -eq "/Fe$Out\hdmi_link_test.exe") { "/Fe$Out\$($control.Name)-mutant.exe" }
        else { $_ }
    })
    $mutantArgs = @("/I$repo\driver\shim") + $mutantArgs
    & $cl @mutantArgs
    if ($LASTEXITCODE -ne 0) { throw "Negative control did not compile: $($control.Name)" }
    $testArgs = @()
    if ($PrivateRom) { $testArgs = @($PrivateRom) }
    & "$Out\$($control.Name)-mutant.exe" @testArgs > "$Out\$($control.Name)-mutant.txt"
    if ($LASTEXITCODE -ne 1 -or !(Select-String -LiteralPath "$Out\$($control.Name)-mutant.txt" -Pattern $control.Pattern -Quiet)) {
        throw "Negative control did not execute and fail expected checks: $($control.Name)"
    }
    Write-Output "$($control.Name) mutant: rejected as expected"
}
exit 0
