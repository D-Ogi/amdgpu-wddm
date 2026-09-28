param([Parameter(Mandatory)][string]$Exe)
$ErrorActionPreference='Stop'
$cases=@(
 @('--install-deferred'),
 @('--unknown','x','x'),
 @('--install-deferred','PCI\VEN_1002&DEV_13FE&TEST','unused','0.7.169'),
 @('--install-deferred','PCI\VEN_1002&DEV_13FE&TEST','unused','0.7.169.1junk'),
 @('--install-deferred','PCI\VEN_1002&DEV_13FE&TEST','unused','0.7.65536.1'),
 @('--install-deferred','PCI\VEN_1002&DEV_13FE&TEST','unused','-1.7.169.1'),
 @('--install-deferred','PCI\VEN_1002&DEV_13FE0','unused','0.7.169.1'),
 @('--install-deferred','PCI\VEN_1234&DEV_13FE','unused','0.7.169.1')
)
foreach($arguments in $cases){
 & $Exe @arguments|Out-Null
 if($LASTEXITCODE -ne 2){throw 'Invalid admission reached beyond argument validation'}
}
'PASS: eight invalid install requests rejected before opening any device'
