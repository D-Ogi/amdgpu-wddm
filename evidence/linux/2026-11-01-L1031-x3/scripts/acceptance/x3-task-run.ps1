# Run the installed C:\BC250\x3\x3-task.ps1 in place (its $PSScriptRoot must be C:\BC250\x3), then read back.
$ErrorActionPreference = 'Stop'
$dir = 'C:\BC250\x3'
Get-ChildItem $dir -File | ForEach-Object { '{0} {1}' -f $_.Name, (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.Substring(0,16) }
& (Join-Path $dir 'x3-task.ps1') -LogonUser '<lab host>\bc250'
$t = Get-ScheduledTask -TaskName 'Lab boot counter reset'
'execute={0}' -f $t.Actions[0].Execute
'args={0}' -f $t.Actions[0].Arguments
'principal={0} logon={1} run={2}' -f $t.Principal.UserId, $t.Principal.LogonType, $t.Principal.RunLevel
'trigger={0} delay={1} state={2}' -f $t.Triggers[0].CimClass.CimClassName, $t.Triggers[0].Delay, $t.State
