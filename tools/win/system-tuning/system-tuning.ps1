#requires -Version 5.1
[CmdletBinding()]
param(
    [string]$Action='List',
    [string]$Item='',
    [int]$PauseDays=7,
    [string]$Scope='Machine',
    [switch]$DryRun,
    [string]$LegacyStatePath=''
)
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)
$OutputEncoding=[Text.UTF8Encoding]::new($false)
$result=$null
try {
    if(-not [Environment]::Is64BitProcess){throw 'Use the 64-bit Windows PowerShell host.'}
    Import-Module (Join-Path $PSScriptRoot 'SystemTuning.Core.psm1') -Force
    Import-Module (Join-Path $PSScriptRoot 'SystemTuning.Native.psm1') -Force
    $legacy=$null
    if($Action -eq 'ImportLegacy'){
        if(-not $LegacyStatePath){throw 'ImportLegacy requires LegacyStatePath.'}
        $file=Get-Item -LiteralPath $LegacyStatePath -Force
        if($file.PSIsContainer -or ($file.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or $file.Length -gt 1MB){throw 'Legacy file must be a regular JSON file of at most 1 MiB.'}
        $legacy=[IO.File]::ReadAllText($file.FullName) | ConvertFrom-Json
    } elseif($LegacyStatePath){throw 'LegacyStatePath is valid only for ImportLegacy.'}
    $result=Invoke-SystemTuning -Ops (New-NativeTuningOperations -Scope $Scope) -Action $Action -Item $Item -PauseDays $PauseDays -DryRun:$DryRun -Legacy $legacy
} catch {
    $result=[pscustomobject]@{schema=1;ok=$false;error=$_.Exception.Message;action=$Action;scope=$Scope;dryRun=[bool]$DryRun;items=@();results=@()}
}
$result | ConvertTo-Json -Depth 18 -Compress
if($result.ok){exit 0}else{exit 1}
