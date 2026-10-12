# Parses every .ps1 of this kit and of the 2026-10-12 kit it extends, with the Windows PowerShell 5.1
# parser that the lab runs. Development PC only: it starts nothing and reads only these files.
#
#   powershell -ExecutionPolicy Bypass -File parse-check.ps1
#
# Exit 0 when every file parses, 1 otherwise.
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$old = Join-Path (Split-Path -Parent (Split-Path -Parent $here)) 'kit'      # scratch\bd093\kit
$files = @()
$files += @(Get-ChildItem -Path $here -Filter *.ps1 -File | ForEach-Object { $_.FullName })
if (Test-Path $old) { $files += @(Get-ChildItem -Path $old -Filter *.ps1 -File | ForEach-Object { $_.FullName }) }
$bad = 0
foreach ($f in $files) {
    $errors = $null
    $null = [System.Management.Automation.PSParser]::Tokenize((Get-Content -Raw -Path $f), [ref]$errors)
    $count = @($errors).Count
    # The directory is printed as well: both kits carry a file called parse-check.ps1, so the leaf name alone
    # looks like the same file twice. The five files are five distinct files.
    $dir = Split-Path -Parent $f
    $tag = (Split-Path -Leaf (Split-Path -Parent $dir)) + '\' + (Split-Path -Leaf $dir)
    Write-Output ("parse " + $tag + "\" + (Split-Path -Leaf $f) + " errors=" + $count)
    if ($count -gt 0) {
        $bad = 1
        foreach ($e in $errors) { Write-Output ("  line " + $e.Token.StartLine + ": " + $e.Message) }
    }
}
Write-Output ("parse files=" + $files.Count + " failed=" + $bad)
exit $bad
