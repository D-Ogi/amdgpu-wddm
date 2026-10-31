# Called after the caller chose its architecture, CRT and SDK. Return object
# paths only; compiler output is written to the host stream.
# -RelativeObjects compiles in -Out and names each object relative to it. With
# clang-cl's -ffile-compilation-dir=. the object's S_OBJNAME is then the same
# for every output directory, which a /Brepro link needs for one PDB GUID (BD-268).
param([Parameter(Mandatory=$true)][string]$Compiler,
      [Parameter(Mandatory=$true)][string]$Out,
      [string[]]$Flags=@(),[switch]$Exports,[switch]$RelativeObjects)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path "$PSScriptRoot\..\..").Path
New-Item -ItemType Directory -Force $Out | Out-Null
$sources='state','transfer','windows_lock','windows','client'
$objects=@()
if($RelativeObjects){Push-Location -LiteralPath $Out}
try {
    foreach($name in $sources) {
        $obj=Join-Path $Out "$name.obj"
        $target=if($RelativeObjects){"$name.obj"}else{$obj}
        $extra=@();if($Exports){$extra+='/DAMDGPU_CLIENT_EXPORTS'}
        & $Compiler /nologo /TC /W4 /WX /c @Flags @extra "/Fo$target" "$repo\driver\state\$name.c" | Out-Host
        if($LASTEXITCODE){throw "State client $name failed to compile"}
        $objects+=$obj
    }
} finally { if($RelativeObjects){Pop-Location} }
$objects
