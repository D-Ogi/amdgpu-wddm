# Offline gate of the emergency channel. It parses every script of the kit with the Windows PowerShell 5.1 parser
# (the engine the listener runs under on unit A) and compiles lab-emerg.py. Nothing is executed, and the lab is
# never contacted. Run it with powershell.exe, not pwsh: the parser must be the one that will read the script.
#   powershell.exe -NoProfile -File tools\win\lab-emerg\parse-check.ps1
# Exit code 0 means every file parsed and compiled.
$ErrorActionPreference = 'Stop'
$bad = 0
$files = @(Get-ChildItem -Path $PSScriptRoot -Filter *.ps1 -File) +
         @(Get-ChildItem -Path (Join-Path $PSScriptRoot 'scripts') -Filter *.ps1 -File -ErrorAction SilentlyContinue)
foreach ($f in $files) {
    $tokens = $null; $errors = $null
    [void][Management.Automation.Language.Parser]::ParseFile($f.FullName, [ref]$tokens, [ref]$errors)
    if ($errors.Count) {
        $bad++
        '{0}: {1} parse errors' -f $f.Name, $errors.Count
        $errors | ForEach-Object { '  line {0}: {1}' -f $_.Extent.StartLineNumber, $_.Message }
    }
}
"parsed $($files.Count) PowerShell scripts, $bad with errors"
$py = Join-Path $PSScriptRoot 'lab-emerg.py'
# -X utf8 instead of an encoding= argument: a quoted keyword does not survive the native argument handoff.
& python -X utf8 -c 'import ast,sys; ast.parse(open(sys.argv[1]).read(), sys.argv[1])' $py
if ($LASTEXITCODE -ne 0) { $bad++; 'lab-emerg.py does not compile' } else { 'lab-emerg.py compiles' }
if ($bad) { 'FAIL'; exit 1 }
'PASS'
exit 0
