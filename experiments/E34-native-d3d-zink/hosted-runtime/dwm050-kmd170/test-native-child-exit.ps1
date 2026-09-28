$ErrorActionPreference='Stop'
. "$PSScriptRoot\native-child-exit.ps1"
foreach($expected in @(0,7)){
 $p=Start-Process -FilePath $env:ComSpec -ArgumentList @('/c',"exit $expected") -WindowStyle Hidden -PassThru
 try{$actual=Wait-NativeChildExit $p 5000;if($actual -ne $expected){throw "Expected $expected got $actual"}}finally{$p.Dispose()}
}
'PASS: exact native exit0 and exit7 obtained through retained process handle'
