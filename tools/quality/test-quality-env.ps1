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
function Assert([bool]$Ok, [string]$What) { if (-not $Ok) { throw "FAIL: $What" }; $script:passed++ }

# A child pwsh with some variables removed or changed. Returns the exit code and the output lines.
function Invoke-Child([string]$Command, [hashtable]$Change) {
    $psi = [Diagnostics.ProcessStartInfo]::new('pwsh')
    foreach ($a in @('-NoProfile', '-NonInteractive', '-Command', $Command)) { $psi.ArgumentList.Add($a) }
    foreach ($k in $Change.Keys) { if ($null -eq $Change[$k]) { [void]$psi.Environment.Remove($k) } else { $psi.Environment[$k] = $Change[$k] } }
    $psi.UseShellExecute = $false; $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
    $p = [Diagnostics.Process]::Start($psi)
    $err = $p.StandardError.ReadToEndAsync()
    $text = $p.StandardOutput.ReadToEnd()
    if (-not $p.WaitForExit(240000)) { $p.Kill(); throw 'FAIL: the child did not end in 240 s' }
    return [pscustomobject]@{ Code = $p.ExitCode; Lines = @(($text + $err.Result) -split "`r?`n" | Where-Object { $_ -match '\S' }) }
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

# When the developer environment counts as present.
Assert (Test-QualityDevEnvironment 'C:\VS\cl.exe' 'x64') 'cl.exe and x64 is a developer environment'
Assert (-not (Test-QualityDevEnvironment '' 'x64')) 'no cl.exe is none'
Assert (-not (Test-QualityDevEnvironment 'C:\VS\cl.exe' 'x86')) 'an x86 developer shell is not the x64 one'
Assert (-not (Test-QualityDevEnvironment 'C:\VS\cl.exe' '')) 'cl.exe without VSCMD_ARG_TGT_ARCH is not one'

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

# The wiring: quick.ps1 refuses before it reads anything else, and imports before it resolves cl.exe.
$text = [IO.File]::ReadAllText($quick)
$at = { param($s) $text.IndexOf($s) }
$refuse = & $at 'if($refusal){'
Assert ($refuse -gt 0 -and $refuse -lt (& $at 'Resolve-Bc250PackageContext -Repo')) 'quick.ps1 refuses before the package context'
$import = & $at 'Initialize-QualityDevEnvironment -Repo $repo'
Assert ($import -gt 0 -and $import -lt (& $at '$clExe=if(')) 'quick.ps1 imports before it resolves cl.exe'

Remove-Item -LiteralPath $base -Recurse -Force
"PASS: quality environment, $passed checks"
