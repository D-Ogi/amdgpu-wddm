# Sign the installed rehearsal artifact after the submission CAB is complete.
function Invoke-DriverMockAttestation {
    param([Parameter(Mandatory)][string]$DriverDirectory,
        [Parameter(Mandatory)][string]$ImageIdentityFile,
        [Parameter(Mandatory)][string]$SignTool,
        [Parameter(Mandatory)][string]$Inf2Cat,
        [Parameter(Mandatory)][string]$TestCertificate,
        [Parameter(Mandatory)][scriptblock]$SignFile,
        [switch]$KmdAlreadySigned,
        [ValidateSet('pinned-test','system')][string]$ExpectedRoot = 'pinned-test')
    . (Join-Path $PSScriptRoot 'driver-signature.ps1')
    $identity = Join-Path $PSScriptRoot 'image_identity.py'
    $python = (Get-Command python).Source
    $r = Invoke-Headless -File $python -Arguments @($identity,'--directory',$DriverDirectory,'--write',$ImageIdentityFile) -TimeoutSeconds 120
    if ($r.code -ne 0) { throw $r.text }
    $images = @(Get-ChildItem -LiteralPath $DriverDirectory -Recurse -File | Where-Object Extension -in '.sys','.dll','.exe')
    foreach ($pe in $images) {
        if (-not ($KmdAlreadySigned -and $pe.Name -ieq 'bc250kmd.sys')) { & $SignFile $pe.FullName }
        Test-DriverFileSignature -Path $pe.FullName -SignTool $SignTool -ExpectedRoot $ExpectedRoot -TestCertificate $TestCertificate
    }
    $r = Invoke-Headless -File $python -Arguments @($identity,'--directory',$DriverDirectory,'--check',$ImageIdentityFile) -TimeoutSeconds 120
    if ($r.code -ne 0) { throw $r.text }
    $r = Invoke-Headless -File $Inf2Cat -Arguments @("/driver:$DriverDirectory",'/os:10_X64','/uselocaltime') -TimeoutSeconds 300
    if ($r.code -ne 0) { throw "mock-attestation Inf2Cat failed: $($r.text)" }
    & $SignFile (Join-Path $DriverDirectory 'bc250kmd.cat')
    Test-DriverFileSignature -Path (Join-Path $DriverDirectory 'bc250kmd.cat') -SignTool $SignTool -ExpectedRoot $ExpectedRoot -TestCertificate $TestCertificate
    Write-Host "PASS mock-attestation: $($images.Count) embedded signatures, unchanged image identities, regenerated catalog"
}
