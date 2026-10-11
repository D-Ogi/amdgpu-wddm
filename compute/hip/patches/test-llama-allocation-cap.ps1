param([Parameter(Mandatory=$true)][string]$Source, [Parameter(Mandatory=$true)][string]$Out)
$ErrorActionPreference='Stop'
python (Join-Path $PSScriptRoot 'test-llama-allocation-cap.py') --source $Source --out $Out
if ($LASTEXITCODE) { throw 'fixture extraction failed' }
New-Item -ItemType Directory -Force (Join-Path $Out 'tmp') | Out-Null
$vs=& "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -products * -property installationPath
$vc=Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
& cmd /c "`"$vc`" >nul 2>&1 && set" | ForEach-Object {if($_ -match '^([^=]+)=(.*)$'){Set-Item "env:$($Matches[1])" $Matches[2]}}
$env:TEMP="$out/tmp";$env:TMP=$env:TEMP
Push-Location $out
try {
 foreach($name in @('limited','ordinary','mutant')) {
  $src=if($name -eq 'mutant'){'planner-mutant.cpp'}else{'planner-test.cpp'}
  $opts=@('/nologo','/EHsc','/W4','/WX',"/Fe:$name.exe",$src)
  if($name -ne 'ordinary'){$opts+='/DBC250_HIP_MAX_ALLOC_BYTES=0xfffc0000ULL'}
  & cl @opts
  if($LASTEXITCODE){throw "compile $name"}
  $output = @(& ".\$name.exe" 2>&1)
  $result = $LASTEXITCODE
  $output | Write-Output
  if ($name -eq 'mutant') {
   if ($result -ne 1 -or -not ($output -match '^FAIL CHECK ')) { throw 'mutant must fail by CHECK, not crash' }
  } elseif ($result -ne 0) { throw "unexpected result $name" }
 }
}finally{Pop-Location}
