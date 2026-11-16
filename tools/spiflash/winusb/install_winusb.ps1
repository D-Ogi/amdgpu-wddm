# Binds the inbox WinUSB driver to the CH341A (USB\VID_1A86&PID_5512) for flashrom and ch341a.py. Run elevated.
# Same method as Zadig/libwdi: a catalog for the INF, signed by a one-time self-signed certificate that is
# trusted on this machine only. The private key is deleted right after signing, so it cannot sign anything else.
# Work files (catalog, certificate, log) go to -WorkDir, never into the repository.
# Removal (elevated): pnputil /delete-driver <oemN.inf> /uninstall, then remove the printed thumbprint from
# Cert:\LocalMachine\Root and Cert:\LocalMachine\TrustedPublisher.
param(
    [Parameter(Mandatory)][string]$WorkDir,
    [string]$Kit = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64'
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $WorkDir | Out-Null
Copy-Item (Join-Path $PSScriptRoot 'ch341a-winusb.inf'), (Join-Path $PSScriptRoot 'ch341a.cdf') $WorkDir -Force
$log = Join-Path $WorkDir ('install-' + (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssZ') + '.log')
Start-Transcript -Path $log | Out-Null
try {
    Set-Location $WorkDir
    Remove-Item (Join-Path $WorkDir 'ch341a.cat') -ErrorAction SilentlyContinue
    & "$Kit\makecat.exe" -v (Join-Path $WorkDir 'ch341a.cdf')
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path (Join-Path $WorkDir 'ch341a.cat'))) { throw "makecat failed ($LASTEXITCODE)" }

    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=BC-250 lab CH341A WinUSB (local only)' `
        -CertStoreLocation Cert:\CurrentUser\My -KeyExportPolicy NonExportable -NotAfter (Get-Date).AddYears(10)
    "certificate thumbprint: $($cert.Thumbprint)"
    $cer = Join-Path $WorkDir 'ch341a-winusb.cer'
    Export-Certificate -Cert $cert -FilePath $cer | Out-Null
    Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
    Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null

    & "$Kit\signtool.exe" sign /fd sha256 /sha1 $cert.Thumbprint /s My (Join-Path $WorkDir 'ch341a.cat')
    $signRc = $LASTEXITCODE
    Remove-Item "Cert:\CurrentUser\My\$($cert.Thumbprint)" -DeleteKey
    'private key deleted'
    if ($signRc -ne 0) { throw "signtool failed ($signRc)" }

    pnputil /add-driver (Join-Path $WorkDir 'ch341a-winusb.inf') /install
    "pnputil exit $LASTEXITCODE"
    Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match 'VID_1A86&PID_5512' } |
        Format-List Status, FriendlyName, Class
    'INSTALL-WINUSB DONE'
} catch {
    "INSTALL-WINUSB FAILED: $_"
} finally {
    Stop-Transcript | Out-Null
}
