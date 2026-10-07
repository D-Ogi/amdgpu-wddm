# Development PC, read-only: the SYS and CAT of a package are signed by the certificate the package carries.
# Chain trust is not asked for here (the lab test root need not be trusted on this PC); the signer identity is.
# -Certificate defaults to the package's bc250-lab-test.cer; a release package passes its payload\cert file.
param([Parameter(Mandatory)][string]$Package,[string]$Certificate)
$ErrorActionPreference='Stop'
if(!$Certificate){$Certificate=Join-Path $Package 'bc250-lab-test.cer'}
$cer=New-Object Security.Cryptography.X509Certificates.X509Certificate2 $Certificate
$result=[ordered]@{package=$Package;certificate=$cer.Thumbprint;files=[ordered]@{}}
foreach($name in @('bc250kmd.sys','bc250kmd.cat')){
 $s=Get-AuthenticodeSignature -LiteralPath (Join-Path $Package $name)
 $signer=if($s.SignerCertificate){$s.SignerCertificate.Thumbprint}else{$null}
 $result.files[$name]=@{status=[string]$s.Status;signer=$signer;match=($signer -eq $cer.Thumbprint)}
 if($signer -ne $cer.Thumbprint){$result|ConvertTo-Json -Depth 4;throw "Signer mismatch: $name"}
}
$result|ConvertTo-Json -Depth 4
