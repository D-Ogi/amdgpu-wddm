# LAB (elevated SSH): DP audio step 3, trial 3 - read only. The MMDevice endpoint properties in full
# (name, container, formats, the engine's supported format) and the codec's audio descriptor as the KMD left it.
$ErrorActionPreference = 'Continue'
$cli = 'C:\Program Files\amdgpu-wddm\tools\bc250kmd_cli.exe'
$known = @{
  '{a45c254e-df1c-4efd-8020-67d146a850e0},2'  = 'PKEY_Device_FriendlyName'
  '{a45c254e-df1c-4efd-8020-67d146a850e0},14' = 'PKEY_Device_DeviceDesc'
  '{b3f8fa53-0004-438e-9003-51a46e139bfc},6'  = 'PKEY_DeviceInterface_FriendlyName'
  '{f19f064d-082c-4e27-bc73-6882a1bb8e4c},0'  = 'PKEY_AudioEngine_DeviceFormat'
  '{f19f064d-082c-4e27-bc73-6882a1bb8e4c},3'  = 'PKEY_AudioEngine_OEMFormat'
  '{e4870e26-3cc5-4cd2-ba46-ca0a9a70ed04},3'  = 'PKEY_AudioEndpoint_PhysicalSpeakers'
  '{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},0'  = 'PKEY_AudioEndpoint_FormFactor'
  '{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},3'  = 'PKEY_AudioEndpoint_PhysicalSpeakers'
  '{8486e691-ce03-4c0f-be72-e1bb9d5ad0f2},0'  = 'PKEY_AudioEndpoint_JackSubType'
  '{83da1e8a-5b5a-4a4c-9a9b-0a0e4e0e0e0e},0'  = '(unknown)'
}
function Fmt($b) {
  if ($b -is [byte[]] -and $b.Length -ge 18) {
    "tag=0x{0:X4} ch={1} rate={2} avg_bytes_s={3} align={4} bits={5}" -f ([BitConverter]::ToUInt16($b,0)),
      ([BitConverter]::ToUInt16($b,2)), ([BitConverter]::ToUInt32($b,4)), ([BitConverter]::ToUInt32($b,8)),
      ([BitConverter]::ToUInt16($b,12)), ([BitConverter]::ToUInt16($b,14))
  } else { "(not a WAVEFORMATEX, $($b.GetType().Name))" }
}
$root = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render'
foreach ($e in (Get-ChildItem -LiteralPath $root -ErrorAction SilentlyContinue)) {
  $state = (Get-ItemProperty -LiteralPath $e.PSPath -Name DeviceState -ErrorAction SilentlyContinue).DeviceState
  $words = switch ($state) { 1 { 'ACTIVE' } 2 { 'DISABLED' } 4 { 'NOTPRESENT' } 8 { 'UNPLUGGED' } default { "?$state" } }
  "=== endpoint {0}  state {1} ({2})" -f $e.PSChildName, $state, $words
  $pp = Join-Path $e.PSPath 'Properties'
  $k = Get-Item -LiteralPath $pp -ErrorAction SilentlyContinue
  if ($null -eq $k) { "   no Properties key"; continue }
  foreach ($n in ($k.GetValueNames() | Sort-Object)) {
    $v = $k.GetValue($n)
    $label = if ($known.ContainsKey($n)) { $known[$n] } else { '' }
    if ($v -is [byte[]]) {
      if ($n -like '{f19f064d*' -and $v.Length -ge 18) { "   {0,-44} {1} {2}" -f $n, $label, (Fmt $v) }
      else { "   {0,-44} {1} bytes[{2}] {3}" -f $n, $label, $v.Length, (($v | Select-Object -First 24 | ForEach-Object { '{0:X2}' -f $_ }) -join '') }
    } else { "   {0,-44} {1} {2}" -f $n, $label, $v }
  }
}
""
"=== the KMD's endpoint 0 audio descriptor and sink (indirect reads, read only)"
& $cli dpaudio 2>&1 | Select-String -Pattern 'EP0_|EP1_|LPCM|sink' | ForEach-Object { $_.Line }
""
"=== KMD log: the configure and LPCM lines"
& $cli log 2>&1 | Select-String -Pattern 'dpaudio: (configure|LPCM)' | ForEach-Object { $_.Line }
""
"=== audio services and the codec's KS filters"
Get-Service -Name Audiosrv,AudioEndpointBuilder | Format-Table -AutoSize Name,Status | Out-String
"=== one-shot task present? (must be absent)"
"$([bool](Get-ScheduledTask -TaskName 'DP audio tone step 3' -ErrorAction SilentlyContinue))"
"=== dp audio switches in the registry (must be the release defaults 1/1/1, EnableDpAudioEdid absent)"
$par = 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
foreach ($v in 'EnableDpAudio','EnableDpAudioEndpoint','EnableDpAudioStream','EnableDpAudioEdid') {
  $x = (Get-ItemProperty -LiteralPath $par -Name $v -ErrorAction SilentlyContinue).$v
  "$v = $(if ($null -eq $x) { 'absent' } else { $x })"
}
"=== temperature"
(& $cli dpm 1 2>&1 | Select-String 'temperature_c') -join ' '
