# Parse only. Never execute lab scripts or inspect the development desktop.
param()
$ErrorActionPreference='Stop'
$repo=(Resolve-Path "$PSScriptRoot\..\..").Path
$files=@(Get-ChildItem -LiteralPath "$repo\tools\win\train-validate\lab" -Filter '*.ps1')
$files+=Get-Item "$repo\tools\win\lab-emerg\scripts\kmd-state.ps1"
$files+=Get-Item "$repo\tools\release\build-release.ps1"
$files+=Get-Item "$repo\tools\release\state-package.ps1"
foreach($file in $files) {
    $tokens=$null; $parseErrors=$null
    [void][Management.Automation.Language.Parser]::ParseFile($file.FullName,[ref]$tokens,[ref]$parseErrors)
    if($parseErrors.Count){throw ($file.Name+': '+($parseErrors -join ', '))}
}
"PowerShell $($PSVersionTable.PSVersion): $($files.Count) state consumer scripts parse"
