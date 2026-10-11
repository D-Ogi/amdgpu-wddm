Set-StrictMode -Version 2
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Security
$script:RegistryView=[Microsoft.Win32.RegistryView]::Registry64
$script:Root=Join-Path ([Environment]::GetFolderPath('CommonApplicationData')) 'amdgpu-wddm\system-tuning'
$script:Entropy=[Text.Encoding]::UTF8.GetBytes('amdgpu-wddm/system-tuning/schema-1')
$script:Scope='Machine'
$script:UserSid=''
$script:Hive=[Microsoft.Win32.RegistryHive]::LocalMachine
$script:Protection=[Security.Cryptography.DataProtectionScope]::LocalMachine

function Test-TuningAdmin {
    $identity=[Security.Principal.WindowsIdentity]::GetCurrent()
    try { return ([Security.Principal.WindowsPrincipal]::new($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator) }
    finally { $identity.Dispose() }
}

function Assert-ProtectedPath([string]$Path) {
    $item=Get-Item -LiteralPath $Path -Force -ErrorAction Stop
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'State path contains a reparse point.' }
    $acl=Get-Acl -LiteralPath $Path
    $trusted=@('S-1-5-18','S-1-5-32-544')
    if($script:Scope -eq 'User'){$trusted+=,$script:UserSid}
    $owner=$acl.GetOwner([Security.Principal.SecurityIdentifier]).Value
    if ($trusted -notcontains $owner) { throw 'State path has an untrusted owner.' }
    $writeMask=[Security.AccessControl.FileSystemRights]::Write -bor [Security.AccessControl.FileSystemRights]::Delete -bor [Security.AccessControl.FileSystemRights]::DeleteSubdirectoriesAndFiles -bor [Security.AccessControl.FileSystemRights]::ChangePermissions -bor [Security.AccessControl.FileSystemRights]::TakeOwnership
    foreach($rule in $acl.GetAccessRules($true,$true,[Security.Principal.SecurityIdentifier])) {
        if ($rule.AccessControlType -eq 'Allow' -and ($rule.FileSystemRights -band $writeMask) -ne 0 -and $trusted -notcontains $rule.IdentityReference.Value) { throw 'State path allows untrusted writes.' }
    }
}

function New-ProtectedDirectory([string]$Path) {
    if ([IO.Directory]::Exists($Path)) { Assert-ProtectedPath $Path; return }
    if ([IO.File]::Exists($Path)) { throw 'A file occupies the state directory.' }
    $acl=New-Object Security.AccessControl.DirectorySecurity
    $admins=New-Object Security.Principal.SecurityIdentifier('S-1-5-32-544')
    $system=New-Object Security.Principal.SecurityIdentifier('S-1-5-18')
    $users=New-Object Security.Principal.SecurityIdentifier('S-1-5-32-545')
    $owner=if($script:Scope -eq 'User'){New-Object Security.Principal.SecurityIdentifier($script:UserSid)}else{$admins}
    $acl.SetOwner($owner);$acl.SetAccessRuleProtection($true,$false)
    foreach($sid in @($admins,$system)) { $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($sid,'FullControl','ContainerInherit,ObjectInherit','None','Allow')) }
    if($script:Scope -eq 'User'){$acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($owner,'FullControl','ContainerInherit,ObjectInherit','None','Allow'))}
    else{$acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($users,'ReadAndExecute','ContainerInherit,ObjectInherit','None','Allow'))}
    $dir=New-Object IO.DirectoryInfo($Path);$dir.Create($acl)
    Assert-ProtectedPath $Path
}

function Assert-StateChain([bool]$Create) {
    $parent=Split-Path -Parent $script:Root
    foreach($path in @($parent,$script:Root)) {
        if ($Create) { New-ProtectedDirectory $path }
        elseif ([IO.Directory]::Exists($path) -or [IO.File]::Exists($path)) { Assert-ProtectedPath $path }
    }
}

function New-ProtectedFile([string]$Path,[IO.FileOptions]$Options=[IO.FileOptions]::None) {
    $security=New-Object Security.AccessControl.FileSecurity
    $admins=New-Object Security.Principal.SecurityIdentifier('S-1-5-32-544')
    $system=New-Object Security.Principal.SecurityIdentifier('S-1-5-18')
    $owner=if($script:Scope -eq 'User'){New-Object Security.Principal.SecurityIdentifier($script:UserSid)}else{$admins}
    $security.SetOwner($owner);$security.SetAccessRuleProtection($true,$false)
    foreach($sid in @($admins,$system,$owner)){$security.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($sid,'FullControl','Allow'))}
    if($script:Scope -eq 'Machine'){
        $users=New-Object Security.Principal.SecurityIdentifier('S-1-5-32-545')
        $security.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($users,'Read','Allow'))
    }
    return [IO.FileStream]::new($Path,[IO.FileMode]::CreateNew,[Security.AccessControl.FileSystemRights]::Read -bor [Security.AccessControl.FileSystemRights]::Write,[IO.FileShare]::None,4096,$Options,$security)
}

function Lock-TuningState {
    Assert-StateChain $true
    $path=Join-Path $script:Root 'machine.lock'
    if ([IO.File]::Exists($path)) { Assert-ProtectedPath $path }
    # Share.None spans processes and sessions. Do not delete or replace this file.
    $stream=if([IO.File]::Exists($path)){
        [IO.File]::Open($path,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
    }else{New-ProtectedFile $path}
    try { Assert-ProtectedPath $path; return $stream } catch { $stream.Dispose(); throw }
}

function Read-TuningJournal {
    Assert-StateChain $false
    $path=Join-Path $script:Root 'state.dpapi'
    if (-not [IO.File]::Exists($path)) { return $null }
    Assert-ProtectedPath $path
    $stream=[IO.File]::Open($path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
    try {
        if ($stream.Length -gt 1MB) { throw 'State file exceeds the size limit.' }
        $buffer=New-Object byte[] ([int]$stream.Length);$offset=0
        while($offset -lt $buffer.Length) { $n=$stream.Read($buffer,$offset,$buffer.Length-$offset);if($n -eq 0){throw 'Truncated state file.'};$offset+=$n }
    } finally { $stream.Dispose() }
    Add-Type -AssemblyName System.Security
    $plain=[Security.Cryptography.ProtectedData]::Unprotect($buffer,$script:Entropy,$script:Protection)
    try { return ([Text.UTF8Encoding]::new($false,$true)).GetString($plain) | ConvertFrom-Json }
    finally { [Array]::Clear($plain,0,$plain.Length) }
}

function Write-TuningJournal($Journal) {
    Assert-StateChain $false
    $path=Join-Path $script:Root 'state.dpapi'
    if ([IO.File]::Exists($path)) { Assert-ProtectedPath $path }
    Add-Type -AssemblyName System.Security
    $plain=[Text.Encoding]::UTF8.GetBytes(($Journal | ConvertTo-Json -Depth 16 -Compress))
    try { $bytes=[Security.Cryptography.ProtectedData]::Protect($plain,$script:Entropy,$script:Protection) }
    finally { [Array]::Clear($plain,0,$plain.Length) }
    $temp=Join-Path $script:Root ([Guid]::NewGuid().ToString('N')+'.tmp')
    try {
        $stream=New-ProtectedFile $temp ([IO.FileOptions]::WriteThrough)
        try { $stream.Write($bytes,0,$bytes.Length);$stream.Flush($true) } finally { $stream.Dispose() }
        Assert-ProtectedPath $temp
        if ([IO.File]::Exists($path)) { [IO.File]::Replace($temp,$path,[Management.Automation.Language.NullString]::Value) } else { [IO.File]::Move($temp,$path) }
        Assert-ProtectedPath $path
    } finally { if([IO.File]::Exists($temp)){[IO.File]::Delete($temp)} }
}

function Read-FixedValue([string]$Key,[string]$Name,[string]$ExpectedKind) {
    $base=[Microsoft.Win32.RegistryKey]::OpenBaseKey($script:Hive,$script:RegistryView)
    try {
        $reg=$base.OpenSubKey($Key,$false)
        if($null -eq $reg){return @{exists=$false;kind=$ExpectedKind;data=$null}}
        try {
            if($reg.GetValueNames() -notcontains $Name){return @{exists=$false;kind=$ExpectedKind;data=$null}}
            return @{exists=$true;kind=[string]$reg.GetValueKind($Name);data=$reg.GetValue($Name,$null,[Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)}
        } finally {$reg.Dispose()}
    } finally {$base.Dispose()}
}

function Write-FixedValue([string]$Key,[string]$Name,$Value) {
    $base=[Microsoft.Win32.RegistryKey]::OpenBaseKey($script:Hive,$script:RegistryView)
    try {
        $reg=if($Value.exists){$base.CreateSubKey($Key)}else{$base.OpenSubKey($Key,$true)}
        if($null -eq $reg){return}
        try {
            if($Value.exists){$reg.SetValue($Name,$Value.data,[Microsoft.Win32.RegistryValueKind][Enum]::Parse([Microsoft.Win32.RegistryValueKind],$Value.kind))}
            else{$reg.DeleteValue($Name,$false)}
            $reg.Flush()
        } finally {$reg.Dispose()}
    } finally {$base.Dispose()}
}

function Get-UpdateSupport {
    $edition=Read-FixedValue 'SOFTWARE\Microsoft\Windows NT\CurrentVersion' 'EditionID' 'String'
    if(-not $edition.exists -or $edition.data -notmatch '^(Professional|Enterprise|Education|IoTEnterprise)') { return @{supported=$false;note='Requires a supported Pro, Enterprise or Education edition.'} }
    $ignore=Read-FixedValue 'SOFTWARE\Microsoft\PolicyManager\current\device\Update' 'IgnoreWindowsUpdateGroupPolicies' 'DWord'
    if($ignore.exists -and $ignore.data -eq 1){return @{supported=$false;note='Device management overrides Windows Update group policies.'}}
    $computer=Get-CimInstance -ClassName Win32_ComputerSystem -Property PartOfDomain
    if($computer.PartOfDomain){return @{supported=$false;note='Domain-managed update policy must be changed by its administrator.'}}
    return @{supported=$true;note='Configured registry policy is not proof of effective Windows Update state.'}
}

function Get-FixedTask([string]$FullName) {
    $scheduler=$null;$folder=$null
    try {
        $scheduler=New-Object -ComObject 'Schedule.Service'
        $scheduler.Connect()
        $folder=$scheduler.GetFolder((Split-Path -Parent $FullName))
        return $folder.GetTask((Split-Path -Leaf $FullName))
    } catch {
        $exception=$_.Exception
        while($null -ne $exception.InnerException){$exception=$exception.InnerException}
        # HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND / ERROR_PATH_NOT_FOUND).
        if($exception.HResult -in @(-2147024894,-2147024893)){return $null}
        throw
    } finally {
        if($null -ne $folder){[void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($folder)}
        if($null -ne $scheduler){[void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($scheduler)}
    }
}

function Initialize-ServiceInterop {
    if('AmdgpuWddm.SystemTuning.ServiceConfig' -as [type]){return}
    # Compile only for mutations. List never creates compiler temporary files.
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
namespace AmdgpuWddm.SystemTuning {
    public static class ServiceConfig {
        private const uint ScManagerConnect = 0x0001;
        private const uint ServiceQueryConfig = 0x0001;
        private const uint ServiceChangeConfig = 0x0002;
        private const uint DelayedAutoStartInfo = 3;
        [DllImport("advapi32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
        private static extern IntPtr OpenSCManagerW(string machine, string database, uint access);
        [DllImport("advapi32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
        private static extern IntPtr OpenServiceW(IntPtr manager, string name, uint access);
        [DllImport("advapi32.dll", SetLastError=true)]
        [return: MarshalAs(UnmanagedType.Bool)] private static extern bool CloseServiceHandle(IntPtr handle);
        [DllImport("advapi32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
        [return: MarshalAs(UnmanagedType.Bool)] private static extern bool QueryServiceConfig2W(IntPtr service, uint level, out int info, uint bytes, out uint needed);
        [DllImport("advapi32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
        [return: MarshalAs(UnmanagedType.Bool)] private static extern bool ChangeServiceConfig2W(IntPtr service, uint level, ref int info);
        private static void CheckName(string name) {
            if (name != "SysMain" && name != "WSearch" && name != "DiagTrack" && name != "MapsBroker")
                throw new ArgumentException("Service is outside the fixed catalog.");
        }
        public static bool Delayed(string name, bool? desired) {
            CheckName(name);
            IntPtr manager = OpenSCManagerW(null, null, ScManagerConnect);
            if (manager == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
            try {
                IntPtr service = OpenServiceW(manager, name, ServiceQueryConfig | (desired.HasValue ? ServiceChangeConfig : 0));
                if (service == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
                try {
                    if (desired.HasValue) {
                        int value = desired.Value ? 1 : 0;
                        if (!ChangeServiceConfig2W(service, DelayedAutoStartInfo, ref value))
                            throw new Win32Exception(Marshal.GetLastWin32Error());
                    }
                    int actual; uint needed;
                    if (!QueryServiceConfig2W(service, DelayedAutoStartInfo, out actual, 4, out needed))
                        throw new Win32Exception(Marshal.GetLastWin32Error());
                    if (needed > 4) throw new InvalidOperationException("Unexpected service configuration size.");
                    return actual != 0;
                } finally { CloseServiceHandle(service); }
            } finally { CloseServiceHandle(manager); }
        }
    }
}
'@
}

function Read-NativeItem($Descriptor) {
    $state=$null;$supported=$true;$note='';$observed=$null
    switch($Descriptor.kind) {
        service {
            $key='SYSTEM\CurrentControlSet\Services\'+$Descriptor.target
            $start=Read-FixedValue $key 'Start' 'DWord'
            $delayed=Read-FixedValue $key 'DelayedAutoStart' 'DWord'
            if(-not $start.exists){$state=@{present=$false;start=4;running=$false;delayed=$delayed};break}
            $service=Get-Service -Name $Descriptor.target
            try {
                if($service.Status -notin @('Stopped','Running')) { throw 'Service is changing state. Try again later.' }
                $state=@{present=$true;start=[int]$start.data;running=($service.Status -eq 'Running');delayed=$delayed}
                if($state.start -eq 4 -and $state.running){$supported=$false;$note='A disabled but running service cannot be restored exactly after stopping.'}
            } finally {$service.Dispose()}
        }
        task {
            $task=Get-FixedTask $Descriptor.target
            try{$state=@{present=($null -ne $task);enabled=($null -ne $task -and [bool]$task.Enabled)}}
            finally{if($null -ne $task){[void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($task)}}
        }
        autostart {
            $value=Read-FixedValue $Descriptor.target $Descriptor.value 'String'
            $state=@{present=$value.exists;value=$value}
        }
        driverPolicy {
            $support=Get-UpdateSupport;$supported=$support.supported;$note=$support.note
            $state=@{present=$true;exclude=(Read-FixedValue $Descriptor.target 'ExcludeWUDriversInQualityUpdate' 'DWord')}
        }
        pausePolicy {
            $support=Get-UpdateSupport;$supported=$support.supported;$note=$support.note
            $state=@{present=$true;quality=(Read-FixedValue $Descriptor.target 'PauseQualityUpdatesStartTime' 'String');feature=(Read-FixedValue $Descriptor.target 'PauseFeatureUpdatesStartTime' 'String')}
            $observed=@{qualityStatus=$null;featureStatus=$null}
            foreach($pair in @(@('qualityStatus','PausedQualityStatus'),@('featureStatus','PausedFeatureStatus'))){
                $v=Read-FixedValue 'SOFTWARE\Microsoft\WindowsUpdate\UpdatePolicy\Settings' $pair[1] 'DWord'
                if($v.exists -and $v.data -in @(0,1,2)){$observed[$pair[0]]=[int]$v.data}
            }
        }
        default {throw 'Unknown item kind.'}
    }
    return @{state=$state;supported=$supported;note=$note;observed=$observed}
}

function Write-NativeItem($Descriptor,$State) {
    # Descriptor always comes from the built-in catalog, never from the journal.
    switch($Descriptor.kind) {
        service {
            Initialize-ServiceInterop
            $delayedBefore=Read-FixedValue ('SYSTEM\CurrentControlSet\Services\'+$Descriptor.target) 'DelayedAutoStart' 'DWord'
            $expectedBefore=($delayedBefore.exists -and $delayedBefore.data -eq 1)
            if([AmdgpuWddm.SystemTuning.ServiceConfig]::Delayed($Descriptor.target,$null) -ne $expectedBefore){throw 'SCM and registry delayed-start settings disagree. No service change was made.'}
            $service=Get-Service -Name $Descriptor.target
            try {
                if(-not $State.running -and $service.Status -eq 'Running') {
                    if(@($service.DependentServices | Where-Object {$_.Status -ne 'Stopped'}).Count -gt 0){throw 'Running dependent services prevent stopping this service.'}
                    if(-not $service.CanStop){throw 'The service does not accept stop requests.'}
                    # sc stop sends only SERVICE_CONTROL_STOP. SCM refuses running dependents.
                    # ServiceController.Stop() also stops dependencies on .NET Framework.
                    $null=& (Join-Path ([Environment]::SystemDirectory) 'sc.exe') stop $Descriptor.target
                    if($LASTEXITCODE -ne 0){throw ('Service stop request failed: '+$LASTEXITCODE)}
                    $service.WaitForStatus([ServiceProcess.ServiceControllerStatus]::Stopped,[TimeSpan]::FromSeconds(10))
                }
                $mode=switch($State.start){2{'Automatic'};3{'Manual'};4{'Disabled'};default{throw 'Invalid startup type.'}}
                Set-Service -Name $Descriptor.target -StartupType $mode
                $desiredDelayed=($State.delayed.exists -and $State.delayed.data -eq 1)
                if([AmdgpuWddm.SystemTuning.ServiceConfig]::Delayed($Descriptor.target,[bool]$desiredDelayed) -ne $desiredDelayed){throw 'SCM did not retain the delayed-start setting.'}
                Write-FixedValue ('SYSTEM\CurrentControlSet\Services\'+$Descriptor.target) 'DelayedAutoStart' $State.delayed
                if([AmdgpuWddm.SystemTuning.ServiceConfig]::Delayed($Descriptor.target,$null) -ne $desiredDelayed){throw 'SCM delayed-start verification failed.'}
                $service.Refresh()
                if($State.running -and $service.Status -ne 'Running'){
                    if($State.start -eq 4){throw 'Cannot restore a running disabled service without changing its startup policy.'}
                    $service.Start();$service.WaitForStatus([ServiceProcess.ServiceControllerStatus]::Running,[TimeSpan]::FromSeconds(10))
                }
            } finally {$service.Dispose()}
        }
        task {
            $task=Get-FixedTask $Descriptor.target
            if($null -eq $task){throw 'Task disappeared before the change.'}
            try{$task.Enabled=$State.enabled}
            finally{[void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($task)}
        }
        autostart { Write-FixedValue $Descriptor.target $Descriptor.value $State.value }
        driverPolicy { Write-FixedValue $Descriptor.target 'ExcludeWUDriversInQualityUpdate' $State.exclude }
        pausePolicy {
            Write-FixedValue $Descriptor.target 'PauseQualityUpdatesStartTime' $State.quality
            Write-FixedValue $Descriptor.target 'PauseFeatureUpdatesStartTime' $State.feature
        }
        default {throw 'Unknown item kind.'}
    }
}

function New-NativeTuningOperations {
    param([ValidateSet('Machine','User')][string]$Scope='Machine')
    $script:Scope=$Scope
    $machine=[Environment]::MachineName
    if($Scope -eq 'User'){
        $identity=[Security.Principal.WindowsIdentity]::GetCurrent()
        try{$script:UserSid=$identity.User.Value}finally{$identity.Dispose()}
        $script:Root=Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'amdgpu-wddm\system-tuning'
        $script:Hive=[Microsoft.Win32.RegistryHive]::CurrentUser
        $script:Protection=[Security.Cryptography.DataProtectionScope]::CurrentUser
        $machine+='|'+$script:UserSid
    }else{
        $script:Root=Join-Path ([Environment]::GetFolderPath('CommonApplicationData')) 'amdgpu-wddm\system-tuning'
        $script:Hive=[Microsoft.Win32.RegistryHive]::LocalMachine
        $script:Protection=[Security.Cryptography.DataProtectionScope]::LocalMachine
    }
    return @{
        Machine=$machine
        Scope=$Scope
        IsAdmin={if($script:Scope -eq 'User'){$true}else{Test-TuningAdmin}}
        Now={[datetime]::UtcNow}
        Acquire={Lock-TuningState}
        Release={param($Lock) $Lock.Dispose()}
        Load={Read-TuningJournal}
        Save={param($Journal) Write-TuningJournal $Journal}
        Read={param($Descriptor) Read-NativeItem $Descriptor}
        Write={param($Descriptor,$State) Write-NativeItem $Descriptor $State}
    }
}

Export-ModuleMember -Function New-NativeTuningOperations
