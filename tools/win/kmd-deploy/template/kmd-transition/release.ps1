# The installed release is the only source of the KMD client (owner, 2026-10-08: the harness must need nothing from
# the old lab directories C:\BC250\m8 to m14, which go away). The release installer writes InstallRoot into
# HKLM\SOFTWARE\amdgpu-wddm\Release, and <InstallRoot>\tools\bc250kmd_cli.exe is the client every query goes through.
# A missing release, a missing client, or a query form the client's own usage does not list is an error that says
# which. Resolve-KmdClient keeps the rule of <BC250_ROOT>\scratch\m15\native-caps001\kmd-client.ps1, the text the game
# harness carries: the release client or a refusal, never a fallback to another copy.
$KmdReleaseKey = 'HKLM:\SOFTWARE\amdgpu-wddm\Release'
$KmdClientRelativePath = 'tools\bc250kmd_cli.exe'

function Get-KmdReleaseRoot {
 param([string]$Key = 'HKLM:\SOFTWARE\amdgpu-wddm\Release')
 $root = [string](Get-ItemProperty -Path $Key -Name InstallRoot -ErrorAction SilentlyContinue).InstallRoot
 if ([string]::IsNullOrWhiteSpace($root)) { throw "No installed release: $Key has no InstallRoot" }
 if (!(Test-Path -LiteralPath $root -PathType Container)) { throw "The installed release root is absent: $root" }
 return $root
}

function Get-KmdReleaseClientPath {
 param([string]$Root)
 if (!$Root) { $Root = Get-KmdReleaseRoot }
 return (Join-Path $Root $KmdClientRelativePath)
}

# Client: the release client's path. Form: the usage text of each query the caller makes ('health read',
# 'log summary'); the client's own usage answers that once per client per script run.
function Resolve-KmdClient {
 param([string]$Client, [string[]]$Form = @())
 if (!$Client -or !(Test-Path -LiteralPath $Client -PathType Leaf)) {
  throw "No release KMD client '$Client' ($KmdReleaseKey InstallRoot\$KmdClientRelativePath)"
 }
 if ($Form.Count) {
  if ($script:KmdClientUsage -isnot [Collections.IDictionary]) { $script:KmdClientUsage = @{} }
  if (!$script:KmdClientUsage.Contains($Client)) {
   # The client prints its usage and exits non-zero without arguments: read it, and never let that stop the caller.
   $previous = $ErrorActionPreference
   $ErrorActionPreference = 'Continue'
   try { $script:KmdClientUsage[$Client] = [string](& $Client 2>&1 | Out-String) } catch { $script:KmdClientUsage[$Client] = '' } finally { $ErrorActionPreference = $previous }
  }
  $usage = [string]$script:KmdClientUsage[$Client]
  foreach ($name in $Form) {
   if (!$usage.Contains($name)) { throw "The release KMD client $Client does not list '$name'" }
  }
 }
 return $Client
}
