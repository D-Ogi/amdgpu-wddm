#Requires -Version 7.0
# Host test of quality-env.ps1 and of its use in quick.ps1: a run without BC250_UMD_SOURCE or BC250_UMD_BUILD stops
# before its first gate with one line that names the missing variable, and a run without the developer environment
# imports it itself.
#   pwsh -File tools\quality\test-quality-env.ps1 -Out <dir>     (the 'quality-env' gate of quick.ps1)
param([Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'quality-env.ps1')
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$base = Join-Path ([IO.Path]::GetFullPath($Out)) ('quality-env-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $base | Out-Null
$passed = 0
$budgetSeconds = 570; $budgetEnd = [DateTime]::UtcNow.AddSeconds($budgetSeconds)
function Assert([bool]$Ok, [string]$What) { if (-not $Ok) { throw "FAIL: $What" }; $script:passed++ }

# A child pwsh with some variables removed or changed. Returns the exit code and the output lines.
function Invoke-Child([string]$Command, [hashtable]$Change) {
    $psi = [Diagnostics.ProcessStartInfo]::new('pwsh')
    foreach ($a in @('-NoProfile', '-NonInteractive', '-Command', $Command)) { $psi.ArgumentList.Add($a) }
    foreach ($k in $Change.Keys) { if ($null -eq $Change[$k]) { [void]$psi.Environment.Remove($k) } else { $psi.Environment[$k] = $Change[$k] } }
    $psi.UseShellExecute = $false; $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
    $p = [Diagnostics.Process]::Start($psi)
    $err = $p.StandardError.ReadToEndAsync()
    $text = $p.StandardOutput.ReadToEndAsync()
    # One time budget for the whole test, not one limit per child: the gate's deadline (600 s in gate-inputs.json)
    # is the authority. A limit of 240 s per child stopped the lab package of tester.39 rc1 at 333 s, in a full
    # suite beside two other builds, although the gate still had 267 s.
    $left = [int][Math]::Max(1000, ($script:budgetEnd - [DateTime]::UtcNow).TotalMilliseconds)
    if (-not $p.WaitForExit($left)) {
        $p.Kill($true); $p.WaitForExit()
        throw "FAIL: a child did not end in the test's time budget ($script:budgetSeconds s from the start): $($Command.Substring(0, [Math]::Min(120, $Command.Length)))"
    }
    return [pscustomobject]@{ Code = $p.ExitCode; Lines = @(($text.Result + $err.Result) -split "`r?`n" | Where-Object { $_ -match '\S' }) }
}

# The refusal: the missing variable by name, both when both are missing, a missing directory by name and path.
$dir = (New-Item -ItemType Directory -Force (Join-Path $base 'tree')).FullName
$r = Get-QualityInputRefusal @{ BC250_UMD_SOURCE = ''; BC250_UMD_BUILD = $null }
Assert ($r -match '^BC250_UMD_SOURCE and BC250_UMD_BUILD are not set: set them to') "both unset: $r"
$r = Get-QualityInputRefusal @{ BC250_UMD_SOURCE = $dir; BC250_UMD_BUILD = '' }
Assert ($r -match '^BC250_UMD_BUILD is not set: set it to' -and $r -notmatch 'BC250_UMD_SOURCE and') "one unset: $r"
$r = Get-QualityInputRefusal @{ BC250_UMD_SOURCE = (Join-Path $base 'gone'); BC250_UMD_BUILD = $dir }
Assert ($r -eq "BC250_UMD_SOURCE names a directory that does not exist: $(Join-Path $base 'gone')") "missing directory: $r"
$file = Join-Path $base 'a-file.txt'; Set-Content -LiteralPath $file -Value 'x'
$r = Get-QualityInputRefusal @{ BC250_UMD_SOURCE = $dir; BC250_UMD_BUILD = $file }
Assert ($r -match '^BC250_UMD_BUILD names a directory that does not exist') "a file is not a tree: $r"
Assert ($null -eq (Get-QualityInputRefusal @{ BC250_UMD_SOURCE = $dir; BC250_UMD_BUILD = $dir })) 'both set to directories'

# TEMP: moved to the workspace when it is on another drive, kept when it is on the same drive.
$savedTemp, $savedTmp = $env:TEMP, $env:TMP
try {
    $other = if ([IO.Path]::GetPathRoot($base) -ieq 'C:\') { 'D:\tmp' } else { 'C:\Windows\Temp' }
    $env:TEMP = $other
    $s = Set-QualityTemp -Workspace $base
    Assert ($s -eq "TEMP: set to $(Join-Path $base 'scratch\tmp')" -and $env:TEMP -eq (Join-Path $base 'scratch\tmp') -and $env:TMP -eq $env:TEMP) "TEMP on another drive: $s"
    $env:TEMP = $dir
    Assert ((Set-QualityTemp -Workspace $base) -eq "TEMP: kept ($dir)") 'TEMP on the workspace drive is kept'
} finally { $env:TEMP, $env:TMP = $savedTemp, $savedTmp }

# quick.ps1 itself: refused before its first gate, one line, exit 2, result.json REFUSED and nothing else written.
$quick = Join-Path $repo 'tools\quality\quick.ps1'
$q = { param($o) "& '$quick' -Workspace '$(Split-Path $repo)' -RepoRoot '$repo' -Out '$o'; exit `$LASTEXITCODE" }
$o1 = Join-Path $base 'q1'
$c = Invoke-Child (& $q $o1) @{ BC250_UMD_SOURCE = $null; BC250_UMD_BUILD = $null }
Assert ($c.Code -eq 2) "quick.ps1 without both variables exits 2, not $($c.Code): $($c.Lines -join ' | ')"
Assert ($c.Lines.Count -eq 1 -and $c.Lines[0] -match '^quick\.ps1 refused: BC250_UMD_SOURCE and BC250_UMD_BUILD are not set') "one line: $($c.Lines -join ' | ')"
$res = Get-Content -LiteralPath (Join-Path $o1 'result.json') -Raw | ConvertFrom-Json
Assert ($res.status -eq 'REFUSED' -and $res.error -match 'BC250_UMD_SOURCE') 'result.json says REFUSED'
Assert (@(Get-ChildItem -LiteralPath $o1 -Force).Count -eq 1) 'nothing but result.json is written'
$o2 = Join-Path $base 'q2'
$c = Invoke-Child (& $q $o2) @{ BC250_UMD_SOURCE = $dir; BC250_UMD_BUILD = $null }
Assert ($c.Code -eq 2 -and $c.Lines.Count -eq 1 -and $c.Lines[0] -match '^quick\.ps1 refused: BC250_UMD_BUILD is not set') "one variable missing: $($c.Lines -join ' | ')"
$c = Invoke-Child (& $q (Join-Path $base 'q3')) @{ BC250_UMD_SOURCE = $dir; BC250_UMD_BUILD = (Join-Path $base 'gone') }
Assert ($c.Code -eq 2 -and $c.Lines[0] -match '^quick\.ps1 refused: BC250_UMD_BUILD names a directory that does not exist') "a missing tree: $($c.Lines -join ' | ')"

# The import, in a child without cl.exe on PATH and without VSCMD_ARG_TGT_ARCH: imported once, then kept.
$path = (@($env:PATH -split ';' | Where-Object { $_ -and -not (Test-Path -LiteralPath (Join-Path $_ 'cl.exe')) }) -join ';')
$probe = ". '$PSScriptRoot\quality-env.ps1'; if (Get-Command cl.exe -ErrorAction SilentlyContinue) { 'cl.exe before' }; " +
    "Initialize-QualityDevEnvironment -Repo '$repo' -TempDir '$base\vsenv'; Initialize-QualityDevEnvironment -Repo '$repo' -TempDir '$base\vsenv'; " +
    "'arch ' + `$env:VSCMD_ARG_TGT_ARCH"
$c = Invoke-Child $probe @{ PATH = $path; VSCMD_ARG_TGT_ARCH = $null; VCINSTALLDIR = $null; VSINSTALLDIR = $null }
Assert ($c.Code -eq 0 -and $c.Lines -notcontains 'cl.exe before') "the child starts without cl.exe: $($c.Lines -join ' | ')"
Assert (@($c.Lines | Where-Object { $_ -match '^developer environment: imported x64 from .* \(.*cl\.exe\)$' }).Count -eq 1) "imported once: $($c.Lines -join ' | ')"
Assert (@($c.Lines | Where-Object { $_ -match '^developer environment: kept \(.*cl\.exe\)$' }).Count -eq 1) "then kept: $($c.Lines -join ' | ')"
Assert ($c.Lines -contains 'arch x64') 'the x64 target'

# Exercise the actual installed VS setup and production common initializer. No compilation or device access.
$validation = Join-Path $base 'validate-vs.ps1'
@'
param([string]$Repo, [string]$Out)
$ErrorActionPreference = 'Stop'
. (Join-Path $Repo 'tools\build\common.ps1')
$checked = 0
function Check([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message }; $script:checked++ }
function Snapshot { return (@($env:PATH, $env:INCLUDE, $env:LIB, $env:LIBPATH, $env:_CL_) | ConvertTo-Json -Compress) }
$install = Find-VsInstall ''
$temp = Join-Path $Out 'vs'
$custom = (New-Item -ItemType Directory -Force (Join-Path $Out 'portable-llvm')).FullName
$env:PATH = "$custom;$env:PATH"
# Retain a non-VS caller path of at least the size that failed in the MPO build.
for ($i = 0; $env:PATH.Length -lt 5000; $i++) { $env:PATH += ';' + (Join-Path $Out ('caller-tool-' + $i + '-' + ('a' * 55))) }
$env:_CL_ = '/DKEEP_CALLER_FLAG'; $env:BC250_VS_X86_POLICY = $null
$env:VSCMD_ARG_TGT_ARCH = $null
$null = Import-VsDevEnvironment -VsInstall $install -TempDir $temp -Arch x64
Check (Test-VsDevEnvironment $install x64) 'x64 initialization failed'
Check (($env:PATH -split ';') -contains $custom) 'caller portable path lost'
Check ($env:_CL_ -eq '/DKEEP_CALLER_FLAG') 'caller compiler flag lost'
$x64 = Snapshot
1..3 | ForEach-Object {
    $null = Import-VsDevEnvironment -VsInstall $install -TempDir $temp -Arch x64
    Check ((Snapshot) -eq $x64) 'same x64 import grows or changes environment'
}
$null = Import-VsDevEnvironment -VsInstall $install -TempDir $temp -Arch x86
Check (Test-VsDevEnvironment $install x86) 'x64 to x86 did not select x86 tools and libraries'
Check (($env:PATH -split ';') -contains $custom) 'portable path lost on architecture switch'
Check ($env:_CL_ -eq '/DKEEP_CALLER_FLAG /external:W1 /we4229 /we4440') 'x86 policy missing or duplicated'
$x86 = Snapshot
1..3 | ForEach-Object {
    $null = Import-VsDevEnvironment -VsInstall $install -TempDir $temp -Arch x86
    Check ((Snapshot) -eq $x86) 'same x86 import grows or changes environment'
}
$null = Import-VsDevEnvironment -VsInstall $install -TempDir $temp -Arch x64
Check (Test-VsDevEnvironment $install x64) 'x86 to x64 did not restore x64 tools and libraries'
Check ((Snapshot) -eq $x64) 'x64 round trip changed caller paths, includes, libraries or flags'
$env:INCLUDE = ''
Check (-not (Test-VsDevEnvironment $install x64)) 'partial environment was trusted'
$null = Import-VsDevEnvironment -VsInstall $install -TempDir $temp -Arch x64
Check (Test-VsDevEnvironment $install x64) 'missing headers were not repaired'
$fakeCl = Join-Path $custom 'cl.exe'; [IO.File]::WriteAllBytes($fakeCl, [byte[]]@())
$env:PATH = "$custom;$env:PATH"
Check (-not (Test-VsDevEnvironment $install x64)) 'unrelated cl.exe on PATH was trusted'
$null = Import-VsDevEnvironment -VsInstall $install -TempDir $temp -Arch x64
Check (Test-VsDevEnvironment $install x64) 'shadowing compiler was not repaired'
Remove-Item -LiteralPath $fakeCl
$fakeInstall = Join-Path $Out 'other-vs'
$fakeBuild = (New-Item -ItemType Directory -Force (Join-Path $fakeInstall 'VC\Auxiliary\Build')).FullName
Set-Content -LiteralPath (Join-Path $fakeBuild 'vcvars64.bat') -Encoding ascii -Value @('@echo off', 'exit /b 0')
Check (-not (Test-VsDevEnvironment $fakeInstall x64)) 'another installation was trusted'
$before = Snapshot; $refused = $false
try { $null = Import-VsDevEnvironment -VsInstall $fakeInstall -TempDir $temp -Arch x64 } catch { $refused = $true }
Check $refused 'incomplete explicit installation silently fell back'
Check ((Snapshot) -eq $before) 'failed initialization changed the caller environment'
$env:VSINSTALLDIR = $fakeInstall
Check (-not (Test-VsDevEnvironment $install x64)) 'stale VS installation variable was trusted'
$null = Import-VsDevEnvironment -VsInstall $install -TempDir $temp -Arch x64
Check (Test-VsDevEnvironment $install x64) 'stale VS installation variable was not repaired'
. (Join-Path $Repo 'tools\quality\quality-env.ps1')
$kits = Join-Path $Out 'chosen-kits'
$null = Initialize-QualityDevEnvironment -Repo $Repo -TempDir $temp -Kits $kits
$prefix = Join-Path $kits 'microsoft.windows.wdk.x64\c\Include\10.0.26100.0\um'
Check (($env:INCLUDE -split ';')[0] -eq $prefix) 'selected WDK header priority was lost'
$quality = Snapshot
$null = Initialize-QualityDevEnvironment -Repo $Repo -TempDir $temp -Kits $kits
Check ((Snapshot) -eq $quality) 'quality initializer duplicates WDK headers'
"VS common: $checked checks; PATH=$($env:PATH.Length) INCLUDE=$($env:INCLUDE.Length) LIB=$($env:LIB.Length)"
'@ | Set-Content -LiteralPath $validation -Encoding utf8
$c = Invoke-Child "& '$validation' -Repo '$repo' -Out '$base'" @{}
Assert ($c.Code -eq 0) "actual VS repeat and architecture checks: $($c.Lines -join ' | ')"
Assert (@($c.Lines | Where-Object { $_ -match '^VS common: \d+ checks;' }).Count -eq 1) 'common environment checks completed'
$wrapper = [IO.File]::ReadAllText((Join-Path $repo 'tools\quality\quick.cmd'))
Assert ($wrapper -notmatch 'vcvars|vswhere|set "INCLUDE=') 'quick.cmd does not initialize VS or expand INCLUDE'

# The wiring: quick.ps1 refuses before it reads anything else, and imports before it resolves cl.exe.
$text = [IO.File]::ReadAllText($quick)
$at = { param($s) $text.IndexOf($s) }
$refuse = & $at 'if($refusal){'
Assert ($refuse -gt 0 -and $refuse -lt (& $at 'Resolve-Bc250PackageContext -Repo')) 'quick.ps1 refuses before the package context'
$import = & $at 'Initialize-QualityDevEnvironment -Repo $repo'
Assert ($import -gt 0 -and $import -lt (& $at '$clExe=if(')) 'quick.ps1 imports before it resolves cl.exe'

Remove-Item -LiteralPath $base -Recurse -Force
"PASS: quality environment, $passed checks"
