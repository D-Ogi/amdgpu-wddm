# Host test for driver\kmd\dcn_cursor.c: the life of the hardware pointer (prepare, caps, shapes before a pipe, in the
# gap of a mode change and on a ready pipe, the two surfaces and the latch wait, the cursor TTU of the inherited pipe,
# a failed register write, the stop, the quiet off and the release; docs/design/hardware-pointer.md). The production
# body is compiled unchanged, without its bc250kmd.h include, against the host model of dcn_cursor_life_test.c.
#
#   pwsh driver\kmd\test\run_dcn_cursor_life.ps1
#   pwsh driver\kmd\test\run_dcn_cursor_life.ps1 -Mutation all          (every negative control, each must fail)
#   pwsh driver\kmd\test\run_dcn_cursor_life.ps1 -Mutation spare-same-surface
#
# -Mutation changes one rule in a COPY of dcn_cursor.c, never the tree, and the test must then fail with a FAIL line.
# A mutation whose text does not occur exactly once in dcn_cursor.c stops the run instead of passing quietly.
# -Mutation all runs every control and fails if any of them passes. The binaries go under -Out; nothing is written to
# drive C:.

param(
    [string]$Root = $(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),
    [string]$Out = "$Root\scratch\m9\dcn-cursor-life",
    [string]$Kits = "$Root\toolchain\nuget",
    [string]$KitVersion = '10.0.26100.0',
    [ValidateSet('', 'all', 'no-unlock-on-failure', 'spare-same-surface', 'no-latch-wait', 'stop-not-sticky',
                 'caps-ignore-link', 'ttu-unchecked', 'shape-stored-not-shown', 'gone-does-nothing',
                 'shape-refused-in-gap', 'stop-skips-off-write')][string]$Mutation = ''
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$kmd = Split-Path -Parent $here
$repo = Split-Path -Parent (Split-Path -Parent $kmd)
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdklib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'

# Each control: the exact text of one rule in dcn_cursor.c, what replaces it, and the rule it breaks.
$mutations = [ordered]@{
    'no-unlock-on-failure' = @{ find = 'unlock = CursorLock(Device, FALSE);'
        with = 'unlock = NT_SUCCESS(status) ? CursorLock(Device, FALSE) : STATUS_SUCCESS;'
        why = 'a failed write in a batch leaves the cursor update lock set' }
    'spare-same-surface' = @{ find = 'spare = c->Current ^ 1u;'; with = 'spare = c->Current;'
        why = 'a new shape is drawn into the surface that the registers name' }
    'no-latch-wait' = @{ find = 'KeQueryInterruptTime() - c->SwitchTime >= DCN_CURSOR_LATCH_MS * 10000ull'
        with = 'KeQueryInterruptTime() - c->SwitchTime >= 0ull'
        why = 'the spare surface is drawn into before the hardware latched the change that left it' }
    'stop-not-sticky' = @{ find = 'if (c->Ready && !c->Stopped && c->ImageValid) status = CursorBatch(Device, c, FALSE, c->Current);'
        with = 'if (c->ImageValid) status = CursorBatch(Device, c, FALSE, c->Current);'
        why = 'a position programs the cursor between the pipes and after the stop' }
    'caps-ignore-link' = @{ find = 'return c->EverReady && !c->Stopped && ConnectorIsEnabled(Device);'
        with = 'return c->EverReady && !c->Stopped && Device != NULL;'
        why = 'the caps report a hardware pointer after the connector link was released' }
    'ttu-unchecked' = @{ find = 'if (Pipe->Delivery > HUBPREQ0_DCN_CUR0_TTU_CNTL0__REFCYC_PER_REQ_DELIVERY_MASK ||'
        with = 'if (c->Stopped ||'
        why = 'a cursor TTU value that does not fit its field is written and the pipe is taken as ready' }
    'shape-stored-not-shown' = @{ find = 'if (ready && c->ShapeValid) status = CursorShow(Device, c, NULL, NULL);'
        with = 'if (ready && c->ShapeValid && c->Stopped) status = CursorShow(Device, c, NULL, NULL);'
        why = 'the new pipe does not show the stored shape' }
    'gone-does-nothing' = @{ find = 'if (c->Ready) InterlockedIncrement(&c->PipeGones);'
        with = 'if (Device->Mmio) { KeReleaseSpinLock(&c->Lock, irql); return; }'
        why = 'the cursor stays on while the pipe changes' }
    'shape-refused-in-gap' = @{ find = 'if (ready || expected) {'; with = 'if (ready) {'
        why = 'a shape that arrives during a mode change is refused and the software pointer stays' }
    'stop-skips-off-write' = @{ find = 'c->Enabled = TRUE;      // write the off batch'
        with = 'c->Enabled |= FALSE;      // write the off batch'
        why = 'the stop trusts the last batch and leaves an enable that the next owner finds' }
}

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
$env:TEMP = "$Root\scratch\tmp"; $env:TMP = $env:TEMP
$env:INCLUDE = ''; $env:LIB = ''

# Builds and runs the test with $Body as dcn_cursor.c; returns the exit code and the output.
function Invoke-DcnCursorLife([string]$Dir, [string]$Body) {
    $obj = Join-Path $Dir 'obj'
    New-Item -ItemType Directory -Force $Dir, $obj | Out-Null
    Remove-Item "$obj\*.obj" -Force -ErrorAction SilentlyContinue
    if ([regex]::Matches($Body, '#include "bc250kmd\.h"').Count -ne 1) { throw 'Expected exactly one production-header include' }
    [IO.File]::WriteAllText((Join-Path $Dir 'dcn_cursor_actual.inc'), $Body.Replace('#include "bc250kmd.h"', ''))
    & $cl @('/nologo', '/TC', '/W4', '/WX', '/O2', '/MT', "/I$Dir", "/I$kmd", "/I$repo\third_party\linux-amdgpu",
        "/I$($msvc.FullName)\include", "/I$sdk\Include\$KitVersion\ucrt", "/Fo$obj\", "/Fe$Dir\dcn_cursor_life_test.exe",
        (Join-Path $here 'dcn_cursor_life_test.c'),
        '/link', "/LIBPATH:$($msvc.FullName)\lib\x64", "/LIBPATH:$sdklib\ucrt\x64", "/LIBPATH:$sdklib\um\x64") | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Host build failed ($Dir)" }
    $output = & "$Dir\dcn_cursor_life_test.exe"
    return @{ Code = $LASTEXITCODE; Output = ($output -join "`n") }
}

$source = Get-Content -Raw (Join-Path $kmd 'dcn_cursor.c')
if (-not $Mutation) {
    $run = Invoke-DcnCursorLife $Out $source
    Write-Output $run.Output
    exit $run.Code
}

$names = if ($Mutation -eq 'all') { @($mutations.Keys) } else { @($Mutation) }
$passed = @()
foreach ($name in $names) {
    $m = $mutations[$name]
    $count = ([regex]::Matches($source, [regex]::Escape($m.find))).Count
    if ($count -ne 1) { throw "mutation ${name}: its text occurs $count times in dcn_cursor.c, not once" }
    $run = Invoke-DcnCursorLife (Join-Path $Out "mutation-$name") $source.Replace($m.find, $m.with)
    $failed = $run.Code -ne 0 -and $run.Output -match 'FAIL line'
    Write-Output ("mutation {0}: {1} ({2})" -f $name, $(if ($failed) { 'caught' } else { 'NOT CAUGHT' }), $m.why)
    if (-not $failed) { $passed += $name }
}
if ($passed.Count) { Write-Output "mutations not caught: $($passed -join ', ')"; exit 1 }
Write-Output "dcn cursor life mutations: $($names.Count) caught"
exit 0
