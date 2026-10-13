# Called after the caller chose its architecture, CRT and SDK. Return object
# paths only; compiler output is written to the host stream.
param([Parameter(Mandatory=$true)][string]$Compiler,
      [Parameter(Mandatory=$true)][string]$Out,
      [string[]]$Flags=@(),[switch]$Exports)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path "$PSScriptRoot\..\..").Path
New-Item -ItemType Directory -Force $Out | Out-Null
$sources='state','transfer','windows_lock','windows','client'
$objects=@()
foreach($name in $sources) {
    $obj=Join-Path $Out "$name.obj"
    $extra=@();if($Exports){$extra+='/DAMDGPU_CLIENT_EXPORTS'}
    & $Compiler /nologo /TC /W4 /WX /c @Flags @extra "/Fo$obj" "$repo\driver\state\$name.c" | Out-Host
    if($LASTEXITCODE){throw "State client $name failed to compile"}
    $objects+=$obj
}
$objects
