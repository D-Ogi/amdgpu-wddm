param([string]$Root='P:\bc-250',[string]$Out='P:\bc-250\scratch\build\health145',[switch]$IgnoreFlush,
      [switch]$NoWitnessReopen,[switch]$ReopenAnyClose,[switch]$StickyFaultKeepsMark,[switch]$ConfirmCertifiesNewEpoch,
      [switch]$MarkOverFailedEngines,[switch]$RefusalDropsOtherNodeMark,[switch]$RearmSkipsOpenWitness,
      [switch]$ConfirmHungRequest,[switch]$QuietIgnoresOwedFlip,[switch]$NoQuiet)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$env:TEMP=Join-Path $Root 'scratch\tmp';$env:TMP=$env:TEMP
New-Item -ItemType Directory -Force $Out | Out-Null
$source=Get-Content (Join-Path $repo 'driver\kmd\start_health.c') -Raw
# BD-120 negative controls. Each one alters only copied production code, must compile, and must then fail a
# CHECK; a changed anchor stops the run instead of passing a mutation that no longer applies.
function Mutate([string]$Text,[string]$From,[string]$To) {
    if(!$Text.Contains($From)){throw "start-health mutation anchor changed: $From"}
    return $Text.Replace($From,$To)
}
# The behaviour before the fix: the witness neither reopens nor re-arms, so flags stay at FULL for the rest of
# the start once the latch has closed it.
if($NoWitnessReopen){$source=Mutate $source 'if (!h->Closed) {' 'if (0) {'
                     $source=Mutate $source '} else if (h->PendingReset) {' '} else if (0) {'}
# A reset that reopens any close, including one a real fault owns.
if($ReopenAnyClose){$source=Mutate $source '} else if (h->PendingReset) {' '} else if (1) {'}
# A sticky fault that leaves the pending-reset mark standing, so the reset undoes a fault of another kind.
if($StickyFaultKeepsMark){$source=Mutate $source 'if (Mark==0) H->PendingReset=FALSE;' 'if (Mark==99) H->PendingReset=FALSE;'}
# A mark taken over a witness whose engines never came up, so the reset clears a startup engine failure.
if($MarkOverFailedEngines){$source=Mutate $source 'if (Mark>0 && H->Engines) H->PendingReset=TRUE;' 'if (Mark>0) H->PendingReset=TRUE;'}
# A refusal on another node that clears node 0's mark anyway, which is the defect in one ordering.
if($RefusalDropsOtherNodeMark){$source=Mutate $source 'if (Mark==0) H->PendingReset=FALSE;' 'if (Mark<=0) H->PendingReset=FALSE;'}
# A recovery that leaves an open witness alone, so a measured interval can span the hang.
if($RearmSkipsOpenWitness){$source=Mutate $source 'if (!h->Closed) {' 'if (0) {'}
# A start that hung node 0 confirming its own clock/voltage and CU request as the next boot's default.
if($ConfirmHungRequest){$source=Mutate $source 'ready=HealthConfirmable(h,&data) && !h->Recovered;' 'ready=HealthConfirmable(h,&data);'
                        $source=Mutate $source 'if (NT_SUCCESS(status) && !recovered) (void)CuModeConfirm(Device,"start-health");' 'if (NT_SUCCESS(status)) (void)CuModeConfirm(Device,"start-health");'
                        $source=Mutate $source 'if (NT_SUCCESS(status) && !recovered) (void)DpmConfirm(Device,"start-health");' 'if (NT_SUCCESS(status)) (void)DpmConfirm(Device,"start-health");'}
# QUIET (0.7.216.104): a flip programmed and never completed taken for nothing owed, so a hung presentation path
# confirms on a stale completion.
if($QuietIgnoresOwedFlip){$source=Mutate $source '&& programmed==H->LastSequence;' ';'}
# The rule before QUIET: freshness alone, which an idle GPU desktop (one flip a minute) does not meet.
if($NoQuiet){$source=Mutate $source '(Data->LastCompletionAgeMs<=BC250_START_HEALTH_FRESH_MS || (Data->Flags&BC250_START_HEALTH_QUIET));' '(Data->LastCompletionAgeMs<=BC250_START_HEALTH_FRESH_MS);'}
# A confirmation that certifies whatever epoch it finds after its registry flush, the new one included.
if($ConfirmCertifiesNewEpoch){$source=Mutate $source 'if (h->Generation!=generation || h->Epoch!=epoch) status=STATUS_RETRY;' 'if (h->Generation!=generation) status=STATUS_RETRY;'}
[IO.File]::WriteAllText((Join-Path $Out 'start_health_actual.inc'),$source.Replace('#include "bc250kmd.h"',''))
$state=Get-Content (Join-Path $repo 'driver\kmd\start_health.h') -Raw
[IO.File]::WriteAllText((Join-Path $Out 'start_health_state.inc'),$state.Replace('#include "bc250kmd_escape.h"',''))
$guard=Get-Content (Join-Path $repo 'driver\kmd\guard.c') -Raw
$code=$guard.Substring($guard.IndexOf('NTSTATUS GuardConfirmStartDurable('))
# Up to the CU mode's own settings helpers (cumode.c), which this test does not model.
$end=$code.IndexOf('// ---- settings the driver itself owns');if($end -gt 0){$code=$code.Substring(0,$end)}
if($IgnoreFlush){$code=$code.Replace('status=ZwFlushKey(key);','(void)ZwFlushKey(key);')}
[IO.File]::WriteAllText((Join-Path $Out 'confirm_actual.inc'),$code)
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'start_health_test.c') -Destination (Join-Path $Out 'test.c') -Force
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp\c';$libs=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp.x64\c'
& "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /TC /W4 /WX /wd4201 /wd4127 /O2 /MT "/I$repo\driver\kmd" "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/Fo$Out\" "/Fe$Out\health_test.exe" "$Out\test.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if($LASTEXITCODE -ne 0){throw 'health host build failed'}
& "$Out\health_test.exe"
exit $LASTEXITCODE
