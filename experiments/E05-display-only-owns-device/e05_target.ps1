# E05 on the target, one phase per call, everything logged under C:\BC250\e05\out\<phase>-*.txt so that the
# lab PC can pull the directory as evidence. Runs elevated (the SSH session is).
#
#   e05_target.ps1 -Phase state      device, driver, problem code, display mode   (any time)
#   e05_target.ps1 -Phase sweep -Tag before|after|...    GC register sweep through bc250rd
#   e05_target.ps1 -Phase install    pnputil /add-driver bc250kmdod.inf /install
#   e05_target.ps1 -Phase cycle      disable + enable the device
#   e05_target.ps1 -Phase rollback   remove our package, rescan: Basic Display comes back
param(
    [Parameter(Mandatory)][ValidateSet('state', 'sweep', 'install', 'cycle', 'rollback')][string]$Phase,
    [string]$Tag = 'x',
    [string]$Package = 'C:\BC250\e05',
    [string]$InfName = 'bc250kmdod.inf'
)

$ErrorActionPreference = 'Continue'
$out = Join-Path $Package 'out'
New-Item -ItemType Directory -Force $out | Out-Null
$stamp = (Get-Date).ToString('HHmmss')
$hwid = 'PCI\VEN_1002&DEV_13FE'

function Get-Gpu { Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like "$hwid*" } | Select-Object -First 1 }

function Write-State([string]$file) {
    $gpu = Get-Gpu
    $lines = @("time " + (Get-Date).ToString('s'))
    if ($gpu) {
        $lines += "device   $($gpu.FriendlyName)"
        $lines += "class    $($gpu.Class)   status $($gpu.Status)   problem $($gpu.Problem)"
        foreach ($k in 'DEVPKEY_Device_Service', 'DEVPKEY_Device_DriverInfPath', 'DEVPKEY_Device_DriverVersion', 'DEVPKEY_Device_DriverProvider', 'DEVPKEY_Device_ProblemStatus') {
            $v = (Get-PnpDeviceProperty -InstanceId $gpu.InstanceId -KeyName $k -ErrorAction SilentlyContinue).Data
            $lines += ('{0,-32} {1}' -f $k, $v)
        }
    } else { $lines += 'GPU device not present' }
    $vc = Get-CimInstance Win32_VideoController | Select-Object -First 1
    $lines += "video    $($vc.Name)  $($vc.CurrentHorizontalResolution)x$($vc.CurrentVerticalResolution)  driver $($vc.DriverVersion)  status $($vc.Status)"
    $lines += 'our packages in the driver store:'
    $lines += (pnputil /enum-drivers | Select-String -Context 1, 5 'bc250') | ForEach-Object { '  ' + ($_.ToString() -replace "`r?`n", ' | ') }
    $lines | Tee-Object -FilePath $file
}

switch ($Phase) {
    'state' { Write-State (Join-Path $out "state-$Tag-$stamp.txt") }
    'sweep' {
        $log = Join-Path $out "sweep-GC-$Tag-$stamp.log"
        & C:\BC250\bc250rd\bc250rd_cli.exe sweep C:\BC250\bc250rd\reglist.txt GC. > $log 2>&1
        "sweep exit $LASTEXITCODE, $((Get-Content $log | Measure-Object -Line).Lines) lines -> $log"
    }
    'install' {
        Write-State (Join-Path $out "state-before-install-$stamp.txt") | Out-Null
        pnputil /add-driver (Join-Path $Package $InfName) /install 2>&1 | Tee-Object -FilePath (Join-Path $out "install-$stamp.txt")
        Start-Sleep -Seconds 5
        Write-State (Join-Path $out "state-after-install-$stamp.txt")
    }
    'cycle' {
        $gpu = Get-Gpu
        "disable: " + (pnputil /disable-device "$($gpu.InstanceId)" 2>&1 | Out-String).Trim() | Tee-Object -FilePath (Join-Path $out "cycle-$stamp.txt")
        Start-Sleep -Seconds 5
        Write-State (Join-Path $out "state-disabled-$stamp.txt") | Out-Null
        "enable: " + (pnputil /enable-device "$($gpu.InstanceId)" 2>&1 | Out-String).Trim() | Tee-Object -Append -FilePath (Join-Path $out "cycle-$stamp.txt")
        Start-Sleep -Seconds 5
        Write-State (Join-Path $out "state-reenabled-$stamp.txt")
    }
    'rollback' {
        $oem = (pnputil /enum-drivers | Select-String -Context 1, 0 ([regex]::Escape($InfName)) | ForEach-Object { $_.Context.PreContext[0] }) -replace '.*:\s*', ''
        foreach ($o in @($oem)) { if ($o) { pnputil /delete-driver $o.Trim() /uninstall /force 2>&1 | Tee-Object -Append -FilePath (Join-Path $out "rollback-$stamp.txt") } }
        pnputil /scan-devices | Out-Null
        Start-Sleep -Seconds 5
        Write-State (Join-Path $out "state-after-rollback-$stamp.txt")
    }
}
