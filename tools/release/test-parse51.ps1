# Gate: every .ps1 in -Directory parses under the running PowerShell. build-release.ps1 and test-dryrun.ps1 run it
# with Windows PowerShell 5.1 (System32\WindowsPowerShell\v1.0\powershell.exe), the version a tester has.
param([Parameter(Mandatory)][string]$Directory)
$bad = 0
foreach ($f in Get-ChildItem -LiteralPath $Directory -Filter *.ps1 -File) {
    $tokens = $null; $errors = $null
    [void][Management.Automation.Language.Parser]::ParseFile($f.FullName, [ref]$tokens, [ref]$errors)
    if ($errors.Count) { $bad++; foreach ($e in $errors) { "  PARSE ERROR {0}:{1} {2}" -f $f.Name, $e.Extent.StartLineNumber, $e.Message } }
    else { "  parses under PowerShell {0}: {1}" -f $PSVersionTable.PSVersion, $f.Name }
}
exit $bad
