# LAB (elevated SSH, session 0): DP audio step 3, trial 1 - read only. Nothing is written anywhere.
#   1. bc250kmd_cli dpaudio (check table + decision + record) and dpaudio state
#   2. the audio registers of docs/design/dp-audio.md by regcalc name
#   3. PnP state of the GPU audio function (1002:13FF) and of the HD Audio codec (HDAUDIO ... DEV_AA01)
#   4. the MMDevice render endpoints: state, name, formats
#   5. the KMD log lines of dpaudio
# Bound: a few seconds. Run with target.py ps; stdout is the evidence.
$ErrorActionPreference = 'Continue'
$cli = 'C:\Program Files\amdgpu-wddm\tools\bc250kmd_cli.exe'
function H($t) { "" ; "=== $t  utc $([DateTime]::UtcNow.ToString('HH:mm:ss.fff'))" }

H 'identity'
"host $env:COMPUTERNAME  utc $([DateTime]::UtcNow.ToString('o'))"
"boot $((Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o'))"
"cli  $cli  sha256 $((Get-FileHash -Algorithm SHA256 -LiteralPath $cli).Hash.Substring(0,8))"
$sys = 'C:\Windows\System32\drivers\bc250kmd.sys'
if (Test-Path -LiteralPath $sys) {
  "sys  $sys  sha256 $((Get-FileHash -Algorithm SHA256 -LiteralPath $sys).Hash.Substring(0,8))  version $((Get-Item $sys).VersionInfo.FileVersion)"
}
$par = 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
foreach ($v in 'EnableDpAudio','EnableDpAudioEndpoint','EnableDpAudioStream','EnableDpAudioEdid','EnableMmio','EnableDcnWrite') {
  $x = (Get-ItemProperty -LiteralPath $par -Name $v -ErrorAction SilentlyContinue).$v
  "switch $v = $(if ($null -eq $x) { 'absent' } else { $x })"
}

H 'bc250kmd_cli dpaudio'
& $cli dpaudio 2>&1
"exit=$LASTEXITCODE"

H 'bc250kmd_cli dpaudio state'
& $cli dpaudio state 2>&1
"exit=$LASTEXITCODE"

H 'audio registers by name (read only)'
$names = @(
  'mmDCCG_AUDIO_DTO_SOURCE','mmDCCG_AUDIO_DTO0_PHASE','mmDCCG_AUDIO_DTO0_MODULE',
  'mmDCCG_AUDIO_DTO1_PHASE','mmDCCG_AUDIO_DTO1_MODULE',
  'mmCLK4_0_CLK4_CLK2_CURRENT_CNT',
  'mmDIG0_AFMT_CNTL','mmDIG0_AFMT_AUDIO_SRC_CONTROL','mmDIG0_AFMT_AUDIO_PACKET_CONTROL',
  'mmDIG0_AFMT_AUDIO_PACKET_CONTROL2','mmDIG0_AFMT_STATUS','mmDIG0_AFMT_INFOFRAME_CONTROL0','mmDIG0_AFMT_60958_0',
  'mmDP0_DP_SEC_CNTL','mmDP0_DP_SEC_AUD_N','mmDP0_DP_SEC_AUD_M_READBACK','mmDP0_DP_SEC_TIMESTAMP',
  'mmDP0_DP_VID_STREAM_CNTL','mmDIG0_DIG_FE_CNTL','mmDIG0_DIG_BE_CNTL','mmHPD0_DC_HPD_INT_STATUS',
  'mmDIG1_AFMT_CNTL','mmDP1_DP_SEC_CNTL','mmDP1_DP_VID_STREAM_CNTL','mmHPD1_DC_HPD_INT_STATUS',
  'mmDC_PINSTRAPS','mmDIO_MEM_PWR_CTRL','mmDIO_MEM_PWR_STATUS',
  'mmAZALIA_F0_CODEC_ROOT_PARAMETER_VENDOR_AND_DEVICE_ID','mmAZALIA_F0_CODEC_ROOT_PARAMETER_REVISION_ID',
  'mmAZALIA_F0_CODEC_FUNCTION_PARAMETER_SUPPORTED_SIZE_RATES','mmAZALIA_F0_CODEC_FUNCTION_PARAMETER_POWER_STATES',
  'mmAZALIA_AUDIO_DTO','mmAZALIA_AUDIO_DTO_CONTROL','mmAZALIA_CONTROLLER_CLOCK_GATING',
  'mmAZALIA_DATA_DMA_CONTROL','mmAZALIA_BDL_DMA_CONTROL','mmAZALIA_CYCLIC_BUFFER_SYNC',
  'mmAZALIA_APPLICATION_POSITION_IN_CYCLIC_BUFFER','mmAZALIA_OUTPUT_STREAM_ARBITER_CONTROL',
  'mmDCCG_GATE_DISABLE_CNTL','mmDCCG_GATE_DISABLE_CNTL2'
)
foreach ($n in $names) {
  $o = (& $cli read $n 2>&1) -join ' | '
  "{0,-62} {1}" -f $n, $o
}

H 'PnP: GPU audio function and HD Audio codec'
foreach ($pat in 'PCI\VEN_1002&DEV_13FF*','PCI\VEN_1002&DEV_13FE*','HDAUDIO\*VEN_1002*') {
  foreach ($d in (Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like $pat })) {
    $inst = $d.InstanceId -replace '\\[^\\]*$', '\<inst>'
    "--- $($d.FriendlyName) [$($d.Status)] problem=$($d.Problem) class=$($d.Class) $inst"
    "    service=$($d.Service)"
    $k = "HKLM:\SYSTEM\CurrentControlSet\Enum\$($d.InstanceId)\Device Parameters\Interrupt Management\MessageSignaledInterruptProperties"
    $k2 = "HKLM:\SYSTEM\CurrentControlSet\Enum\$($d.InstanceId)"
    $msi = (Get-ItemProperty -LiteralPath $k -Name MSISupported -ErrorAction SilentlyContinue).MSISupported
    "    MSISupported=$(if ($null -eq $msi) { 'absent' } else { $msi })"
    foreach ($p in 'DEVPKEY_Device_LocationInfo','DEVPKEY_Device_DriverVersion','DEVPKEY_Device_DriverDesc','DEVPKEY_PciDevice_InterruptSupport') {
      $v = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName $p -ErrorAction SilentlyContinue).Data
      if ($null -ne $v) { "    $p = $v" }
    }
  }
}

H 'MMDevice render endpoints'
$root = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render'
foreach ($e in (Get-ChildItem -LiteralPath $root -ErrorAction SilentlyContinue)) {
  $state = (Get-ItemProperty -LiteralPath $e.PSPath -Name DeviceState -ErrorAction SilentlyContinue).DeviceState
  $pp = Join-Path $e.PSPath 'Properties'
  function P($g) { (Get-ItemProperty -LiteralPath $pp -Name $g -ErrorAction SilentlyContinue).$g }
  $name = P '{a45c254e-df1c-4efd-8020-67d146a850e0},2'
  $desc = P '{b3f8fa53-0004-438e-9003-51a46e139bfc},6'
  $iface = P '{b3f8fa53-0004-438e-9003-51a46e139bfc},1'
  $fmt = P '{f19f064d-082c-4e27-bc73-6882a1bb8e4c},0'
  $oem = P '{e4870e26-3cc5-4cd2-ba46-ca0a9a70ed04},3'
  $words = switch ($state) { 1 { 'ACTIVE' } 2 { 'DISABLED' } 4 { 'NOTPRESENT' } 8 { 'UNPLUGGED' } default { "?$state" } }
  "--- id=$($e.PSChildName)"
  "    state=$state ($words) name='$name' desc='$desc' iface='$iface'"
  foreach ($pair in @(,@('DeviceFormat',$fmt)) + @(,@('OEMFormat',$oem))) {
    $b = $pair[1]
    if ($b -is [byte[]] -and $b.Length -ge 18) {
      $tag = [BitConverter]::ToUInt16($b,0); $ch = [BitConverter]::ToUInt16($b,2)
      $rate = [BitConverter]::ToUInt32($b,4); $bits = [BitConverter]::ToUInt16($b,14)
      "    $($pair[0]): tag=0x{0:X4} ch={1} rate={2} bits={3}" -f $tag,$ch,$rate,$bits
    }
  }
}

H 'default render endpoint (policy config)'
$dr = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render'
"(the probe prints the effective default with its device= line)"

H 'KMD log: dpaudio lines'
$log = (& $cli log 2>&1)
"log lines total: $(@($log).Count)"
@($log) | Where-Object { $_ -match 'dpaudio|audio|azalia|afmt|dto' } | ForEach-Object { $_ }

H 'KMD log summary'
& $cli log summary only 2>&1 | Select-Object -First 25

H 'temperature'
& $cli telemetry 1 2>&1 | Select-Object -First 12
H 'done'
