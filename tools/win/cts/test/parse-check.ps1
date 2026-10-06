param([string[]]$Path)
$bad = 0
foreach ($p in $Path) {
    $tokens = $null; $errors = $null
    [void][System.Management.Automation.Language.Parser]::ParseFile($p, [ref]$tokens, [ref]$errors)
    if ($errors.Count) { $bad++; foreach ($e in $errors) { Write-Output ("{0}:{1}: {2}" -f $p, $e.Extent.StartLineNumber, $e.Message) } }
    else { Write-Output "$p parses (PowerShell $($PSVersionTable.PSVersion))" }
}
exit $bad
