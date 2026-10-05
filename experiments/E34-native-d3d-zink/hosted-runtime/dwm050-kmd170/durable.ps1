# Durable artifacts must exist before any experimental gate or DLL mutation.
function Copy-VerifiedDurable {
 param([string]$Source,[string]$Destination,[string]$Expected)
 if((Get-FileHash -LiteralPath $Source).Hash -ne $Expected){throw 'Durable source hash mismatch'}
 if(Test-Path -LiteralPath $Destination){throw 'Durable destination exists; inspect original attempt'}
 $inputFile=[IO.File]::Open($Source,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
 try {
  $outputFile=New-Object IO.FileStream($Destination,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read,1050576,[IO.FileOptions]::WriteThrough)
  try {$inputFile.CopyTo($outputFile);$outputFile.Flush($true)} finally {$outputFile.Dispose()}
 } finally {$inputFile.Dispose()}
 if((Get-FileHash -LiteralPath $Destination).Hash -ne $Expected){throw 'Durable destination hash mismatch'}
}
function Write-DurableText {
 param([string]$Path,[string]$Text)
 $bytes=(New-Object Text.UTF8Encoding($false)).GetBytes($Text)
 $temporary=$Path+'.pending-'+[guid]::NewGuid().ToString('N')
 try {
  $file=New-Object IO.FileStream($temporary,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read,4096,[IO.FileOptions]::WriteThrough)
  try {$file.Write($bytes,0,$bytes.Length);$file.Flush($true)} finally {$file.Dispose()}
  # Same-directory rename publishes a complete receipt, never a partial JSON file.
  # Move refuses to replace an existing receipt.
  [IO.File]::Move($temporary,$Path)
 } finally {if(Test-Path -LiteralPath $temporary){Remove-Item -LiteralPath $temporary -Force}}
}
function Flush-ExistingFile {
 param([string]$Path)
 $file=[IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::ReadWrite)
 try {$file.Flush($true)} finally {$file.Dispose()}
}
function Set-DurablePresentGates {
 param([ValidateSet(0,1)][int]$Value)
 if(-not ('BC250TrialRegistryFlush' -as [type])) {
  Add-Type -TypeDefinition 'using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class BC250TrialRegistryFlush {
 [DllImport("advapi32.dll", ExactSpelling=true)]
 public static extern int RegFlushKey(SafeRegistryHandle key);
}'
 }
 $key=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters',$true)
 if(!$key){throw 'Driver registry key absent'}
 try {
  $key.SetValue('EnableGpuPresentBlit',$Value,[Microsoft.Win32.RegistryValueKind]::DWord)
  $key.SetValue('EnableCddDwmInterop',$Value,[Microsoft.Win32.RegistryValueKind]::DWord)
  $errorCode=[BC250TrialRegistryFlush]::RegFlushKey($key.Handle)
  if($errorCode -ne 0){throw "RegFlushKey failed: $errorCode"}
  if($key.GetValue('EnableGpuPresentBlit') -ne $Value -or $key.GetValue('EnableCddDwmInterop') -ne $Value){throw 'Gate readback mismatch'}
 } finally {$key.Dispose()}
}

function Restore-DurableBaseline {
 param([string]$Path,[string]$Backup,[string]$Original,[string]$Baseline,[string]$Candidate)
 $current=if(Test-Path -LiteralPath $Path){(Get-FileHash -LiteralPath $Path).Hash}else{''}
 if($current -eq $Baseline){return}
 if($current -and $current -ne $Candidate){throw 'Unexpected active file; preserved'}
 $verified=$null
 foreach($file in @($Backup,$Original)){
  if((Test-Path -LiteralPath $file) -and (Get-FileHash -LiteralPath $file).Hash -eq $Baseline){$verified=$file;break}
 }
 if(!$verified){throw 'No verified baseline/original backup'}
 if($current){Move-Item -LiteralPath $Path -Destination ($Path+'.held-'+[guid]::NewGuid().ToString('N'))}
 Copy-VerifiedDurable $verified $Path $Baseline
}
