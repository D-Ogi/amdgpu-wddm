$ErrorActionPreference='Stop'
$key='HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\state-transitions-control.exe'
if(Test-Path $key){throw 'Existing per-application dump configuration; not changed'}
$created=$false
try {
 New-Item -Path $key -Force | Out-Null
 $created=$true
 New-Item -ItemType Directory -Path 'C:\BC250\m13\hosted-runtime041\dumps097' -Force | Out-Null
 New-ItemProperty -Path $key -Name DumpFolder -Value 'C:\BC250\m13\hosted-runtime041\dumps097' -PropertyType ExpandString | Out-Null
 New-ItemProperty -Path $key -Name DumpType -Value 2 -PropertyType DWord | Out-Null
 New-ItemProperty -Path $key -Name DumpCount -Value 2 -PropertyType DWord | Out-Null
 & powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\BC250\m13\hosted-runtime041\run097-body.ps1
 $result=$LASTEXITCODE
 Get-ChildItem -LiteralPath 'C:\BC250\m13\hosted-runtime041\dumps097' -File | Select-Object Name,Length | ConvertTo-Json
} finally {
 if($created){Remove-Item -LiteralPath $key -Force}
}
exit $result
