param([Parameter(Mandatory)][string]$Out)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\registration.ps1"
. "$PSScriptRoot\verify-cpu.ps1"
if(Test-Path $Out){throw 'Use a fresh output directory'}
[void](New-Item -ItemType Directory $Out)
function Must-Reject([scriptblock]$Action){$failed=$false;try{& $Action|Out-Null}catch{$failed=$true};if(!$failed){throw 'False acceptance'}}
function New-FakeKey([hashtable]$Values){
 $store=[ordered]@{};foreach($k in $Values.Keys){$store[$k]=$Values[$k]}
 $key=[pscustomobject]@{Store=$store}
 $key|Add-Member ScriptMethod GetValueNames {@($this.Store.Keys)}
 $key|Add-Member ScriptMethod GetValueKind {param($n) [Microsoft.Win32.RegistryValueKind]$this.Store[$n].kind}
 $key|Add-Member ScriptMethod GetValue {param($n,$d,$o) ,$this.Store[$n].value}
 $key|Add-Member ScriptMethod SetValue {param($n,$v,$k) $this.Store[$n]=@{kind=[string]$k;value=$v}}
 $key|Add-Member ScriptMethod DeleteValue {param($n,$t) $this.Store.Remove($n)}
 $key|Add-Member ScriptMethod Flush {}
 return $key
}
function RoundTrip($Value){$Value|ConvertTo-Json -Depth 8|ConvertFrom-Json}
# The deployed shape: a four-entry UserModeDriverName (D3D12 slot last) and a one-entry VulkanDriverName.
$umd=[string[]]@('bc250umd.dll','C:\BC250\m11\resource-close\bc250d3d.dll','C:\BC250\m11\resource-close\bc250d3d.dll','C:\BC250\m15\registration002\amdgpu_wddm_d3d12.dll')
$icd=[string[]]@('C:\BC250\m10\wsi-final\radeon_icd.json')
$key=New-FakeKey @{UserModeDriverName=@{kind='MultiString';value=$umd};VulkanDriverName=@{kind='MultiString';value=$icd};DriverDesc=@{kind='String';value='BC-250 GPU'};OpenGLFlags=@{kind='DWord';value=3}}
$capture=Read-KmdGraphicsRegistration $key
if(@($capture.Keys).Count -ne 3 -or $capture.Contains('DriverDesc')){throw 'Wrong value family captured'}
$saved=RoundTrip @{graphics_registration=$capture;umd_registration=$umd;icd_registration=$icd}
if(@($saved.graphics_registration.UserModeDriverName.value).Count -ne 4 -or @($saved.graphics_registration.VulkanDriverName.value).Count -ne 1){throw 'JSON round trip changed list length'}
[void](Assert-KmdGraphicsRegistrationBaseline $saved.graphics_registration)

# An install rewrites the list to three stub entries and adds a Wow value; Configure restores the capture.
$key.Store['UserModeDriverName']=@{kind='MultiString';value=[string[]]@('bc250umd.dll','bc250umd.dll','bc250umd.dll')}
$key.Store['VulkanDriverNameWow']=@{kind='MultiString';value=[string[]]@('x.json')}
$key.Store['DriverDesc']=@{kind='String';value='changed by install'}
$r=Set-KmdGraphicsRegistration -Key $key -Saved $saved.graphics_registration
if((@($r.removed) -join ',') -ne 'VulkanDriverNameWow'){throw 'Install-added value not removed'}
$after=Read-KmdGraphicsRegistration $key
Assert-KmdGraphicsRegistration $saved.graphics_registration $after
if(@($after.UserModeDriverName.value).Count -ne 4 -or $after.UserModeDriverName.value[3] -cne $umd[3]){throw 'Fourth entry lost'}
if($key.Store['DriverDesc'].value -ne 'changed by install'){throw 'Non-graphics value touched'}
# The legacy CPU witness accepts the restored lists entry by entry.
Assert-KmdCpuBaseline @{umd_registration=$saved.umd_registration;icd_registration=$saved.icd_registration;umd_sha256='CPU'} @{umd_registration=@($after.UserModeDriverName.value);icd_registration=@($after.VulkanDriverName.value);parameters=@{EnableGpuPresentBlit=0;EnableCddDwmInterop=0;UnconfirmedStarts=0};dwm=@(@{pid=1;start='s';modules=@(@{name='bc250d3d.dll';sha256='CPU'})})}

# Entry-by-entry comparison: order, content, case of contents, count and kind all matter; names are case-insensitive.
function With([scriptblock]$Change){$c=RoundTrip $capture;& $Change $c;return $c}
$cases=@(
 {param($c) $c.UserModeDriverName.value=@($umd[0],$umd[1],$umd[3],$umd[2])},
 {param($c) $c.UserModeDriverName.value=@($umd[0],$umd[1],$umd[2])},
 {param($c) $c.UserModeDriverName.value=@($umd+'extra.dll')},
 {param($c) $c.UserModeDriverName.value=@($umd[0],$umd[1],$umd[2],$umd[3].ToUpperInvariant())},
 {param($c) $c.VulkanDriverName.kind='String';$c.VulkanDriverName.value=$icd[0]},
 {param($c) $c.OpenGLFlags.value=4},
 {param($c) $c.PSObject.Properties.Remove('OpenGLFlags')}
)
foreach($case in $cases){$observed=With $case;Must-Reject {Assert-KmdGraphicsRegistration $saved.graphics_registration $observed}}
$renamed=[ordered]@{usermodedrivername=$capture.UserModeDriverName;VULKANDRIVERNAME=$capture.VulkanDriverName;OpenGLFlags=$capture.OpenGLFlags}
Assert-KmdGraphicsRegistration $saved.graphics_registration $renamed

# Capture admission and unsupported kinds.
Must-Reject {Assert-KmdGraphicsRegistrationBaseline (RoundTrip @{UserModeDriverName=@{kind='MultiString';value=$umd}})}
Must-Reject {Assert-KmdGraphicsRegistrationBaseline (RoundTrip @{UserModeDriverName=@{kind='MultiString';value=@('a.dll',' ')};VulkanDriverName=@{kind='MultiString';value=$icd}})}
Must-Reject {Assert-KmdGraphicsRegistrationBaseline (RoundTrip @{UserModeDriverName=@{kind='MultiString';value=@()};VulkanDriverName=@{kind='MultiString';value=$icd}})}
$binary=New-FakeKey @{UserModeDriverName=@{kind='MultiString';value=$umd};VulkanDriverName=@{kind='MultiString';value=$icd};D3DBlob=@{kind='Binary';value=[byte[]]@(1,2)}}
Must-Reject {Assert-KmdGraphicsRegistrationBaseline (Read-KmdGraphicsRegistration $binary)}
# An install-added value of an unsupported kind is still removed.
$binary.Store.Remove('D3DBlob');$binary.Store['D3DBlob2']=@{kind='Binary';value=[byte[]]@(3)}
$r=Set-KmdGraphicsRegistration -Key $binary -Saved $saved.graphics_registration
if($binary.Store.Contains('D3DBlob2') -or !$binary.Store.Contains('OpenGLFlags')){throw 'Unsupported install-added value not removed or capture not written'}

# Registration files: absolute paths hashed, a bare name resolved against System32, absence recorded.
$system=Join-Path $Out 'System32';[void](New-Item -ItemType Directory $system)
[IO.File]::WriteAllText((Join-Path $system 'stub.dll'),'stub')
[IO.File]::WriteAllText((Join-Path $Out 'umd.dll'),'umd')
$fileReg=RoundTrip @{UserModeDriverName=@{kind='MultiString';value=@('stub.dll',(Join-Path $Out 'umd.dll'),(Join-Path $Out 'umd.dll'))};VulkanDriverName=@{kind='MultiString';value=@((Join-Path $Out 'missing.json'))}}
$files=Get-KmdRegistrationFiles $fileReg -System32 $system
if(@($files.Keys).Count -ne 3 -or $files[(Join-Path $system 'stub.dll')] -ne (Get-FileHash (Join-Path $system 'stub.dll')).Hash -or $files[(Join-Path $Out 'missing.json')] -ne 'missing'){throw 'Registration files wrong'}
$savedFiles=RoundTrip $files
Assert-KmdRegistrationFiles $savedFiles (Get-KmdRegistrationFiles $fileReg -System32 $system)
[IO.File]::WriteAllText((Join-Path $Out 'umd.dll'),'changed')
Must-Reject {Assert-KmdRegistrationFiles $savedFiles (Get-KmdRegistrationFiles $fileReg -System32 $system)}
'PASS: four-entry list restored through JSON, install-added values removed, 7 entry-level mismatches, case-insensitive names, 4 capture refusals, file identity'
