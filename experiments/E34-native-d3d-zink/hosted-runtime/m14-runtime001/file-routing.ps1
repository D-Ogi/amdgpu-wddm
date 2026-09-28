# Call only after the previous bounded phase's job is confirmed empty.
function Install-M14FileRoute {
 param([string]$Active,[string]$Backup,[string]$Original,[string]$Source,[string]$Baseline,[string]$Candidate)
 if((Get-FileHash -LiteralPath $Active).Hash -ne $Baseline -or (Get-FileHash -LiteralPath $Backup).Hash -ne $Baseline){throw 'Baseline file mismatch'}
 if(Test-Path -LiteralPath $Original){throw 'Consumed original path'}
 $prepared=$Original+'.candidate'
 Copy-VerifiedDurable $Source $prepared $Candidate
 # Never expose a partially copied DLL under the active path.
 Move-Item -LiteralPath $Active -Destination $Original
 Move-Item -LiteralPath $prepared -Destination $Active
 if((Get-FileHash -LiteralPath $Active).Hash -ne $Candidate){throw 'Installed router mismatch'}
}
function Restore-M14FileRoute {
 param([string]$Active,[string]$Backup,[string]$Baseline,[string]$Candidate)
 $current=if(Test-Path -LiteralPath $Active){(Get-FileHash -LiteralPath $Active).Hash}else{''}
 if($current -eq $Baseline){return}
 if($current -and $current -ne $Candidate){throw 'Foreign active file; preserved'}
 $prepared=$Backup+'.restore-'+[guid]::NewGuid().ToString('N')
 Copy-VerifiedDurable $Backup $prepared $Baseline
 if($current){Move-Item -LiteralPath $Active -Destination ($Active+'.m14-held-'+[guid]::NewGuid().ToString('N'))}
 Move-Item -LiteralPath $prepared -Destination $Active
 if((Get-FileHash -LiteralPath $Active).Hash -ne $Baseline){throw 'Restored baseline mismatch'}
}
