# Runs once, elevated, at the first logon of the lab account (FirstLogonCommands in unattend.xml).
# Makes the machine reachable without a keyboard: Wi-Fi profile, OpenSSH server with key login, RDP.
# Every step is independent: a failure is logged and the next step still runs.

$ErrorActionPreference = 'Continue'
$root = 'C:\BC250'
Start-Transcript -Path "$root\firstlogon.log" -Append | Out-Null

function Step([string]$Name, [scriptblock]$Body) {
    Write-Host "== $Name"
    try { & $Body; Write-Host "   ok" } catch { Write-Host "   FAILED: $_" }
}

Step 'Wi-Fi profile' {
    $xml = "$root\wifi.xml"
    if (Test-Path $xml) {
        netsh wlan add profile filename="$xml" user=all
        Remove-Item $xml -Force    # holds the passphrase in clear text
    } else { Write-Host '   no wifi.xml, skipped' }
}

Step 'OpenSSH server' {
    $dst = Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'OpenSSH'
    if (-not (Test-Path "$dst\sshd.exe")) {
        Expand-Archive -Path "$root\OpenSSH-Win64.zip" -DestinationPath "$root\ssh-unpack" -Force
        New-Item -ItemType Directory -Force $dst | Out-Null
        Copy-Item "$root\ssh-unpack\OpenSSH-Win64\*" $dst -Recurse -Force
        Remove-Item "$root\ssh-unpack" -Recurse -Force
    }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$dst\install-sshd.ps1"
    New-Item -ItemType Directory -Force 'C:\ProgramData\ssh' | Out-Null
    $keys = 'C:\ProgramData\ssh\administrators_authorized_keys'
    Copy-Item "$root\authorized_keys" $keys -Force
    icacls $keys /inheritance:r /grant '*S-1-5-32-544:F' /grant '*S-1-5-18:F' | Out-Null
    New-Item -Path 'HKLM:\SOFTWARE\OpenSSH' -Force | Out-Null
    Set-ItemProperty 'HKLM:\SOFTWARE\OpenSSH' DefaultShell 'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe'
    Set-Service sshd -StartupType Automatic
    Start-Service sshd
    if (-not (Get-NetFirewallRule -Name 'bc250-sshd' -ErrorAction SilentlyContinue)) {
        New-NetFirewallRule -Name 'bc250-sshd' -DisplayName 'OpenSSH server (BC-250 lab)' -Direction Inbound `
            -Protocol TCP -LocalPort 22 -Action Allow -Profile Any | Out-Null
    }
}

Step 'Remote Desktop' {
    Set-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Terminal Server' fDenyTSConnections 0
    Enable-NetFirewallRule -Group '@FirewallAPI.dll,-28752'
}

Step 'Ping' {
    if (-not (Get-NetFirewallRule -Name 'bc250-icmp4' -ErrorAction SilentlyContinue)) {
        New-NetFirewallRule -Name 'bc250-icmp4' -DisplayName 'ICMPv4 echo (BC-250 lab)' -Direction Inbound `
            -Protocol ICMPv4 -IcmpType 8 -Action Allow -Profile Any | Out-Null
    }
}

Step 'Power: never sleep, no hibernation, no fast startup' {
    powercfg /change standby-timeout-ac 0
    powercfg /change monitor-timeout-ac 0
    powercfg /hibernate off
}

Step 'Crash handling: keep the bugcheck on screen, write a kernel dump' {
    $cc = 'HKLM:\SYSTEM\CurrentControlSet\Control\CrashControl'
    Set-ItemProperty $cc AutoReboot 0
    Set-ItemProperty $cc CrashDumpEnabled 2
}

Step 'State report' {
    # This is a diagnostic listing, not a parser: keep translated labels too.
    bcdedit /enum '{current}'
    Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.IPAddress -notlike '127.*' } |
        Select-Object InterfaceAlias, IPAddress | Format-Table -AutoSize
    Get-Service sshd | Format-Table -AutoSize
}

Stop-Transcript | Out-Null
