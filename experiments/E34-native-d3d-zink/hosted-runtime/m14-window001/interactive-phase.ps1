param([Parameter(Mandatory)][ValidateSet('Cpu','Gpu')][string]$Phase)
$ErrorActionPreference='Stop'
try {
 & "$PSScriptRoot\phase.ps1" -Phase $Phase *> "$PSScriptRoot\$Phase-console.log"
} catch {
 $_ | Out-String | Set-Content "$PSScriptRoot\$Phase-console.err"
 exit 1
}
