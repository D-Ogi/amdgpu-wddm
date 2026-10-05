# Development PC: every .ps1 under the given roots parses under Windows PowerShell 5.1 (the lab's shell).
# -File passes one string, so several roots are separated by ';'.
param([Parameter(Mandatory)][string]$Root)
$ErrorActionPreference='Stop'
$files=@($Root.Split(';')|Where-Object {$_}|ForEach-Object {Get-ChildItem -LiteralPath $_ -Filter '*.ps1' -File -Recurse})
$failed=0
foreach($file in $files){
 $tokens=$null;$errors=$null
 [void][System.Management.Automation.Language.Parser]::ParseFile($file.FullName,[ref]$tokens,[ref]$errors)
 if($errors.Count){$failed++;foreach($e in $errors){"PARSE $($file.FullName):$($e.Extent.StartLineNumber) $($e.Message)"}}
}
"parsed $($files.Count) files, $failed with errors, PowerShell $($PSVersionTable.PSVersion)"
if($failed -or !$files.Count){exit 1}
