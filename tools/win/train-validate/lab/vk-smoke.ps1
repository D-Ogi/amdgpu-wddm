# Vulkan smoke on the INSTALLED system ICD and on the installed wow64 ICD, plus the x86 arm. Generic: it
# names no train and no package, and -Out names its own output directory. Read-only except for the two output directories under C:\BC250\tmp.
#   vulkaninfo  the release's own tools\vulkaninfo.exe --summary, first through the registered loader entry
#               (what an application sees), then with VK_DRIVER_FILES on the installed radeon_icd.json
#   vkfill-x64  vkfillcheck-x64 --icd <InstallRoot>\vulkan\vulkan_radeon.dll (the system ICD renders)
#   vkfill-x86  vkfillcheck-x86 --icd <InstallRoot>\wow64\vulkan\vulkan_radeon.dll (32-bit, ICD 887A08E5)
# Each step is one call; the whole script stays well under the three-minute bound.
param([ValidateSet('vulkaninfo', 'vkfill-x64', 'vkfill-x86', 'all')][string]$Step = 'all',
      [string]$Out = 'C:\BC250\tmp\train-vk')
$ErrorActionPreference = 'Continue'
$inst = [string](Get-ItemProperty 'HKLM:\SOFTWARE\amdgpu-wddm\Release' -Name InstallRoot).InstallRoot
$out = $Out
New-Item -ItemType Directory -Force $out | Out-Null
$cli = Join-Path $inst 'tools\bc250kmd_cli.exe'
function Faults { $l = & $cli log 2>&1; '{0} faults, {1} timeouts' -f ($l | Select-String 'GPU FAULT').Count, ($l | Select-String 'HARDWARE FENCE TIMEOUT|ResetEngine node').Count }
function Sha8($p) { if (Test-Path -LiteralPath $p) { (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.Substring(0, 8) } else { 'ABSENT' } }
"step $Step start $([DateTime]::UtcNow.ToString('o')); before: $(Faults)"
"system ICD   $(Sha8 (Join-Path $inst 'vulkan\vulkan_radeon.dll'))  $(Join-Path $inst 'vulkan\vulkan_radeon.dll')"
"wow64 ICD    $(Sha8 (Join-Path $inst 'wow64\vulkan\vulkan_radeon.dll'))  $(Join-Path $inst 'wow64\vulkan\vulkan_radeon.dll')"
"vulkaninfo   $(Sha8 (Join-Path $inst 'tools\vulkaninfo.exe'))"

if ($Step -in 'vulkaninfo', 'all') {
    $vi = Join-Path $inst 'tools\vulkaninfo.exe'
    foreach ($mode in 'registered', 'driver-files') {
        '--- vulkaninfo --summary ({0})' -f $mode
        $o = "$out\vulkaninfo-$mode.txt"; $e = "$out\vulkaninfo-$mode.err"
        if ($mode -eq 'driver-files') {
            $env:VK_DRIVER_FILES = Join-Path $inst 'vulkan\radeon_icd.json'
            $env:VK_ICD_FILENAMES = $env:VK_DRIVER_FILES
        } else {
            Remove-Item Env:\VK_DRIVER_FILES -ErrorAction SilentlyContinue
            Remove-Item Env:\VK_ICD_FILENAMES -ErrorAction SilentlyContinue
        }
        $s = Get-Date
        $p = Start-Process -FilePath $vi -ArgumentList '--summary' -Wait -PassThru -NoNewWindow -RedirectStandardOutput $o -RedirectStandardError $e
        'exit {0} after {1:N1} s' -f $p.ExitCode, ((Get-Date) - $s).TotalSeconds
        Get-Content $o -ErrorAction SilentlyContinue | Select-String -Pattern 'Vulkan Instance Version|deviceName|driverName|driverInfo|apiVersion|driverVersion|deviceType|GPU\d' | Select-Object -First 14 | ForEach-Object { '  ' + $_.Line.Trim() }
        $err = @(Get-Content $e -ErrorAction SilentlyContinue)
        if ($err.Count) { '  stderr: ' + ($err | Select-Object -First 3) }
    }
    Remove-Item Env:\VK_DRIVER_FILES -ErrorAction SilentlyContinue
    Remove-Item Env:\VK_ICD_FILENAMES -ErrorAction SilentlyContinue
}
if ($Step -in 'vkfill-x64', 'all') {
    '--- vkfillcheck x64 on the system ICD'
    $exe = 'C:\BC250\vkfillcheck\vkfillcheck-x64.exe'
    $icd = Join-Path $inst 'vulkan\vulkan_radeon.dll'
    "client $(Sha8 $exe); icd $(Sha8 $icd)"
    $s = Get-Date
    $o = & $exe --icd $icd --deadline 40 2>&1
    $code = $LASTEXITCODE
    $o | Out-File -Encoding utf8 "$out\vkfill-system-x64.txt"
    $o | Select-String -Pattern '^icd module|^device|^cases|^result|FAIL |error|deadline' | Select-Object -Last 8 | ForEach-Object { $_.Line.Substring(0, [Math]::Min(200, $_.Line.Length)) }
    'exit {0} after {1:N1} s' -f $code, ((Get-Date) - $s).TotalSeconds
}
if ($Step -in 'vkfill-x86', 'all') {
    '--- vkfillcheck x86 on the wow64 Vulkan ICD'
    $exe = 'C:\BC250\vkfillcheck\vkfillcheck-x86.exe'
    $icd = Join-Path $inst 'wow64\vulkan\vulkan_radeon.dll'
    "client $(Sha8 $exe); icd $(Sha8 $icd)"
    $s = Get-Date
    $o = & $exe --icd $icd --deadline 40 2>&1
    $code = $LASTEXITCODE
    $o | Out-File -Encoding utf8 "$out\vkfill-wow64-x86.txt"
    $o | Select-String -Pattern '^icd module|^device|^cases|^result|FAIL |error|deadline' | Select-Object -Last 8 | ForEach-Object { $_.Line.Substring(0, [Math]::Min(200, $_.Line.Length)) }
    'exit {0} after {1:N1} s' -f $code, ((Get-Date) - $s).TotalSeconds
}
"after: $(Faults)"
