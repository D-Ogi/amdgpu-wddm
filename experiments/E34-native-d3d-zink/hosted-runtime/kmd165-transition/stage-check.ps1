$ErrorActionPreference='Stop'
$out='C:\BC250\m12\candidate07165'
foreach($file in (Get-ChildItem $out -Filter '*.ps1')){
 $tokens=$null;$errors=$null
 [System.Management.Automation.Language.Parser]::ParseFile($file.FullName,[ref]$tokens,[ref]$errors)|Out-Null
 if($errors.Count){throw ('PS5 parser failure: '+$file.Name)}
 if([IO.File]::ReadAllText($file.FullName) -match '[^\x00-\x7F]'){throw 'Non-ASCII script'}
}
& "$out\dispatch165.ps1" -Mode Prepare
