# LAB (elevated SSH): DP audio step 3 - the full format blobs of the active endpoint, read only.
$ErrorActionPreference = 'Continue'
$pp = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\{<endpoint-0-guid-removed>}\Properties'
$k = Get-Item -LiteralPath $pp
foreach ($n in '{f19f064d-082c-4e27-bc73-6882a1bb8e4c},0','{e4870e26-3cc5-4cd2-ba46-ca0a9a70ed04},0','{3d6e1656-2e50-4c4c-8d85-d0acae3c6c68},2','{624f56de-fd24-473e-814a-de40aacaed16},3','{74e4357a-7959-47a4-a8b2-feb2ffd48427},2','{908dba32-edff-4c28-8e45-c918561f6748},2') {
  $b = $k.GetValue($n)
  if ($null -eq $b) { "$n absent"; continue }
  "$n  bytes=$($b.Length)"
  "   hex " + (($b | ForEach-Object { '{0:X2}' -f $_ }) -join '')
  # PROPVARIANT VT_BLOB: 4 bytes vt, 4 bytes reserved, then the payload; a WAVEFORMATEX starts at 8.
  if ($b.Length -ge 26) {
    "   as WAVEFORMATEX at offset 8: tag=0x{0:X4} ch={1} rate={2} avg={3} align={4} bits={5} cbSize={6}" -f `
      ([BitConverter]::ToUInt16($b,8)), ([BitConverter]::ToUInt16($b,10)), ([BitConverter]::ToUInt32($b,12)), `
      ([BitConverter]::ToUInt32($b,16)), ([BitConverter]::ToUInt16($b,20)), ([BitConverter]::ToUInt16($b,22)), ([BitConverter]::ToUInt16($b,24))
  }
}
