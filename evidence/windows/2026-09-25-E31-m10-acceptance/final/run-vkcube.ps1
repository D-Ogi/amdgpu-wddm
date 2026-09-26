param([ValidateRange(1,36000)][int]$Frames=600)
$ErrorActionPreference='Stop'
$base='C:\BC250\m10\wsi-final'
if((Get-FileHash "$base\vulkan_radeon.dll").Hash -ne '9C40083C08210C71A6F6DC7EA12F7C1205BBC6AA1DFE48E596C8495B6C2A9798'){throw 'Unexpected M10 ICD'}
if((Invoke-RestMethod http://127.0.0.1:2250/flags).stop){throw 'Owner STOP is active'}
$env:VK_DRIVER_FILES="$base\radeon_icd.json"
$env:VK_ICD_FILENAMES=$env:VK_DRIVER_FILES
$env:MESA_VK_WSI_DEBUG=''
$env:BC250_TRACE_SUBMITS='0'
& "$base\vkcube.exe" --c $Frames --width 640 --height 480 --suppress_popups
exit $LASTEXITCODE
