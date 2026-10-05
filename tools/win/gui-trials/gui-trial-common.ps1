# T2 infrastructure only. Dot-sourcing defines functions; it does not run a trial.
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
function Get-GuiField($Object,[string]$Name,$Default=$null) {
    if ($null -ne $Object -and $null -ne $Object.PSObject.Properties[$Name]) { return $Object.$Name }
    return $Default
}
function Write-GuiJson([string]$Path,$Value) {
    $tmp=$Path+'.'+$PID+'.tmp'
    [IO.File]::WriteAllText($tmp,($Value|ConvertTo-Json -Depth 20 -Compress),[Text.UTF8Encoding]::new($false))
    Move-Item -LiteralPath $tmp -Destination $Path -Force
}
function Read-GuiJson([string]$Path) {
    if ((Get-Item -LiteralPath $Path).Length -gt 1048576) { throw 'JSON exceeds 1 MiB' }
    return ([IO.File]::ReadAllText($Path)|ConvertFrom-Json)
}
function Get-GuiQpc { return [Diagnostics.Stopwatch]::GetTimestamp() }
function Get-GuiBoot {
    return (Get-CimInstance Win32_OperatingSystem -OperationTimeoutSec 3).LastBootUpTime.ToUniversalTime().ToString('o')
}
function Assert-GuiSystem {
    if ([Security.Principal.WindowsIdentity]::GetCurrent().User.Value -ne 'S-1-5-18') { throw 'SYSTEM task required' }
}
function Assert-GuiArtifact($Artifact) {
    if ($null -eq $Artifact -or $Artifact.sha256 -notmatch '^[0-9a-fA-F]{64}$') { throw 'Artifact identity missing' }
    $p=[IO.Path]::GetFullPath([string]$Artifact.path)
    if (-not [IO.Path]::IsPathRooted($Artifact.path) -or $p.StartsWith('\\')) { throw 'Local absolute artifact path required' }
    if ((Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash -ine $Artifact.sha256) { throw "Artifact SHA mismatch: $p" }
}
function Get-GuiStages([string]$Trial) {
    switch ($Trial) {
        'L1' { return @('view') }
        'L3' { return @('startup-and-click') }
        'L4' { return @('without-prepared','with-prepared') }
        'L5' { return @('upgrade','resume','repair','verify') }
        'L-CU' { return @('baseline-24','request-40','observe-40','confirm-40','request-24','observe-24') }
        'L-OFF' { return @('repair') }
        default { throw 'Unknown trial' }
    }
}
function Test-GuiMutating($C) {
    return ($C.trial -in @('L5','L-OFF') -or ($C.trial -eq 'L-CU' -and $C.stage -in @('request-40','confirm-40','request-24')))
}
function Assert-GuiConfig($C,[string]$Trial,[string]$ConfigPath) {
    if ($C.schema -ne 1 -or $C.trial -cne $Trial -or $C.trial_id -notmatch '^[A-Za-z0-9_-]{1,64}$') { throw 'Invalid config identity' }
    $expected='C:\BC250\tmp\gui\'+$C.trial_id
    if ([IO.Path]::GetFullPath($C.directory).TrimEnd('\') -ine $expected) { throw 'Unexpected trial directory' }
    if ([IO.Path]::GetFullPath($ConfigPath) -ine ($expected+'\config.json')) { throw 'Use directory\config.json' }
    if ($C.stage -notin (Get-GuiStages $Trial)) { throw 'Invalid stage' }
    if ($C.duration_seconds -isnot [int] -or $C.duration_seconds -lt 45 -or $C.duration_seconds -gt 180) { throw 'Duration must be 45..180 seconds' }
    if ($C.expected_session_id -le 0 -or $C.expected_user_sid -notmatch '^S-1-5-21-(\d+-){3}\d+$') { throw 'Expected interactive session/SID required' }
    if (-not $C.target_computer_name -or $env:COMPUTERNAME -ine $C.target_computer_name) { throw 'Target computer mismatch' }
    if ($Trial -eq 'L-CU' -and $C.thermal.mode -cne 'local-cli') { throw 'L-CU requires local-cli thermal input' }
}
function Assert-GuiInputs($C) {
    Assert-GuiArtifact $C.scenario_adapter
    Assert-GuiArtifact $C.bounded_helper
    foreach ($a in @($C.artifacts)) { Assert-GuiArtifact $a }
    if ($C.trial -eq 'L-CU') { Assert-GuiArtifact ([pscustomobject]@{path=$C.thermal.cli_path;sha256=$C.thermal.cli_sha256}) }
    if (Test-GuiMutating $C) {
        $admission=Get-GuiField $C 'mutation_admission'
        Assert-GuiArtifact $admission
        $a=Read-GuiJson $admission.path
        if ($a.schema -ne 1 -or $a.adapter_sha256 -ine $C.scenario_adapter.sha256 -or
            -not $a.review_reference -or $a.safe_cancel_verified -ne $true -or
            $a.elevated_children_bounded -ne $true -or $a.durable_boundary_verified -ne $true) {
            throw 'Mutating stage blocked: reviewed safe-cancel/closure contract missing'
        }
    }
}
function Assert-GuiMutationClosure($C,$Result,[string]$Boot) {
    if($Result.schema -ne 1 -or $Result.trial_id -cne $C.trial_id -or $Result.stage -cne $C.stage -or
        $Result.boot_id -cne $Boot -or $Result.outcome -notin @('complete','pending-restart','cancelled','failed') -or
        $Result.elevated_children_closed -isnot [bool] -or -not $Result.elevated_children_closed -or
        $Result.tree_closed -isnot [bool] -or -not $Result.tree_closed) {throw 'Invalid mutation closure record'}
    Assert-GuiArtifact $Result.closure_evidence
    Assert-GuiArtifact $Result.durable_boundary_evidence
    foreach($file in @($Result.closure_evidence,$Result.durable_boundary_evidence)) {
        $e=Read-GuiJson $file.path
        if($e.schema -ne 1 -or $e.trial_id -cne $C.trial_id -or $e.stage -cne $C.stage -or $e.boot_id -cne $Boot) {
            throw 'Closure evidence belongs to another invocation'
        }
    }
    $closed=Read-GuiJson $Result.closure_evidence.path
    $durable=Read-GuiJson $Result.durable_boundary_evidence.path
    if($closed.elevated_children_closed -isnot [bool] -or -not $closed.elevated_children_closed -or
        $durable.safe_boundary -isnot [bool] -or -not $durable.safe_boundary) {throw 'Engine closure not established'}
}
function ConvertTo-GuiArgument([AllowEmptyString()][string]$Value) {
    $b=New-Object Text.StringBuilder; [void]$b.Append('"'); $slashes=0
    foreach($c in $Value.ToCharArray()) {
        if($c -eq '\') { $slashes++; continue }
        $n=if($c -eq '"'){$slashes*2}else{$slashes}
        if($n){[void]$b.Append(('\'*$n))}; $slashes=0
        if($c -eq '"'){[void]$b.Append('\')}; [void]$b.Append($c)
    }
    if($slashes){[void]$b.Append(('\'*($slashes*2)))}
    [void]$b.Append('"'); return $b.ToString()
}
function New-GuiEvent($C,[string]$Event,$Data) {
    return [ordered]@{schema=1;protocol='bc250.gui-trial.v1';event=$Event;trial_id=$C.trial_id;trial=$C.trial;stage=$C.stage;data=$Data}
}
function Add-GuiEvent($C,[string]$Event,$Data) {
    $script:GuiSequence++
    $v=New-GuiEvent $C $Event $Data
    $v.sequence=$script:GuiSequence; $v.qpc=Get-GuiQpc; $v.utc=[DateTime]::UtcNow.ToString('o')
    [IO.File]::AppendAllText(($C.directory+'\events.jsonl'),(($v|ConvertTo-Json -Depth 12 -Compress)+[char]10),[Text.UTF8Encoding]::new($false))
}
function Set-GuiStop($C,[string]$Reason,[switch]$Hard) {
    $name=if($Hard){'CANCEL'}else{'STOP'}
    [IO.File]::WriteAllText(($C.directory+'\evidence\'+$name),$Reason,[Text.UTF8Encoding]::new($false))
}
function Get-GuiStop($C) {
    if (Test-Path -LiteralPath ($C.directory+'\evidence\STOP')) { return 'stop-file' }
    try {
        $r=Invoke-RestMethod -Uri 'http://127.0.0.1:2250/flags' -TimeoutSec 1
        if ($null -eq $r.PSObject.Properties['stop']) { return 'overlay-stop-unavailable' }
        if ($r.stop) { return 'owner-stop' }
    } catch { return 'overlay-stop-unavailable' }
    return ''
}
function Start-GuiBounded($C,[string]$Stem,[string]$Executable,[string[]]$Arguments,[long]$Deadline,[switch]$Interactive) {
    $out=$C.directory+'\evidence\'+$Stem
    $v=@('--cancel-file',($C.directory+'\evidence\CANCEL'),[string]$Deadline,($out+'.stdout'),($out+'.stderr'),$Executable)+$Arguments
    if($Interactive){$v=@('--active-console')+$v}
    $si=New-Object Diagnostics.ProcessStartInfo
    $si.FileName=$C.bounded_helper.path; $si.Arguments=($v|ForEach-Object {ConvertTo-GuiArgument $_}) -join ' '
    $si.UseShellExecute=$false; $si.CreateNoWindow=$true; $si.RedirectStandardOutput=$true; $si.RedirectStandardError=$true
    $p=New-Object Diagnostics.Process; $p.StartInfo=$si
    if(-not $p.Start()){throw 'Bounded helper start failed'}
    $handle=$p.Handle # Retain identity/handle, never use a process name to kill.
    return @{process=$p;handle=$handle;output=$p.StandardOutput.ReadToEndAsync();error=$p.StandardError.ReadToEndAsync();stem=$Stem}
}
function Complete-GuiBounded($C,$Child,[long]$Deadline) {
    if(-not $Child.process.HasExited){throw 'Helper still running'}
    $remaining=[int][Math]::Max(0,1000*($Deadline-(Get-GuiQpc))/[Diagnostics.Stopwatch]::Frequency)
    if(-not [Threading.Tasks.Task]::WaitAll([Threading.Tasks.Task[]]@($Child.output,$Child.error),$remaining)){throw 'Helper receipt pipes did not close'}
    $r=$Child.output.Result|ConvertFrom-Json
    Write-GuiJson ($C.directory+'\evidence\'+$Child.stem+'.receipt.json') @{exit_code=$Child.process.ExitCode;receipt=$r;stderr=$Child.error.Result}
    if($r.job_empty -ne $true){throw 'Process tree closure unconfirmed'}
    return @{exit_code=$Child.process.ExitCode;receipt=$r}
}
function ConvertFrom-GuiTemperatureText([string]$Text) {
    $m=[regex]::Matches($Text,'(?m)^dpm .* temperature_c=(-?\d+(?:\.\d+)?)(\S*) ')
    if($m.Count -ne 1 -or $m[0].Groups[2].Value -ne ''){throw 'Missing/stale thermal reading'}
    $value=[double]::Parse($m[0].Groups[1].Value,[Globalization.CultureInfo]::InvariantCulture)
    if($value -lt -20 -or $value -gt 130){throw 'Implausible thermal reading'}
    return $value
}
function Read-GuiTemperature($C,$Boundary,[int]$Sequence) {
    $deadline=[Math]::Min([long]$Boundary.work_deadline_qpc,(Get-GuiQpc)+3*[Diagnostics.Stopwatch]::Frequency)
    $stem='thermal-'+$Sequence
    $p=Start-GuiBounded $C $stem $C.thermal.cli_path @('telemetry','1') $deadline
    try {
        while(-not $p.process.HasExited -and (Get-GuiQpc) -lt $deadline){Start-Sleep -Milliseconds 50}
        try {$r=Complete-GuiBounded $C $p $deadline} catch {$script:GuiAuxClosure=$false;throw}
        if($r.exit_code -ne 0 -or $r.receipt.child_exit -ne 0){throw 'Thermal read failed'}
        $text=[IO.File]::ReadAllText(($C.directory+'\evidence\'+$stem+'.stdout'))
        $value=ConvertFrom-GuiTemperatureText $text
        Add-GuiEvent $C 'thermal' @{temperature_c=$value;source='local-cli telemetry 1';sample_sequence=$Sequence}
        return $value
    } finally {
        if(-not $p.process.HasExited){$script:GuiAuxClosure=$false;$p.process.Kill()}
        $p.process.Dispose()
    }
}

function Set-GuiAcl($C) {
    # The adapter can write evidence, never the SYSTEM task scripts/config/state.
    $acl=New-Object Security.AccessControl.DirectorySecurity
    $acl.SetAccessRuleProtection($true,$false)
    $inherit=[Security.AccessControl.InheritanceFlags]'ContainerInherit,ObjectInherit'
    foreach($sid in @('S-1-5-18','S-1-5-32-544')) {
        $r=New-Object Security.AccessControl.FileSystemAccessRule(
            [Security.Principal.SecurityIdentifier]::new($sid),'FullControl',$inherit,'None','Allow')
        $acl.AddAccessRule($r)
    }
    $acl.SetOwner([Security.Principal.SecurityIdentifier]::new('S-1-5-32-544'))
    $read=New-Object Security.AccessControl.FileSystemAccessRule(
        [Security.Principal.SecurityIdentifier]::new($C.expected_user_sid),'ReadAndExecute',$inherit,'None','Allow')
    $acl.AddAccessRule($read)
    Set-Acl -LiteralPath $C.directory -AclObject $acl
    # Staging must be fresh: only this kit/config, no inherited reparse points or prior trial state.
    foreach($f in Get-ChildItem -LiteralPath $C.directory -File) {
        $fa=Get-Acl -LiteralPath $f.FullName
        $fa.SetAccessRuleProtection($false,$false)
        foreach($r in @($fa.Access|Where-Object {-not $_.IsInherited})){$fa.RemoveAccessRuleSpecific($r)}
        $fa.SetOwner([Security.Principal.SecurityIdentifier]::new('S-1-5-32-544'))
        Set-Acl -LiteralPath $f.FullName -AclObject $fa
    }
    $ev=$C.directory+'\evidence'
    New-Item -ItemType Directory -Path $ev | Out-Null
    $ea=Get-Acl -LiteralPath $ev
    $r=New-Object Security.AccessControl.FileSystemAccessRule(
        [Security.Principal.SecurityIdentifier]::new($C.expected_user_sid),'Modify',$inherit,'None','Allow')
    $ea.AddAccessRule($r); Set-Acl -LiteralPath $ev -AclObject $ea
}
function Get-GuiTaskName($C,[switch]$Guard) {
    $suffix=if($Guard){'-cleanup'}else{'-supervisor'}
    return 'BC250-GUI-'+$C.trial_id+$suffix
}
function Register-GuiTask($C,[string]$Script,[string]$ConfigPath,[switch]$Guard) {
    $name=Get-GuiTaskName $C -Guard:$Guard
    if(Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue){throw 'Task exists; do not overwrite'}
    $mode=if($Guard){'Guard'}else{'Supervise'}
    $exe=$env:WINDIR+'\System32\WindowsPowerShell\v1.0\powershell.exe'
    $args=@('-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',$Script,'-Mode',$mode,'-ConfigPath',$ConfigPath)
    $action=New-ScheduledTaskAction -Execute $exe -Argument (($args|ForEach-Object {ConvertTo-GuiArgument $_}) -join ' ') -WorkingDirectory $C.directory
    $principal=New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
    $settings=New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Seconds 180) -MultipleInstances IgnoreNew -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
    if($Guard) {
        Register-ScheduledTask -TaskName $name -Action $action -Principal $principal -Settings $settings -Trigger (New-ScheduledTaskTrigger -AtStartup) | Out-Null
    } else {
        Register-ScheduledTask -TaskName $name -Action $action -Principal $principal -Settings $settings | Out-Null
    }
}
function Assert-GuiPrepared($C,[string]$ConfigPath) {
    $p=Read-GuiJson ($C.directory+'\prepared.json')
    if($p.config_sha256 -ine (Get-FileHash -LiteralPath $ConfigPath -Algorithm SHA256).Hash){throw 'Prepared configuration changed'}
    foreach($s in @($p.scripts)){Assert-GuiArtifact $s}
}
function Invoke-GuiWorker($C,[string]$ConfigPath) {
    $session=(Get-Process -Id $PID).SessionId
    $identity=[Security.Principal.WindowsIdentity]::GetCurrent()
    $admin=([Security.Principal.WindowsPrincipal]::new($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if($session -ne $C.expected_session_id -or $session -le 0 -or $identity.User.Value -cne $C.expected_user_sid -or $admin) {
        throw 'Active-console child is not the expected unelevated user/session'
    }
    $b=Read-GuiJson ($C.directory+'\running.json')
    if($b.boot_id -cne (Get-GuiBoot) -or (Get-GuiQpc) -ge $b.work_deadline_qpc){throw 'Worker boundary invalid'}
    if(Test-Path -LiteralPath ($C.directory+'\evidence\STOP')){throw 'STOP before worker'}
    $env:TEMP=$C.directory+'\evidence\temp'; $env:TMP=$env:TEMP
    New-Item -ItemType Directory -Path $env:TEMP -Force | Out-Null
    Write-GuiJson ($C.directory+'\evidence\worker-identity.json') @{schema=1;trial_id=$C.trial_id;pid=$PID;session_id=$session;user_sid=$identity.User.Value;elevated=$admin;boot_id=$b.boot_id}
    # Adapter interface deliberately separate from candidate CLI. No candidate flags are guessed here.
    & $C.scenario_adapter.path -ConfigPath $ConfigPath -ContextPath ($C.directory+'\running.json') -ResultPath ($C.directory+'\evidence\scenario-result.json')
    if(-not (Test-Path -LiteralPath ($C.directory+'\evidence\scenario-result.json'))){throw 'Adapter returned without a terminal record'}
}
function Invoke-GuiSupervisor($C,[string]$Script,[string]$ConfigPath) {
    Assert-GuiSystem
    $script:GuiSequence=0; $script:GuiAuxClosure=$true; $child=$null; $closure=$true; $offlineClean=$true
    $status='failed'; $reason='supervisor-incomplete'; $b=$null
    try {
        Assert-GuiPrepared $C $ConfigPath
        Assert-GuiInputs $C
        $start=Read-GuiJson ($C.directory+'\start.json')
        $freq=[Diagnostics.Stopwatch]::Frequency; $boot=Get-GuiBoot
        if($start.boot_id -cne $boot -or $start.frequency -ne $freq -or (Get-GuiQpc) -lt $start.qpc){throw 'Start boundary invalid'}
        $b=[ordered]@{schema=1;trial_id=$C.trial_id;trial=$C.trial;stage=$C.stage;directory=$C.directory;boot_id=$boot;
            qpc=[long]$start.qpc;frequency=$freq;work_deadline_qpc=([long]$start.qpc+($C.duration_seconds-20)*$freq);
            child_deadline_qpc=([long]$start.qpc+($C.duration_seconds-10)*$freq);
            deadline_qpc=([long]$start.qpc+$C.duration_seconds*$freq);
            stop_file=($C.directory+'\evidence\STOP');cancel_file=($C.directory+'\evidence\CANCEL');
            scenario_arguments=@($C.scenario_arguments)}
        if((Get-GuiQpc) -ge $b.work_deadline_qpc){throw 'No work budget'}
        $prior=Get-GuiField $C 'previous_result'
        if($C.stage -in @('resume','verify','observe-40','observe-24')) {
            if(-not $prior){throw 'Previous stage evidence required'}
            Assert-GuiArtifact $prior
            $p=Read-GuiJson $prior.path
            if($p.status -cne 'evidence-ready' -or $p.boot_id -ceq $boot -or $p.trial -cne $C.trial){throw 'Previous successful stage and a NEW boot required'}
            if($C.stage -eq 'observe-40' -and $p.stage -cne 'request-40'){throw 'Expected request-40 predecessor'}
            if($C.stage -eq 'observe-24' -and $p.stage -cne 'request-24'){throw 'Expected request-24 predecessor'}
        }
        $stop=Get-GuiStop $C
        if($stop){throw ('Preflight stop: '+$stop)}
        Write-GuiJson ($C.directory+'\running.json') $b
        Add-GuiEvent $C 'running' $b
        Write-GuiJson ($C.directory+'\heartbeat.json') @{qpc=(Get-GuiQpc)}
        if($C.trial -eq 'L-OFF') {
            $offlineClean=$false
            Start-ScheduledTask -TaskName (Get-GuiTaskName $C -Guard)
            $until=(Get-GuiQpc)+3*$freq
            while(-not (Test-Path -LiteralPath ($C.directory+'\guard-ready.json')) -and (Get-GuiQpc) -lt $until){Start-Sleep -Milliseconds 50}
            if(-not (Test-Path -LiteralPath ($C.directory+'\guard-ready.json'))){throw 'Independent cleanup watchdog not ready'}
            $ack=Read-GuiJson ($C.directory+'\guard-ready.json')
            if($ack.boot_id -cne $b.boot_id -or $ack.qpc -lt $b.qpc){throw 'Stale watchdog acknowledgement'}
            Enable-GuiOffline $C $b
            Add-GuiEvent $C 'stage' @{name='offline-ready';action='record local management continuity and half-scale screenshot'}
        }
        $thermalSequence=0; $nextThermal=0L
        if($C.trial -eq 'L-CU') {
            if((Read-GuiTemperature $C $b $thermalSequence) -ge 87){throw 'Thermal stop before GUI'}
            $thermalSequence++; $nextThermal=(Get-GuiQpc)+2*$freq
        }
        $ps=$env:WINDIR+'\System32\WindowsPowerShell\v1.0\powershell.exe'
        $args=@('-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',$Script,'-Mode','Worker','-ConfigPath',$ConfigPath)
        $closure=$false
        $child=Start-GuiBounded $C 'scenario' $ps $args $b.child_deadline_qpc -Interactive
        Add-GuiEvent $C 'stage' @{name='scenario-started';action='host overlay and screenshots at scale 0.5; manual scenario actions'}
        $stopReason=''; $stopAt=0L
        while(-not $child.process.HasExited) {
            $now=Get-GuiQpc
            Write-GuiJson ($C.directory+'\heartbeat.json') @{qpc=$now}
            if(-not $stopReason) {
                $stopReason=Get-GuiStop $C
                if(-not $stopReason -and $now -ge $b.work_deadline_qpc){$stopReason='work-deadline'}
                if(-not $stopReason -and $C.trial -eq 'L-CU' -and $now -ge $nextThermal) {
                    try {
                        if((Read-GuiTemperature $C $b $thermalSequence) -ge 87){$stopReason='temperature-at-or-above-87C'}
                    } catch {$stopReason='thermal-reading-unavailable'}
                    $thermalSequence++; $nextThermal=(Get-GuiQpc)+2*$freq
                }
                if($stopReason) {
                    $stopAt=Get-GuiQpc
                    Set-GuiStop $C $stopReason
                    Add-GuiEvent $C 'stage' @{name='stopping';reason=$stopReason}
                }
            }
            if($stopReason -and ((Get-GuiQpc)-$stopAt -ge 3*$freq -or -not (Test-GuiMutating $C))) {Set-GuiStop $C $stopReason -Hard}
            if((Get-GuiQpc) -ge $b.child_deadline_qpc){throw 'Helper missed absolute deadline'}
            Start-Sleep -Milliseconds 100
        }
        $r=Complete-GuiBounded $C $child $b.deadline_qpc
        $closure=($r.receipt.job_empty -eq $true)
        # An empty worker Job cannot prove that a UAC-brokered engine has stopped.
        # Validate this invocation's elevated closure on cancellation as well as success.
        if(Test-GuiMutating $C) {
            try {
                $mutationResult=Read-GuiJson ($C.directory+'\evidence\scenario-result.json')
                Assert-GuiMutationClosure $C $mutationResult $boot
            } catch {
                $closure=$false
                throw 'Mutating stage lacks this-run elevated closure/durable boundary evidence'
            }
        }
        if($stopReason){$status='stopped';$reason=$stopReason}
        elseif($r.exit_code -ne 0 -or $r.receipt.child_exit -ne 0 -or -not $r.receipt.root_exit_observed){throw 'Scenario process failed'}
        else {
            $s=Read-GuiJson ($C.directory+'\evidence\scenario-result.json')
            if($s.schema -ne 1 -or $s.trial_id -cne $C.trial_id -or $s.stage -cne $C.stage -or
                $s.boot_id -cne $boot -or $s.outcome -notin @('complete','pending-restart') -or $s.tree_closed -ne $true) {
                throw 'Scenario terminal result incomplete or invalid'
            }
            if(Test-GuiMutating $C) {
                Assert-GuiMutationClosure $C $s $boot
            }
            if($C.trial -eq 'L-OFF') {
                $blocked=-not (Test-GuiExternalConnection $C)
                Write-GuiJson ($C.directory+'\offline-end.json') @{blocked=$blocked;qpc=(Get-GuiQpc)}
                if(-not $blocked){throw 'External access returned before repair ended'}
            }
            $status='evidence-ready'; $reason='host-checklist-still-required'
        }
    } catch {
        $reason=$_.Exception.Message
        if(($child -and -not $closure) -or -not $script:GuiAuxClosure){$status='recovery-required'}
    } finally {
        if($child) {
            if(-not $child.process.HasExited) {
                Set-GuiStop $C 'supervisor-finally'
                Set-GuiStop $C 'supervisor-finally' -Hard
                if(-not $child.process.WaitForExit(1000)){$child.process.Kill()}
                $status='recovery-required'; $closure=$false
            }
            $child.process.Dispose()
        }
        if($C.trial -eq 'L-OFF') {
            try {$offlineClean=Disable-GuiOffline $C; if(-not $offlineClean){throw 'Offline cleanup unverified'}}
            catch {$offlineClean=$false; $status='recovery-required';$reason+='; firewall cleanup unverified'}
        }
        $elapsed=if($b){((Get-GuiQpc)-$b.qpc)/[double]$b.frequency}else{$null}
        $result=[ordered]@{schema=1;trial_id=$C.trial_id;trial=$C.trial;stage=$C.stage;status=$status;reason=$reason;
            boot_id=$(if($b){$b.boot_id}else{$null});elapsed_seconds=$elapsed;tree_closed=($closure -and $script:GuiAuxClosure);firewall_restored=$offlineClean}
        Write-GuiJson ($C.directory+'\result.json') $result
        Add-GuiEvent $C 'result' $result
    }
}

function Assert-GuiTask($C,[string]$Script,[string]$ConfigPath,[switch]$Guard) {
    $t=Get-ScheduledTask -TaskName (Get-GuiTaskName $C -Guard:$Guard)
    $mode=if($Guard){'Guard'}else{'Supervise'}
    $exe=$env:WINDIR+'\System32\WindowsPowerShell\v1.0\powershell.exe'
    $args=@('-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',$Script,'-Mode',$mode,'-ConfigPath',$ConfigPath)
    $expected=($args|ForEach-Object {ConvertTo-GuiArgument $_}) -join ' '
    if(@($t.Actions).Count -ne 1 -or $t.Actions[0].Execute -ine $exe -or $t.Actions[0].Arguments -cne $expected -or
        $t.Principal.UserId -notin @('SYSTEM','S-1-5-18') -or $t.Settings.ExecutionTimeLimit -cne 'PT3M') {throw 'Scheduled task identity mismatch'}
    return $t
}
function Invoke-GuiTrial {
    param([string]$Mode,[string]$ConfigPath,[string]$Trial,[string]$Script)
    $C=Read-GuiJson $ConfigPath
    Assert-GuiConfig $C $Trial $ConfigPath
    . ($PSScriptRoot+'\gui-trial-firewall.ps1')
    if($Mode -eq 'Prepare') {
        if(Test-Path -LiteralPath ($C.directory+'\prepared.json')){throw 'Already prepared; use a new trial_id'}
        if(Test-Path -LiteralPath ($C.directory+'\evidence')){throw 'Evidence already exists; use a new trial_id'}
        if((Get-Item -LiteralPath $C.directory).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Reparse directory refused'}
        foreach($f in Get-ChildItem -LiteralPath $C.directory -Recurse -Force) {
            if($f.Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Reparse entry refused'}
        }
        Assert-GuiInputs $C
        Set-GuiAcl $C
        $scripts=@($Script,($PSScriptRoot+'\gui-trial-common.ps1'),($PSScriptRoot+'\gui-trial-firewall.ps1'))|ForEach-Object {
            [pscustomobject]@{path=$_;sha256=(Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash}
        }
        foreach($guard in @($false,$true)) {
            if($guard -and $Trial -ne 'L-OFF'){continue}
            if(Get-ScheduledTask -TaskName (Get-GuiTaskName $C -Guard:$guard) -ErrorAction SilentlyContinue){throw 'Task collision before preparation'}
        }
        # Persist recoverable identity before registering either task.
        Write-GuiJson ($C.directory+'\prepared.json') @{schema=1;config_sha256=(Get-FileHash -LiteralPath $ConfigPath -Algorithm SHA256).Hash;scripts=@($scripts)}
        try {
            Register-GuiTask $C $Script $ConfigPath
            if($Trial -eq 'L-OFF'){Register-GuiTask $C $Script $ConfigPath -Guard}
        } catch {
            $failure=$_
            foreach($guard in @($false,$true)) {
                if($guard -and $Trial -ne 'L-OFF'){continue}
                $name=Get-GuiTaskName $C -Guard:$guard
                if(Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue) {
                    [void](Assert-GuiTask $C $Script $ConfigPath -Guard:$guard)
                    Unregister-ScheduledTask -TaskName $name -Confirm:$false
                }
            }
            throw $failure
        }
        $e=New-GuiEvent $C 'prepared' @{directory=$C.directory};$e.status='prepared';$e|ConvertTo-Json -Compress -Depth 10
        return
    }
    Assert-GuiPrepared $C $ConfigPath
    if($Mode -eq 'Start') {
        Assert-GuiInputs $C
        if(Test-Path -LiteralPath ($C.directory+'\start.json')){throw 'Trial is single-use; Observe it, never restart it'}
        $stop=Get-GuiStop $C; if($stop){throw ('Start refused: '+$stop)}
        $task=Assert-GuiTask $C $Script $ConfigPath
        if($task.State -ne 'Ready'){throw 'Supervisor task is not Ready'}
        if($Trial -eq 'L-OFF'){[void](Assert-GuiTask $C $Script $ConfigPath -Guard)}
        Write-GuiJson ($C.directory+'\start.json') @{qpc=(Get-GuiQpc);frequency=[Diagnostics.Stopwatch]::Frequency;boot_id=(Get-GuiBoot)}
        Start-ScheduledTask -TaskName (Get-GuiTaskName $C)
        $until=(Get-GuiQpc)+5*[Diagnostics.Stopwatch]::Frequency
        while((Get-GuiQpc) -lt $until) {
            if(Test-Path -LiteralPath ($C.directory+'\running.json')) {
                $b=Read-GuiJson ($C.directory+'\running.json')
                $e=New-GuiEvent $C 'running' $b;$e.status='running';$e|ConvertTo-Json -Depth 10 -Compress
                return
            }
            if(Test-Path -LiteralPath ($C.directory+'\result.json')){throw 'Supervisor refused before running'}
            Start-Sleep -Milliseconds 50
        }
        Set-GuiStop $C 'start-acknowledgement-timeout'
        throw 'No running acknowledgement; STOP requested, inspect task'
    }
    if($Mode -eq 'Observe') {
        $events=$C.directory+'\events.jsonl'
        if(Test-Path -LiteralPath $events) {
            if((Get-Item -LiteralPath $events).Length -gt 4194304){throw 'Event log exceeds 4 MiB'}
            foreach($line in [IO.File]::ReadAllLines($events)) {
                try {$v=$line|ConvertFrom-Json; if($v.protocol -ceq 'bc250.gui-trial.v1' -and $v.trial_id -ceq $C.trial_id){$line}} catch {}
            }
        }
        $resultPath=$C.directory+'\result.json'
        $state='prepared';$detail=$null
        if(Test-Path -LiteralPath ($C.directory+'\start.json')){$state='starting'}
        if(Test-Path -LiteralPath ($C.directory+'\running.json')){$state='running'}
        if(Test-Path -LiteralPath ($C.directory+'\evidence\STOP')){$state='stopping'}
        if(Test-Path -LiteralPath $resultPath){$detail=Read-GuiJson $resultPath;$state=$detail.status}
        else {
            $t=Get-ScheduledTask -TaskName (Get-GuiTaskName $C) -ErrorAction SilentlyContinue
            if($state -ne 'prepared' -and (-not $t -or $t.State -ne 'Running')){$state='recovery-required'}
        }
        $supervisor=Get-ScheduledTask -TaskName (Get-GuiTaskName $C) -ErrorAction SilentlyContinue
        $guardTask=if($Trial -eq 'L-OFF'){Get-ScheduledTask -TaskName (Get-GuiTaskName $C -Guard) -ErrorAction SilentlyContinue}else{$null}
        $tasksIdle=(-not $supervisor -or $supervisor.State -ne 'Running') -and (-not $guardTask -or $guardTask.State -ne 'Running')
        $closed=if(Test-Path -LiteralPath ($C.directory+'\start.json')) {
            $detail -and $detail.tree_closed -eq $true -and $detail.firewall_restored -eq $true -and $detail.status -ne 'recovery-required'
        }else{$true}
        $e=New-GuiEvent $C 'state' $detail;$e.status=$state;$e.can_cleanup=([bool]$tasksIdle -and [bool]$closed);$e|ConvertTo-Json -Depth 12 -Compress
        return
    }
    if($Mode -eq 'Stop') {
        Set-GuiStop $C 'host-stop'
        $e=New-GuiEvent $C 'stop' @{requested=$true};$e.status='stopping';$e|ConvertTo-Json -Depth 8 -Compress
        return
    }
    if($Mode -eq 'Cleanup') {
        $t=Get-ScheduledTask -TaskName (Get-GuiTaskName $C) -ErrorAction SilentlyContinue
        if($t -and $t.State -eq 'Running'){throw 'Supervisor still running; Stop and Observe before Cleanup'}
        if($Trial -eq 'L-OFF' -and -not (Disable-GuiOffline $C)){throw 'Firewall cleanup not verified'}
        if(Test-Path -LiteralPath ($C.directory+'\start.json')) {
            if(-not (Test-Path -LiteralPath ($C.directory+'\result.json'))){throw 'Missing terminal record; recovery required'}
            $r=Read-GuiJson ($C.directory+'\result.json')
            if($r.status -eq 'recovery-required' -or -not $r.tree_closed -or -not $r.firewall_restored){throw 'Terminal result does not prove closure; preserve tasks/evidence for recovery'}
        }
        foreach($guard in @($false,$true)) {
            if($guard -and $Trial -ne 'L-OFF'){continue}
            $name=Get-GuiTaskName $C -Guard:$guard
            $task=Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue
            if($task -and $task.State -eq 'Running'){throw 'Cleanup watchdog still active'}
            if($task){Unregister-ScheduledTask -TaskName $name -Confirm:$false}
        }
        # All logs, original config and result.json deliberately survive.
        $e=New-GuiEvent $C 'cleanup' @{evidence_preserved=$true};$e.status='cleaned';$e|ConvertTo-Json -Depth 8 -Compress
        return
    }
    if($Mode -eq 'Supervise'){Invoke-GuiSupervisor $C $Script $ConfigPath;return}
    if($Mode -eq 'Worker'){Invoke-GuiWorker $C $ConfigPath;return}
    if($Mode -eq 'Guard'){Invoke-GuiFirewallGuard $C;return}
    throw 'Unknown mode'
}
