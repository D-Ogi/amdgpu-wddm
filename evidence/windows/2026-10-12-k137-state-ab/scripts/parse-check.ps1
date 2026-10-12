# Self-check: parse every .ps1 of this kit with the PowerShell 5.1 parser. Nothing here runs the scripts.
#   powershell -NoProfile -ExecutionPolicy Bypass -File parse-check.ps1
$bad = 0
Get-ChildItem -Path $PSScriptRoot -Filter *.ps1 | ForEach-Object {
    $tokens = $null
    $errors = $null
    [void][System.Management.Automation.Language.Parser]::ParseFile($_.FullName, [ref]$tokens, [ref]$errors)
    if ($errors.Count -gt 0) {
        $bad = $bad + 1
        Write-Output ($_.Name + ': ' + $errors.Count + ' parse error(s)')
        foreach ($e in $errors) { Write-Output ('  line ' + $e.Extent.StartLineNumber + ': ' + $e.Message) }
    } else {
        Write-Output ($_.Name + ': OK (' + $tokens.Count + ' tokens)')
    }
}
Write-Output ('files with errors: ' + $bad)
exit $bad
