#Requires -Version 7.0
# Host test of Get-Bc250ReproducibleFlags (umd-compiler.ps1, BD-268): one fixture DLL of two C++ files, built with
# clang-cl and lld-link /Brepro /DEBUG from two copies of its sources at different paths into two output directories
# of different lengths, the way the D3D11 shell and the control recipes build. The two images must be the same bytes.
# The control builds the same fixture the old way (no /Fo, so clang-cl names its objects with random temporary names,
# absolute paths, no flags of the helper) and must give two different images, so the check can see the difference.
# The test also reads the two recipes and checks that they use the helper the same way.
#   pwsh -File tools\build\test-pdb-repro.ps1 -Out <dir>     (the 'pdb-repro' gate of quick.ps1)
param([Parameter(Mandatory)][string]$Out, [string]$VsInstall)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\common.ps1"
. "$PSScriptRoot\umd-compiler.ps1"
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$root = Get-Bc250Root $repo
$base = Join-Path ([IO.Path]::GetFullPath($Out)) ('pdb-repro-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $base | Out-Null
$passed = 0
function Assert([bool]$Ok, [string]$What) { if (-not $Ok) { throw "FAIL: $What" }; $script:passed++ }

# The fixture sources, the same bytes in two trees.
$a = "#include <windows.h>`nextern `"C`" __declspec(dllexport) int Bc250Add(int x) { OutputDebugStringA(__FILE__); return x + 1; }`n"
$b = "#include <string>`nextern `"C`" __declspec(dllexport) int Bc250Length(const char* s) { return (int)std::string(s).size(); }`n"
$trees = @((Join-Path $base 'tree-a'), (Join-Path $base 'tree-with-a-longer-name'))
foreach ($t in $trees) {
    New-Item -ItemType Directory -Force "$t\src" | Out-Null
    [IO.File]::WriteAllText("$t\src\first.cpp", $a); [IO.File]::WriteAllText("$t\src\second.cpp", $b)
    [IO.File]::WriteAllText("$t\src\fixture.def", "EXPORTS`n    Bc250Add`n    Bc250Length`n")
}

$saved = Save-ProcessEnvironment
try {
    $env:TEMP = $base; $env:TMP = $base
    $null = Import-VsDevEnvironment -VsInstall $VsInstall -TempDir $base -Arch x64
    $cc = Resolve-Bc250UmdCompiler $root 'clang-cl' '' $base 'x64'
    function Build-Fixture([string]$Tree, [string]$Dir, [switch]$Old) {
        New-Item -ItemType Directory -Force "$Dir\obj" | Out-Null
        $flags = ConvertTo-Bc250ClangFlags @('/nologo', '/std:c++20', '/EHsc', '/W4', '/WX', '/O2', '/MT', '/Brepro', '/Zi', '/DNOMINMAX') 'x64' @($Tree)
        Push-Location -LiteralPath $Dir
        try {
            if ($Old) {
                Invoke-Bc250UmdCompiler $cc @flags /LD "/Fe:$Dir\fixture.dll" "$Tree\src\first.cpp" "$Tree\src\second.cpp" `
                    /link /Brepro /DEBUG /OPT:REF /OPT:ICF '/PDBALTPATH:%_PDB%' "/DEF:$Tree\src\fixture.def" | Out-Host
            } else {
                $repro = Get-Bc250ReproducibleFlags 'clang-cl'
                Copy-Item -LiteralPath "$Tree\src\fixture.def" -Destination "$Dir\obj\fixture.def"
                Invoke-Bc250UmdCompiler $cc @flags @($repro.Compile) /LD /Foobj\ /Fe:fixture.dll "$Tree\src\first.cpp" "$Tree\src\second.cpp" `
                    /link /Brepro /DEBUG /OPT:REF /OPT:ICF @($repro.Link) '/PDBALTPATH:%_PDB%' '/DEF:obj\fixture.def' | Out-Host
            }
            if ($LASTEXITCODE) { throw "fixture build in $Dir failed ($LASTEXITCODE)" }
        } finally { Pop-Location }
        return (Get-FileHash -LiteralPath "$Dir\fixture.dll").Hash
    }
    $one = Build-Fixture $trees[0] (Join-Path $base 'out-1')
    $two = Build-Fixture $trees[1] (Join-Path $base 'out-number-two')
    "reproducible: $($one.Substring(0, 8)) and $($two.Substring(0, 8))"
    Assert ($one -eq $two) 'two trees and two output directories give one image'
    $oldOne = Build-Fixture $trees[0] (Join-Path $base 'old-1') -Old
    $oldTwo = Build-Fixture $trees[0] (Join-Path $base 'old-2') -Old
    "control (the old way, one tree): $($oldOne.Substring(0, 8)) and $($oldTwo.Substring(0, 8))"
    Assert ($oldOne -ne $oldTwo) 'the control: the old way gives two images, so the check above can see a difference'
} finally { Restore-ProcessEnvironment $saved }

# The recipes use the helper with relative paths.
$dxvk = [IO.File]::ReadAllText("$repo\tools\build\build-umd-dxvk.ps1")
Assert ($dxvk.Contains('$repro=Get-Bc250ReproducibleFlags $Compiler') -and $dxvk.Contains('@($repro.Compile) @flavourDefine @includes /LD /Foobj\ /Fe:amdgpu_wddm_d3d11.dll') -and
        $dxvk.Contains("@(`$repro.Link) '/PDBALTPATH:%_PDB%' '/DEF:obj\amdgpu_wddm_d3d11.def'")) 'build-umd-dxvk.ps1 uses the helper, obj\ and a relative .def'
$control = [IO.File]::ReadAllText("$repo\tools\win\amdgpu_wddm_control\build.ps1")
Assert ($control.Contains("(Get-Bc250ReproducibleFlags 'clang-cl').Compile") -and $control.Contains('$clangLink = @((Get-Bc250ReproducibleFlags $Compiler).Link)') -and
        $control.Contains("'/Febc250control.dll'") -and $control.Contains("'/Febc250kmd_cli.exe'") -and $control.Contains('-RelativeObjects')) 'the control recipe uses the helper and relative paths'
Remove-Item -LiteralPath $base -Recurse -Force
"PASS: reproducible PDB, $passed checks (BD-268)"
