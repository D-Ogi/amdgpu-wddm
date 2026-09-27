param([Parameter(Mandatory)][string]$OutDir)
$ErrorActionPreference='Stop'
New-Item -ItemType Directory -Path $OutDir -ErrorAction Stop | Out-Null
$wrapper=Join-Path $PSScriptRoot 'invoke-recorded-worker.ps1'
$hostExe="$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe"
$cases=@(@{name='preflight-throw';text="throw 'injected before child launch'";code=1},@{name='worker-exit';text="'worker output';exit 7";code=7},@{name='success';text="'completed';exit 0";code=0})
foreach($case in $cases) {
 $worker=Join-Path $OutDir ($case.name+'.ps1')
 [IO.File]::WriteAllText($worker,$case.text)
 & $hostExe -NoProfile -ExecutionPolicy Bypass -File $wrapper -Worker $worker -ReceiptDirectory $OutDir -Attempt $case.name
 if($LASTEXITCODE -ne $case.code){throw 'Wrapper exit mismatch'}
 $receipt=Get-Content (Join-Path $OutDir ($case.name+'-terminal.json')) -Raw | ConvertFrom-Json
 if($receipt.exit_code -ne $case.code){throw 'Receipt exit mismatch'}
 if($case.name -eq 'preflight-throw' -and $receipt.failure.message -ne 'injected before child launch'){throw 'Preflight exception missing'}
 if(!(Test-Path (Join-Path $OutDir ($case.name+'-started.json')))){throw 'Missing startup receipt'}
}
# A repeated invocation must preserve both original receipts and not run the worker.
$old=(Get-FileHash (Join-Path $OutDir 'success-terminal.json')).Hash
$ErrorActionPreference='Continue'
& $hostExe -NoProfile -ExecutionPolicy Bypass -File $wrapper -Worker (Join-Path $OutDir 'success.ps1') -ReceiptDirectory $OutDir -Attempt success 2>&1 | Out-File (Join-Path $OutDir 'duplicate-output.txt')
$duplicateExit=$LASTEXITCODE
$ErrorActionPreference='Stop'
if($duplicateExit -eq 0 -or (Get-FileHash (Join-Path $OutDir 'success-terminal.json')).Hash -ne $old){throw 'Duplicate overwrote result'}
'PASS preflight exception, nonzero worker exit, success, and duplicate attempt protection'
