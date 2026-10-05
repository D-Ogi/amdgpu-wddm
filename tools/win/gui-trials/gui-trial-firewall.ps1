# Pure range helpers plus lab-only firewall functions; never invoked by parse/static validation.
function ConvertTo-GuiIpNumber([string]$Address) {
    $ip=[Net.IPAddress]::Parse($Address)
    $n=[System.Numerics.BigInteger]::Zero
    foreach($b in $ip.GetAddressBytes()){$n=$n*256+[int]$b}
    return $n
}
function ConvertFrom-GuiIpNumber([System.Numerics.BigInteger]$Number,[int]$Bits) {
    $bytes=New-Object byte[] ($Bits/8)
    for($i=$bytes.Length-1;$i -ge 0;$i--){
        $bytes[$i]=[byte]($Number % 256)
        $Number=[System.Numerics.BigInteger]::Divide($Number,256)
    }
    return ([Net.IPAddress]::new($bytes)).ToString()
}
function Get-GuiCidr([string]$Cidr) {
    if($Cidr -notmatch '^([^/%]+)/(0|[1-9][0-9]*)$'){throw 'Explicit IP CIDR required'}
    $ip=[Net.IPAddress]::Parse($Matches[1]);$prefix=[int]$Matches[2]
    $bits=$ip.GetAddressBytes().Length*8
    if($prefix -lt 1 -or $prefix -gt $bits){throw 'Invalid/broad local subnet'}
    $size=[System.Numerics.BigInteger]::Pow(2,$bits-$prefix)
    $n=ConvertTo-GuiIpNumber $ip.ToString()
    $first=[System.Numerics.BigInteger]::Divide($n,$size)*$size
    return [pscustomobject]@{bits=$bits;first=$first;last=($first+$size-1);cidr=((ConvertFrom-GuiIpNumber $first $bits)+'/'+$prefix)}
}
function Get-GuiOfflineRanges($C) {
    Add-Type -AssemblyName System.Numerics
    $local=@()
    foreach($a in @(Get-NetIPAddress -AddressState Preferred -ErrorAction Stop)){
        if($a.PrefixLength -gt 0){$local+=Get-GuiCidr ($a.IPAddress+'/'+$a.PrefixLength)}
    }
    $allowed=@(Get-GuiCidr '127.0.0.0/8';Get-GuiCidr '::1/128')
    if(@($C.offline.local_subnets).Count -eq 0){throw 'No local subnets supplied'}
    foreach($text in @($C.offline.local_subnets)) {
        $r=Get-GuiCidr $text
        if(-not @($local|Where-Object {$_.bits -eq $r.bits -and $_.first -eq $r.first -and $_.last -eq $r.last}).Count){throw 'Allowed CIDR is not a current interface subnet'}
        $allowed+=$r
    }
    $management=[Net.IPAddress]::Parse($C.offline.management_address)
    $m=ConvertTo-GuiIpNumber $management.ToString()
    if(-not @($allowed|Where-Object {$_.bits -eq $management.GetAddressBytes().Length*8 -and $m -ge $_.first -and $m -le $_.last}).Count){throw 'Management address is outside allowed subnets'}
    $probe=[Net.IPAddress]::Parse($C.offline.external_probe_address)
    $p=ConvertTo-GuiIpNumber $probe.ToString()
    if(@($allowed|Where-Object {$_.bits -eq $probe.GetAddressBytes().Length*8 -and $p -ge $_.first -and $p -le $_.last}).Count){throw 'External positive-control probe is local'}
    if($C.offline.external_probe_port -lt 1 -or $C.offline.external_probe_port -gt 65535){throw 'Invalid probe port'}
    $blocked=@()
    foreach($bits in @(32,128)) {
        $cursor=[System.Numerics.BigInteger]::Zero
        $max=[System.Numerics.BigInteger]::Pow(2,$bits)-1
        foreach($r in @($allowed|Where-Object bits -eq $bits|Sort-Object first,last)) {
            if($r.first -gt $cursor){$blocked+=((ConvertFrom-GuiIpNumber $cursor $bits)+'-'+(ConvertFrom-GuiIpNumber ($r.first-1) $bits))}
            if($r.last -ge $cursor){$cursor=$r.last+1}
        }
        if($cursor -le $max){$blocked+=((ConvertFrom-GuiIpNumber $cursor $bits)+'-'+(ConvertFrom-GuiIpNumber $max $bits))}
    }
    return @{allowed=@($allowed|ForEach-Object cidr|Select-Object -Unique);blocked=$blocked}
}
function Test-GuiExternalConnection($C) {
    $client=[Net.Sockets.TcpClient]::new([Net.IPAddress]::Parse($C.offline.external_probe_address).AddressFamily)
    try {
        $task=$client.ConnectAsync([Net.IPAddress]::Parse($C.offline.external_probe_address),[int]$C.offline.external_probe_port)
        if(-not $task.Wait(1500)){return $false}
        return $client.Connected
    } catch {return $false}
    finally {$client.Dispose()}
}
function Get-GuiFirewallNames($C) {
    return @(('BC250-GUI-'+$C.trial_id+'-offline-block'),('BC250-GUI-'+$C.trial_id+'-offline-local'))
}
function Enter-GuiFirewallLock($C,[long]$Deadline=[long]::MaxValue) {
    $until=[Math]::Min($Deadline,(Get-GuiQpc)+3*[Diagnostics.Stopwatch]::Frequency)
    do {
        try {return [IO.File]::Open(($C.directory+'\offline.lock'),[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)}
        catch [IO.IOException] {Start-Sleep -Milliseconds 50}
    } while((Get-GuiQpc) -lt $until)
    throw 'Firewall lock timed out; cleanup not established'
}
function Assert-GuiOfflineLeaseOpen($C,$Boundary) {
    if((Test-Path -LiteralPath ($C.directory+'\offline.closed')) -or
       (Test-Path -LiteralPath ($C.directory+'\evidence\STOP')) -or
       (Test-Path -LiteralPath ($C.directory+'\evidence\CANCEL')) -or
       (Get-GuiQpc) -ge $Boundary.work_deadline_qpc){throw 'Offline lease closed or expired'}
}
function Enable-GuiOffline($C,$Boundary) {
    Assert-GuiSystem
    $profiles=@(Get-NetFirewallProfile -PolicyStore ActiveStore)
    if($profiles.Count -ne 3 -or @($profiles|Where-Object {$_.Enabled -ne $true -or [string]$_.AllowLocalFirewallRules -eq 'False'}).Count){throw 'Enabled profiles and local rule merge required'}
    if((Get-Service MpsSvc).Status -ne 'Running'){throw 'Firewall service not running'}
    $ranges=Get-GuiOfflineRanges $C
    $names=@(Get-GuiFirewallNames $C)
    if($names.Count -ne 2 -or $names[0] -ceq $names[1]){throw 'Invalid exact rule names'}
    $before=Test-GuiExternalConnection $C
    Write-GuiJson ($C.directory+'\offline-before.json') @{reachable=$before;profiles=@($profiles|Select-Object Name,Enabled,DefaultOutboundAction,AllowLocalFirewallRules);ranges=$ranges}
    if(-not $before){throw 'External positive control unavailable; cannot claim firewall caused isolation'}
    $lock=Enter-GuiFirewallLock $C
    try {
        # Shared serialization and a permanent closed marker prevent create-after-cleanup.
        if((Test-Path -LiteralPath ($C.directory+'\offline.closed')) -or
           (Test-Path -LiteralPath ($C.directory+'\evidence\STOP')) -or
           (Test-Path -LiteralPath ($C.directory+'\evidence\CANCEL')) -or
           (Get-GuiQpc) -ge $Boundary.work_deadline_qpc){throw 'Offline lease closed or expired'}
        foreach($name in $names){if(Get-NetFirewallRule -Name $name -ErrorAction SilentlyContinue){throw 'Pre-existing trial rule collision; nothing is owned'}}
        $nonce=[Guid]::NewGuid().ToString('N')
        $group='BC250-GUI-'+$C.trial_id+'-'+$nonce
        # Persist intent after proving names absent, before the first write, for interrupted creation.
        Write-GuiJson ($C.directory+'\offline-owned.json') @{schema=1;trial_id=$C.trial_id;names=$names;group=$group;description=('T2 offline lease '+$nonce)}
        $owner=Read-GuiJson ($C.directory+'\offline-owned.json')
        Assert-GuiOfflineLeaseOpen $C $Boundary
        New-NetFirewallRule -Name $names[1] -DisplayName $names[1] -Group $group -Description $owner.description -PolicyStore PersistentStore -Direction Outbound -Action Allow -Profile Any -RemoteAddress $ranges.allowed -Protocol Any -Enabled True | Out-Null
        Assert-GuiOfflineLeaseOpen $C $Boundary
        New-NetFirewallRule -Name $names[0] -DisplayName $names[0] -Group $group -Description $owner.description -PolicyStore PersistentStore -Direction Outbound -Action Block -Profile Any -RemoteAddress $ranges.blocked -Protocol Any -Enabled True | Out-Null
        $active=Get-NetFirewallRule -PolicyStore ActiveStore -Name $names[0] -ErrorAction Stop
        if($active.Enabled -ne $true -or [string]$active.Action -ne 'Block' -or [string]$active.Direction -ne 'Outbound' -or $active.Group -cne $group){throw 'Owned block rule absent from effective policy'}
        $filters=@($active|Get-NetFirewallAddressFilter)
        $blocked=-not (Test-GuiExternalConnection $C)
        Write-GuiJson ($C.directory+'\offline-isolated.json') @{blocked=$blocked;qpc=(Get-GuiQpc);active_rule=$active.Name;remote_addresses=@($filters.RemoteAddress)}
        if(-not $blocked){throw 'External TCP still reachable under block; abort repair'}
        Assert-GuiOfflineLeaseOpen $C $Boundary
    } finally {$lock.Dispose()}
}
function Disable-GuiOffline($C,[long]$Deadline=[long]::MaxValue) {
    if(-not ([Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'Elevated cleanup required'}
    $lock=Enter-GuiFirewallLock $C $Deadline
    try {
        [IO.File]::WriteAllText(($C.directory+'\offline.closed'),'closed',[Text.UTF8Encoding]::new($false))
        $ownedPath=$C.directory+'\offline-owned.json'
        if(-not (Test-Path -LiteralPath $ownedPath)){return $true} # Admission refusal never owns colliding rules.
        $owner=Read-GuiJson $ownedPath
        $names=@(Get-GuiFirewallNames $C)
        if($owner.trial_id -cne $C.trial_id -or @($owner.names).Count -ne 2 -or
           $owner.names[0] -cne $names[0] -or $owner.names[1] -cne $names[1]){throw 'Ownership record mismatch'}
        foreach($name in $names) {
            $r=Get-NetFirewallRule -PolicyStore PersistentStore -Name $name -ErrorAction SilentlyContinue
            if($r) {
                if($r.Group -cne $owner.group -or $r.Description -cne $owner.description){throw 'Foreign rule identity; do not remove it'}
                $r|Remove-NetFirewallRule -ErrorAction Stop
            }
        }
        $left=@()
        foreach($name in $names){$left+=@(Get-NetFirewallRule -PolicyStore ActiveStore -Name $name -ErrorAction SilentlyContinue)}
        $restored=($left.Count -eq 0)
        Write-GuiJson ($C.directory+'\offline-restored-'+$PID+'.json') @{schema=1;trial_id=$C.trial_id;rules_absent=$restored;utc=[DateTime]::UtcNow.ToString('o');qpc=(Get-GuiQpc)}
        return $restored
    } finally {$lock.Dispose()}
}
function Invoke-GuiFirewallGuard($C) {
    Assert-GuiSystem
    if($C.trial -ne 'L-OFF'){throw 'Firewall watchdog belongs only to L-OFF'}
    $cleanupUntil=(Get-GuiQpc)+15*[Diagnostics.Stopwatch]::Frequency
    try {
        if(-not (Test-Path -LiteralPath ($C.directory+'\running.json'))){return}
        $b=Read-GuiJson ($C.directory+'\running.json')
        $boot=Get-GuiBoot
        Write-GuiJson ($C.directory+'\guard-ready.json') @{trial_id=$C.trial_id;boot_id=$boot;qpc=(Get-GuiQpc);pid=$PID}
        if($b.boot_id -cne $boot){return} # AtStartup restores after interruption/reboot.
        $cleanupUntil=$b.deadline_qpc-$b.frequency
        $limit=$b.qpc+($C.duration_seconds-8)*$b.frequency
        while((Get-GuiQpc) -lt $limit) {
            if(Test-Path -LiteralPath ($C.directory+'\result.json')){return}
            $h=Read-GuiJson ($C.directory+'\heartbeat.json')
            if((Get-GuiQpc)-$h.qpc -gt 10*$b.frequency) {
                Set-GuiStop $C 'supervisor-heartbeat-lost'
                Set-GuiStop $C 'supervisor-heartbeat-lost' -Hard
                return
            }
            Start-Sleep -Milliseconds 200
        }
        Set-GuiStop $C 'offline-watchdog-deadline'
        Set-GuiStop $C 'offline-watchdog-deadline' -Hard
    } finally {
        $restored=$false;$failure='cleanup budget exhausted'
        # Ordinary lock contention must not abandon the independent cleaner.
        while(-not $restored -and (Get-GuiQpc) -lt $cleanupUntil) {
            try {$restored=Disable-GuiOffline $C $cleanupUntil}
            catch {
                $failure=$_.Exception.Message
                if($failure -notlike 'Firewall lock timed out*'){break}
            }
            if(-not $restored){Start-Sleep -Milliseconds 50}
        }
        if(-not $restored) {
            Write-GuiJson ($C.directory+'\guard-unverified.json') @{trial_id=$C.trial_id;rules_absent=$false;reason=$failure;qpc=(Get-GuiQpc)}
            throw 'Independent firewall restoration unverified; recovery required'
        }
        Write-GuiJson ($C.directory+'\guard-done.json') @{trial_id=$C.trial_id;rules_absent=$true;qpc=(Get-GuiQpc)}
    }
}
