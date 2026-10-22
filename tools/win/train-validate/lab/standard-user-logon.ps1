# BD-080 on hardware: a logon of an account that is not an administrator confirms the driver start
# (Windows PowerShell 5.1, SSH session, session 0).
#
#   standard-user-logon.ps1 -Phase prepare|verify|cleanup [-Account bc250std] [-Logons 2]
#
# The defect. Up to 0.7.216.100-tester.29 the installer registered one logon task for the Administrators group
# alone, so on a computer whose everyday account is a standard account nothing confirmed a start. The boot-loop
# guard counted every start and the third start of the machine left Windows on Microsoft Basic Display Adapter,
# with no way back for that user. No existing arm can see this: every suite call over ssh is elevated and every
# installer arm logs on as the lab's administrator account.
#
# Phases. The arm runs prepare, then a restart, then verify, then cleanup and a restart.
#   prepare  Creates the local account, not in Administrators, with a random password that this script never
#            prints, and arms Windows to log that account on by itself for -Logons starts (AutoAdminLogon with
#            AutoLogonCount, which Windows itself counts down and clears). It records the registry values it
#            overwrites so cleanup can put them back.
#   verify   Reads what the two logons left behind: the confirmation lines of start-confirm.log for the standard
#            account, the window task's own line with its last result, UnconfirmedStarts back at 0, the clock
#            control still on, both tasks registered, and the display adapter still ours rather than Microsoft
#            Basic Display Adapter.
#   cleanup  Clears the automatic logon, deletes the account and its profile, and puts the recorded values back.
#
# The password. It is generated on the lab, written only into the registry value that Windows reads for an
# automatic logon, and never printed by this script. Windows keeps that value in plain text while the automatic
# logon is armed, which is why -Logons is small and why cleanup removes the value and the account. Run cleanup
# in the same validation round, and never leave a lab with this armed.
param([Parameter(Mandatory)][ValidateSet('prepare', 'verify', 'cleanup')][string]$Phase,
      [string]$Account = 'bc250std', [int]$Logons = 2, [int]$WindowSeconds = 120)
$ErrorActionPreference = 'Continue'
$bad = @()
$work = 'C:\BC250\tmp\standard-user-logon'
$saved = Join-Path $work 'winlogon-before.json'
$winlogon = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon'
$confirmLog = Join-Path $env:ProgramData 'amdgpu-wddm\start-confirm.log'

function Get-WinlogonValue([string]$Name) {
    $v = Get-ItemProperty -Path $winlogon -Name $Name -ErrorAction SilentlyContinue
    if ($null -eq $v) { return $null }
    return [string]$v.$Name
}

try {
    "utc $([DateTime]::UtcNow.ToString('o')) phase $Phase account $Account"
    if ($Phase -ne 'cleanup') {
        $inst = [string](Get-ItemProperty 'HKLM:\SOFTWARE\amdgpu-wddm\Release' -Name InstallRoot -ErrorAction Stop).InstallRoot
        $cli = Join-Path $inst 'tools\bc250kmd_cli.exe'
        . (Join-Path $inst 'tools\state-reader.ps1')
        $state = Read-DriverStateValues -Cli $cli
        if (-not $state.ContainsKey('UnconfirmedStarts')) { throw 'UnconfirmedStarts is absent' }
    }
    [void][IO.Directory]::CreateDirectory($work)
    $boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime
    "boot $($boot.ToUniversalTime().ToString('o'))"

    if ($Phase -eq 'prepare') {
        if (-not (Test-Path -LiteralPath $saved)) {
            @{ AutoAdminLogon = Get-WinlogonValue 'AutoAdminLogon'
               DefaultUserName = Get-WinlogonValue 'DefaultUserName'
               DefaultDomainName = Get-WinlogonValue 'DefaultDomainName'
               HadDefaultPassword = [bool](Get-WinlogonValue 'DefaultPassword')
               AutoLogonCount = Get-WinlogonValue 'AutoLogonCount' } |
                ConvertTo-Json | Set-Content -LiteralPath $saved -Encoding UTF8
            "winlogon record saved to $saved"
        }
        # 24 characters from the printable ASCII range, from the system's own generator.
        $bytes = New-Object byte[] 24
        [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
        $alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_'
        $password = -join ($bytes | ForEach-Object { $alphabet[$_ % $alphabet.Length] })
        $secure = ConvertTo-SecureString $password -AsPlainText -Force
        $existing = Get-LocalUser -Name $Account -ErrorAction SilentlyContinue
        if ($existing) {
            Set-LocalUser -Name $Account -Password $secure -PasswordNeverExpires $true
            "account $Account existed: its password was set again"
        } else {
            New-LocalUser -Name $Account -Password $secure -PasswordNeverExpires:$true `
                -FullName 'BC-250 standard user probe' `
                -Description 'BD-080 train arm: a logon that is not an administrator. Removed by the cleanup phase.' | Out-Null
            "account $Account created"
        }
        Add-LocalGroupMember -Group 'Users' -Member $Account -ErrorAction SilentlyContinue
        $admins = @(Get-LocalGroupMember -Group 'Administrators' -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })
        if ($admins -match [regex]::Escape($Account)) {
            Remove-LocalGroupMember -Group 'Administrators' -Member $Account -ErrorAction SilentlyContinue
        }
        $admins = @(Get-LocalGroupMember -Group 'Administrators' -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })
        "in Administrators: $([bool]($admins -match [regex]::Escape($Account)))"
        if ($admins -match [regex]::Escape($Account)) { $bad += "$Account is still in Administrators: the arm would prove nothing" }
        Set-ItemProperty -Path $winlogon -Name 'AutoAdminLogon' -Value '1'
        Set-ItemProperty -Path $winlogon -Name 'DefaultUserName' -Value $Account
        Set-ItemProperty -Path $winlogon -Name 'DefaultDomainName' -Value $env:COMPUTERNAME
        Set-ItemProperty -Path $winlogon -Name 'DefaultPassword' -Value $password
        Set-ItemProperty -Path $winlogon -Name 'AutoLogonCount' -Value $Logons -Type DWord
        $password = $null
        "automatic logon armed for $Logons start(s); the password is not printed and cleanup removes it"
        # The mark the verify phase reads the log from: everything after this line belongs to the probe.
        $mark = Join-Path $work 'mark.json'
        $logLines = 0
        if (Test-Path -LiteralPath $confirmLog) { $logLines = @(Get-Content -LiteralPath $confirmLog).Count }
        $unconfirmed = [uint32]$state['UnconfirmedStarts']
        @{ utc = [DateTime]::UtcNow.ToString('o'); log_lines = $logLines; unconfirmed = $unconfirmed } |
            ConvertTo-Json | Set-Content -LiteralPath $mark -Encoding UTF8
        "mark written to $mark"
    }
    elseif ($Phase -eq 'verify') {
        $mark = Join-Path $work 'mark.json'
        $from = 0
        if (Test-Path -LiteralPath $mark) {
            $m = Get-Content -LiteralPath $mark -Raw | ConvertFrom-Json
            $from = [int]$m.log_lines
            "mark: $($m.utc), log line $from, UnconfirmedStarts was $($m.unconfirmed)"
        } else { $bad += "no mark at ${mark}: run the prepare phase first" }

        # 1. the two tasks, as the installer registers them
        foreach ($name in @('amdgpu-wddm start confirm', 'amdgpu-wddm start window')) {
            $t = Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue
            if (-not $t) { $bad += "the task '$name' is not registered"; continue }
            $i = Get-ScheduledTaskInfo -TaskName $name -ErrorAction SilentlyContinue
            "task '$name': state $($t.State) principal $($t.Principal.UserId)/$($t.Principal.GroupId) runlevel $($t.Principal.RunLevel) last result $($i.LastTaskResult) last run $($i.LastRunTime)"
            if ($name -eq 'amdgpu-wddm start window' -and $i.LastTaskResult -ne 0) {
                $bad += "the window task's last result is $($i.LastTaskResult), not 0 (2 means the window could not be shown)"
            }
        }

        # 2. the confirmation log of this round
        if (Test-Path -LiteralPath $confirmLog) {
            $lines = @(Get-Content -LiteralPath $confirmLog)
            $new = @($lines | Select-Object -Skip $from)
            "start-confirm.log: $($lines.Count) lines, $($new.Count) since the mark"
            foreach ($l in $new) { '  log: ' + $l }
            if ($new.Count -eq 0) { $bad += 'start-confirm.log gained no line: nothing confirmed the start of this logon' }
            if (-not ($new -match 'confirm')) { $bad += 'no confirmation line in the log since the mark' }
            if (-not ($new -match 'window task')) { $bad += 'no window task line in the log since the mark: the confirmation did not report what the window half did' }
        } else { $bad += "no $confirmLog" }

        # 3. the driver's own counters
        $p = Read-DriverStateValues -Cli $cli
        if (-not $p.ContainsKey('UnconfirmedStarts')) { throw 'UnconfirmedStarts is absent' }
        "state: UnconfirmedStarts=$($p.UnconfirmedStarts) DpmMode=$($p.DpmMode) DpmMaxMHz=$($p.DpmMaxMHz) DpmClosedReason=$($p.DpmClosedReason)"
        if ([int]$p.UnconfirmedStarts -ne 0) { $bad += "UnconfirmedStarts is $($p.UnconfirmedStarts), not 0: the logon of the standard account confirmed nothing" }
        if ($null -ne $p.DpmMode -and [int]$p.DpmMode -eq 0) { $bad += 'DpmMode is 0: the automatic clock control was set aside' }

        # 4. the adapter is still ours
        $adapters = @(Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue |
            ForEach-Object { '{0} [{1}] driver {2}' -f $_.Name, $_.Status, $_.DriverVersion })
        foreach ($a in $adapters) { "adapter: $a" }
        if ($adapters -match 'Basic Display') { $bad += 'Windows is on Microsoft Basic Display Adapter: this is the defect, not the fix' }
        $health = ''
        $inst = [string](Get-ItemProperty 'HKLM:\SOFTWARE\amdgpu-wddm\Release' -Name InstallRoot -ErrorAction SilentlyContinue).InstallRoot
        if ($inst) {
            $cli = Join-Path $inst 'tools\bc250kmd_cli.exe'
            if (Test-Path -LiteralPath $cli) { $health = (& $cli health read 2>&1) -join ' ' }
        }
        "health: $health"
        # 15, or 31 with QUIET (16, KMD 0.7.216.65) beside the four start bits.
        if ($health -and $health -notmatch 'flags=(15|31) ') { $bad += "start health is not flags=15 (31 with QUIET): $health" }

        # 5. the logons that actually happened, for the record
        $count = Get-WinlogonValue 'AutoLogonCount'
        "AutoLogonCount left: $count (Windows counts it down at every automatic logon)"
        $profile = Join-Path $env:SystemDrive "Users\$Account"
        "profile present: $([bool](Test-Path -LiteralPath $profile))"
    }
    else {
        Set-ItemProperty -Path $winlogon -Name 'AutoLogonCount' -Value 0 -Type DWord -ErrorAction SilentlyContinue
        Remove-ItemProperty -Path $winlogon -Name 'DefaultPassword' -ErrorAction SilentlyContinue
        Remove-ItemProperty -Path $winlogon -Name 'AutoLogonCount' -ErrorAction SilentlyContinue
        if (Test-Path -LiteralPath $saved) {
            $old = Get-Content -LiteralPath $saved -Raw | ConvertFrom-Json
            foreach ($n in @('AutoAdminLogon', 'DefaultUserName', 'DefaultDomainName')) {
                $v = [string]$old.$n
                if ([string]::IsNullOrEmpty($v)) { Remove-ItemProperty -Path $winlogon -Name $n -ErrorAction SilentlyContinue }
                else { Set-ItemProperty -Path $winlogon -Name $n -Value $v }
            }
            'winlogon values restored from the saved record'
            if ($old.HadDefaultPassword) { $bad += 'the computer had a DefaultPassword before this arm: it is not restored, set it again by hand' }
        } else { $bad += "no saved record at ${saved}: check the Winlogon values by hand" }
        "AutoAdminLogon now: $(Get-WinlogonValue 'AutoAdminLogon'), DefaultUserName: $(Get-WinlogonValue 'DefaultUserName'), DefaultPassword present: $([bool](Get-WinlogonValue 'DefaultPassword'))"
        if (Get-WinlogonValue 'DefaultPassword') { $bad += 'DefaultPassword is still in the registry: remove it' }
        $u = Get-LocalUser -Name $Account -ErrorAction SilentlyContinue
        if ($u) {
            $sid = $u.SID.Value
            Remove-LocalUser -Name $Account -ErrorAction SilentlyContinue
            "account $Account removed"
            $key = "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList\$sid"
            $pp = (Get-ItemProperty -Path $key -Name ProfileImagePath -ErrorAction SilentlyContinue).ProfileImagePath
            Remove-Item -Path $key -Recurse -Force -ErrorAction SilentlyContinue
            if ($pp -and (Test-Path -LiteralPath $pp) -and $pp -like "$env:SystemDrive\Users\$Account*") {
                Remove-Item -LiteralPath $pp -Recurse -Force -ErrorAction SilentlyContinue
                "profile $pp removed"
            }
        } else { "account $Account is not present" }
        if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue }
    }
}
catch { $bad += "probe error: $($_.Exception.Message)" }

if ($bad.Count) {
    foreach ($b in $bad) { "problem: $b" }
    "standard user logon arm ($Phase): FAIL, $($bad.Count) problem(s)"
    exit 1
}
"standard user logon arm ($Phase): ok"
exit 0
