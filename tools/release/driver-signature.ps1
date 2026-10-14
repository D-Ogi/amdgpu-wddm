# Explicit trust mode, never a fallback after /pa fails. No root-store mutation.
param([string]$VerifyPath, [string]$PinnedCertificate)
function Test-DriverFileSignature {
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][string]$SignTool,
        [ValidateSet('pinned-test','system')][string]$ExpectedRoot = 'system', [string]$TestCertificate)
    if ($ExpectedRoot -eq 'system') {
        $r = Invoke-Headless -File $SignTool -Arguments @('verify','/pa',$Path) -TimeoutSeconds 120
        if ($r.code -ne 0) { throw "system signature verification failed: $($r.text)" }
    } else {
        if (-not $TestCertificate) { throw 'pinned-test requires the release public certificate' }
        if ($PSVersionTable.PSVersion.Major -gt 5) {
            $ps51 = Join-Path $env:windir 'System32\WindowsPowerShell\v1.0\powershell.exe'
            $r = Invoke-Headless -File $ps51 -Arguments @('-NoProfile','-NonInteractive','-File',
                (Join-Path $PSScriptRoot 'driver-signature.ps1'),'-VerifyPath',$Path,'-PinnedCertificate',$TestCertificate) -TimeoutSeconds 120
            if ($r.code -ne 0) { throw "pinned signature verification failed: $($r.text)" }
            return
        }
        if (-not ('AmdgpuWddm.Release.DriverSignature' -as [type])) {
            Add-Type -Path (Join-Path $PSScriptRoot 'DriverSignature.cs') -ReferencedAssemblies @('System.Security','System.Core')
        }
        [AmdgpuWddm.Release.DriverSignature]::Verify($Path,$TestCertificate)
    }
}
if ($VerifyPath) {
    $ErrorActionPreference = 'Stop'
    Test-DriverFileSignature -Path $VerifyPath -SignTool 'unused' -ExpectedRoot pinned-test -TestCertificate $PinnedCertificate
    'PASS pinned test-root signature, signed SHA-256 content and code-signing EKU'
}
