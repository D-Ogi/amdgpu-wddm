$ErrorActionPreference='Stop'
$root='C:\BC250\m13\runtime-probe001'
$hosted='C:\BC250\m13\hosted-runtime006'
$umd='C:\BC250\m11\resource-close\bc250d3d.dll'
$backup='C:\BC250\m11\resource-close\bc250d3d.m13-original.dll'
if(Test-Path $backup){
 if((Get-FileHash $backup).Hash -ne '8279AC7F6342CD0A31CB96531194CEF4A224A0D010BCEE4B549D9606D5E405EA'){throw 'CPU backup mismatch'}
 Move-Item -LiteralPath $umd -Destination "$hosted\router-held.dll"
 Move-Item -LiteralPath $backup -Destination $umd
}
if((Get-FileHash $umd).Hash -ne '8279AC7F6342CD0A31CB96531194CEF4A224A0D010BCEE4B549D9606D5E405EA'){throw 'CPU restore mismatch'}
'CPU UMD verified restored'
$active='C:\BC250\m10\wsi-final\vulkan_radeon.dll'
if((Get-FileHash $active).Hash -ne '9C40083C08210C71A6F6DC7EA12F7C1205BBC6AA1DFE48E596C8495B6C2A9798'){
 Move-Item -LiteralPath $active -Destination "$hosted\standalone-held.dll"
 Copy-Item -LiteralPath "$root\baseline.dll" -Destination $active
}
if((Get-FileHash $active).Hash -ne '9C40083C08210C71A6F6DC7EA12F7C1205BBC6AA1DFE48E596C8495B6C2A9798'){throw 'ICD restore mismatch'}
'ICD verified restored'
Move-Item -LiteralPath "$root\bc250d3d_zink.dll" -Destination "$hosted\umd-held.dll"
Copy-Item -LiteralPath "$hosted\previous-umd.dll" -Destination "$root\bc250d3d_zink.dll"
'Control UMD restored'
Get-Process dwm | Select-Object Id
