# Builds the per-machine payload (answer file, Wi-Fi profile, SSH key, OpenSSH zip) from the secrets folder.
# Output goes to <SecretsDir>\win\payload and never into the repository. Nothing secret is printed.
#
#   pwsh tools\wininstall\make_payload.ps1 -SecretsDir $env:BC250_ROOT\secrets -OpenSshZip P:\...\OpenSSH-Win64.zip

param(
    [Parameter(Mandatory)][string]$SecretsDir,
    [Parameter(Mandatory)][string]$OpenSshZip,
    [string]$Account = 'bc250',
    [int]$WifiNetworkIndex = 0
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$winDir = Join-Path $SecretsDir 'win'
$out = Join-Path $winDir 'payload'
$bc = Join-Path $out 'BC250'
New-Item -ItemType Directory -Force $bc | Out-Null

# Account password: generated once, kept in secrets\win\account.txt
$accountFile = Join-Path $winDir 'account.txt'
if (Test-Path $accountFile) {
    $password = (Get-Content $accountFile | Where-Object { $_ -like 'password=*' }) -replace '^password=', ''
} else {
    $alphabet = 'abcdefghijkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789'
    $bytes = [byte[]]::new(20)
    [System.Security.Cryptography.RandomNumberGenerator]::Fill($bytes)
    $password = -join ($bytes | ForEach-Object { $alphabet[$_ % $alphabet.Length] })
    Set-Content -Path $accountFile -Value @("account=$Account", "password=$password") -Encoding utf8
}
$esc = { param($s) [System.Security.SecurityElement]::Escape($s) }

$template = Get-Content (Join-Path $here 'unattend.template.xml') -Raw
$rendered = $template.Replace('@@ACCOUNT@@', (& $esc $Account)).Replace('@@PASSWORD@@', (& $esc $password))
Set-Content -Path (Join-Path $out 'unattend.xml') -Value $rendered -Encoding utf8 -NoNewline

# Wi-Fi profile from the same wpa_supplicant.conf the diagnostic stick uses
$conf = Get-Content (Join-Path $SecretsDir 'net\wpa_supplicant.conf') -Raw
$blocks = [regex]::Matches($conf, 'network\s*=\s*\{(.*?)\}', 'Singleline')
$wifi = 'none'
if ($blocks.Count -gt $WifiNetworkIndex) {
    $body = $blocks[$WifiNetworkIndex].Groups[1].Value
    $ssid = [regex]::Match($body, '(?m)^\s*ssid\s*=\s*"(.*)"\s*$').Groups[1].Value
    $pskQuoted = [regex]::Match($body, '(?m)^\s*psk\s*=\s*"(.*)"\s*$')
    $pskRaw = [regex]::Match($body, '(?m)^\s*psk\s*=\s*([0-9a-fA-F]{64})\s*$')
    if ($ssid -and ($pskQuoted.Success -or $pskRaw.Success)) {
        $keyType = if ($pskQuoted.Success) { 'passPhrase' } else { 'networkKey' }
        $key = if ($pskQuoted.Success) { $pskQuoted.Groups[1].Value } else { $pskRaw.Groups[1].Value }
        $hex = -join ([System.Text.Encoding]::UTF8.GetBytes($ssid) | ForEach-Object { $_.ToString('X2') })
        $profile = @"
<?xml version="1.0"?>
<WLANProfile xmlns="http://www.microsoft.com/networking/WLAN/profile/v1">
  <name>$(& $esc $ssid)</name>
  <SSIDConfig><SSID><hex>$hex</hex><name>$(& $esc $ssid)</name></SSID></SSIDConfig>
  <connectionType>ESS</connectionType>
  <connectionMode>auto</connectionMode>
  <MSM><security>
    <authEncryption><authentication>WPA2PSK</authentication><encryption>AES</encryption><useOneX>false</useOneX></authEncryption>
    <sharedKey><keyType>$keyType</keyType><protected>false</protected><keyMaterial>$(& $esc $key)</keyMaterial></sharedKey>
  </security></MSM>
</WLANProfile>
"@
        Set-Content -Path (Join-Path $bc 'wifi.xml') -Value $profile -Encoding utf8
        $wifi = "network #$WifiNetworkIndex"
    }
}

Copy-Item (Join-Path $SecretsDir 'client\bc250diag_ed25519.pub') (Join-Path $bc 'authorized_keys') -Force
Copy-Item $OpenSshZip (Join-Path $bc 'OpenSSH-Win64.zip') -Force
Copy-Item (Join-Path $here 'firstlogon.ps1') $bc -Force

Write-Host "payload: $out"
Write-Host "account: $Account (password in secrets\win\account.txt), wifi profile: $wifi"
Get-ChildItem $out -Recurse -File | ForEach-Object { '{0,10}  {1}' -f $_.Length, $_.FullName.Substring($out.Length + 1) }
