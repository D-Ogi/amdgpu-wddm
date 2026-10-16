# Pure CPU test of the real Verify branch and shared session selector. No host process/module reads.
param([string]$Installer=(Join-Path $PSScriptRoot 'installer'))
$ErrorActionPreference='Stop'
Add-Type -TypeDefinition @'
namespace AmdgpuWddmInstaller {
 public static class Wts {
  public static long SelectedConsole;
  public static long ConsoleSession() { return SelectedConsole; }
 }
}
'@
. (Join-Path $Installer 'dwm-session.ps1')
$script:own=0; $script:dwms=@(); $script:checks=0
function Get-Process { param($Name,$Id,$ErrorAction)
 if($PSBoundParameters.ContainsKey('Id')){return [pscustomobject]@{SessionId=$script:own}}
 if($Name -ne 'dwm'){throw 'Unexpected process query'}
 return $script:dwms
}
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $Installer 'install.ps1'),[ref]$tokens,[ref]$errors)
if($errors.Count){throw 'Installer parse failed'}
$node=@($ast.FindAll({param($n) $n -is [Management.Automation.Language.IfStatementAst] -and $n.Extent.Text.StartsWith('if ($forceCpu -eq 0)')},$true))
if($node.Count -ne 1){throw 'Expected one production GPU desktop verification branch'}
$branch=[scriptblock]::Create($node[0].Extent.Text)
function Case([string]$Label,[int]$Own,[long]$Console,[int]$DwmSession,[bool]$Module,[bool]$Expected,[int]$Selected){
 $script:own=$Own;[AmdgpuWddmInstaller.Wts]::SelectedConsole=$Console
 $script:dwms=@([pscustomobject]@{SessionId=$DwmSession;Modules=@([pscustomobject]@{FileName=$(if($Module){'C:\driver\bc250d3d_zink.dll'}else{'C:\driver\cpu.dll'})})})
 $forceCpu=0;$dwmOk=$true;$dwmNote='';$mySession=-1
 . $branch
 if($dwmOk -ne $Expected -or $mySession -ne $Selected){throw "Failed: $Label"}
 if($Selected -eq 0 -and $dwmNote -notlike '*no interactive desktop session*'){throw 'Absent desktop was not explicit'}
 $script:checks++;"PASS $Label"
}
Case 'SYSTEM selects active console GPU DWM' 0 1 1 $true $true 1
Case 'SSH selects active console GPU DWM' 0 3 3 $true $true 3
Case 'Interactive caller keeps own session' 2 1 2 $true $true 2
Case 'Other session GPU DWM does not pass' 2 1 1 $true $false 2
Case 'Console CPU DWM does not pass' 0 1 1 $false $false 1
Case 'Console DWM absent does not pass' 0 1 2 $true $false 1
Case 'No console sentinel refuses session zero module' 0 4294967295 0 $true $false 0
Case 'No desktop zero refuses session zero module' 0 0 0 $true $false 0
Case 'Failed console query refuses' 0 -1 0 $true $false 0
"PASS $script:checks desktop verification checks"
