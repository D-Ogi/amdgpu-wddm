# Boot-state locale regression tests. All WMI/CIM and mutation operations are stubs.
# Synthetic localized display samples are negative controls, not recorded Windows output.
# powershell -NoProfile -File tools/release/test-boot-locale.ps1 [-Out <scratch directory>]
param([string]$Installer = (Join-Path $PSScriptRoot 'installer'), [string]$Out = '')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
$script:Checks = 0
$script:ProviderCalls = 0
$script:CimCalls = 0
$script:NativeCalls = 0
function Check([bool]$Ok, [string]$Message) {
    $script:Checks++
    if (-not $Ok) { throw "FAIL: $Message" }
}
function Load-TestedFunctions([string]$Path, [string[]]$Names) {
    $tokens = $null; $parseErrors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile($Path, [ref]$tokens, [ref]$parseErrors)
    if ($parseErrors.Count) { throw "Parse failure: $Path" }
    foreach ($name in $Names) {
        $functions = @($ast.FindAll({ param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq $name }.GetNewClosure(), $true))
        if ($functions.Count -ne 1) { throw "Missing or repeated function: $name" }
        Set-Item -LiteralPath "Function:script:$name" -Value ([scriptblock]::Create($functions[0].Body.Extent.Text.Trim().Substring(1).TrimEnd().TrimEnd('}')))
    }
}
Load-TestedFunctions (Join-Path $Installer 'common.ps1') @('Get-TestSigningConfigured', 'Get-BitLockerState', 'Test-BitLockerConsentRequired')
function Test-IsAdmin { return $script:Admin }
function Invoke-Native { $script:NativeCalls++; throw 'A native process must never run in this test.' }
function Get-BitLockerVolume { throw 'The BitLocker PowerShell module is unavailable in this fixture.' }
function Get-WmiObject {
    param($Namespace, $Class, [switch]$List, [switch]$EnableAllPrivileges, $ErrorAction)
    $script:ProviderCalls++
    Check ($Namespace -eq 'root\WMI' -and $Class -eq 'BcdStore' -and $List -and $EnableAllPrivileges -and $ErrorAction -eq 'Stop') 'BCD provider contract'
    if ($script:BcdCase -eq 'throw') { throw 'Synthetic provider failure' }
    return $script:Provider
}
$script:Provider = New-Object PSObject
$script:Store = New-Object PSObject
$script:Loader = New-Object PSObject
$script:Provider | Add-Member ScriptMethod OpenStore {
    param($File)
    Check ($File -ceq '') 'BCD system store path'
    return [pscustomobject]@{ ReturnValue = ($script:BcdCase -ne 'store-failed'); Store = $script:Store }
}
$script:Store | Add-Member ScriptMethod OpenObject {
    param($Id)
    Check ($Id -eq '{fa926493-6f1c-4193-a414-58f0b2456d1e}') 'BCD current loader identity'
    return [pscustomobject]@{ ReturnValue = ($script:BcdCase -ne 'object-failed'); Object = $script:Loader }
}
$script:Loader | Add-Member ScriptMethod EnumerateElements {
    $element = [pscustomobject]@{ Type = [uint32]0x16000049; Boolean = $true }
    $elements = @($element)
    switch ($script:BcdCase) {
        'false' { $element.Boolean = $false }
        'absent' { $elements = @([pscustomobject]@{ Type = [uint32]0x12000004; String = 'Description' }) }
        'empty' { $elements = @() }
        'malformed' { $element.Boolean = 'false' }
        'duplicate' { $elements = @($element, $element) }
    }
    return [pscustomobject]@{ ReturnValue = ($script:BcdCase -ne 'enumerate-failed'); Elements = $elements }
}
function Get-CimInstance {
    param($Namespace, $ClassName, $Filter, $ErrorAction)
    $script:CimCalls++
    Check ($Namespace -eq 'root\CIMV2\Security\MicrosoftVolumeEncryption' -and $ClassName -eq 'Win32_EncryptableVolume') 'BitLocker class'
    Check ($Filter -ceq "DriveLetter='Q:'" -and $ErrorAction -eq 'Stop') 'BitLocker exact system-drive query'
    if ($script:CimCase -eq 'throw') { throw 'Synthetic CIM failure' }
    if ($script:CimCase -eq 'absent') { return }
    $volume = [pscustomobject]@{ DriveLetter = 'Q:'; ProtectionStatus = 99 }
    if ($script:CimCase -eq 'duplicate') { return @($volume, $volume) }
    return $volume
}
function Invoke-CimMethod {
    param($InputObject, $MethodName, $Arguments, $ErrorAction)
    Check ($InputObject.DriveLetter -ceq 'Q:' -and $ErrorAction -eq 'Stop') 'BitLocker method target'
    if ($script:CimCase -eq 'method-throws') { throw 'Synthetic method failure' }
    if ($MethodName -eq 'GetProtectionStatus') {
        return [pscustomobject]@{ ReturnValue = $script:ProtectionReturn; ProtectionStatus = $script:Protection }
    }
    if ($MethodName -eq 'GetConversionStatus') {
        Check ($Arguments.PrecisionFactor -is [uint32] -and $Arguments.PrecisionFactor -eq 0) 'BitLocker conversion precision'
        return [pscustomobject]@{ ReturnValue = $script:ConversionReturn; ConversionStatus = $script:Conversion }
    }
    throw "Unexpected method: $MethodName"
}
function Reset-BitLocker {
    $script:CimCase = 'normal'; $script:ProtectionReturn = [uint32]0; $script:ConversionReturn = [uint32]0
    $script:Protection = [uint32]1; $script:Conversion = [uint32]1
}

# Execute the actual installer gate with fake confirmation and mutation callbacks.
$tokens = $null; $parseErrors = $null
$installAst = [Management.Automation.Language.Parser]::ParseFile((Join-Path $Installer 'install.ps1'), [ref]$tokens, [ref]$parseErrors)
Check ($parseErrors.Count -eq 0) 'installer parses'
$gates = @($installAst.FindAll({ param($n)
    $n -is [Management.Automation.Language.IfStatementAst] -and
    $n.Clauses[0].Item1.Extent.Text -eq 'Test-BitLockerConsentRequired $script:BitLockerState'
}, $true))
Check ($gates.Count -eq 2) 'plan and execution share the BitLocker consent rule'
$planGate = [scriptblock]::Create($gates[0].Extent.Text)
$bootGate = [scriptblock]::Create($gates[1].Extent.Text)
$bcdWrites = @($installAst.FindAll({ param($n)
    $n -is [Management.Automation.Language.CommandAst] -and $n.GetCommandName() -eq 'Invoke-Change' -and
    $n.Extent.Text -match "Invoke-Change 'bcdedit /set \{current\} testsigning on'"
}, $true))
Check ($bcdWrites.Count -eq 1 -and $gates[1].Extent.EndOffset -lt $bcdWrites[0].Extent.StartOffset) 'consent precedes boot mutation'
$refreshes = @($installAst.FindAll({ param($n)
    $n -is [Management.Automation.Language.AssignmentStatementAst] -and
    $n.Extent.Text -eq '$script:BitLockerState = Get-BitLockerState'
}, $true))
Check ($refreshes.Count -eq 1 -and $refreshes[0].Extent.EndOffset -lt $gates[1].Extent.StartOffset -and
    $refreshes[0].Extent.StartOffset -gt $gates[0].Extent.EndOffset) 'protection refreshed before the boot decision'
function Read-Confirmation { param($Question, $Answer) return $false }
function Exit-Engine { param($Code, $Outcome, $MessageId, $Detail) throw "fixture-exit:$Code`:$Outcome" }
function Invoke-Change {
    param($Description, [scriptblock]$Action)
    if (-not $script:FakeDryRun) { & $Action }
}
function Suspend-BitLocker {
    param($MountPoint, $RebootCount, $ErrorAction)
    $script:SuspendCalls++
    Check ($MountPoint -ceq 'Q:' -and $RebootCount -eq 2 -and $ErrorAction -eq 'Stop') 'suspension uses two restarts'
    if ($script:SuspendFails) { throw 'fixture-suspend-failed' }
    if (-not $script:SuspendUnverifiable) { Reset-BitLocker; $script:Protection = [uint32]0 }
}
function Set-StateValue { param($State, $Name, $Value) $State[$Name] = $Value }

$priorCulture = [Threading.Thread]::CurrentThread.CurrentCulture
$priorUiCulture = [Threading.Thread]::CurrentThread.CurrentUICulture
$priorDrive = $env:SystemDrive
$results = @()
$samples = @(
    @{ tag = 'en-US'; yes = 'Yes'; protection = 'Protection Status: Protection On' },
    @{ tag = 'pl-PL'; yes = 'Tak'; protection = 'Stan ochrony: Ochrona wlaczona' },
    @{ tag = 'es-ES'; yes = ('S' + [char]0x00ed); protection = ('Estado de protecci' + [char]0x00f3 + 'n: Activada') },
    @{ tag = 'ja-JP'; yes = ([string][char]0x306f + [char]0x3044); protection = ([string][char]0x4fdd + [char]0x8b77 + ': ' + [char]0x6709 + [char]0x52b9) },
    @{ tag = 'ko-KR'; yes = [string][char]0xc608; protection = ([string][char]0xbcf4 + [char]0xd638 + ': ' + [char]0xcf1c + [char]0xc9d0) }
)
try {
    $env:SystemDrive = 'Q:'
    foreach ($sample in $samples) {
        $startChecks = $script:Checks
        [Threading.Thread]::CurrentThread.CurrentCulture = [Globalization.CultureInfo]::GetCultureInfo($sample.tag)
        [Threading.Thread]::CurrentThread.CurrentUICulture = [Globalization.CultureInfo]::GetCultureInfo($sample.tag)
        $script:Admin = $true
        $script:BcdCase = 'true'
        Check ((Get-TestSigningConfigured) -ceq $true) "$($sample.tag) BCD true"
        $script:BcdCase = 'false'
        Check ((Get-TestSigningConfigured) -ceq $false) "$($sample.tag) BCD false"
        foreach ($case in @('absent', 'empty')) {
            $script:BcdCase = $case
            Check ((Get-TestSigningConfigured) -ceq $false) "$($sample.tag) BCD $case"
        }
        foreach ($case in @('store-failed', 'object-failed', 'enumerate-failed', 'malformed', 'duplicate', 'throw')) {
            $script:BcdCase = $case
            Check ($null -eq (Get-TestSigningConfigured)) "$($sample.tag) BCD $case is unknown"
        }
        $oldBcd = ("testsigning $($sample.yes)" -match '(?im)^\s*testsigning\s+Yes\s*$')
        $oldBitLocker = ($sample.protection -match '(?im)Protection Status:\s+Protection On')
        Check ($oldBcd -eq ($sample.tag -eq 'en-US')) "$($sample.tag) old BCD parser negative control"
        Check ($oldBitLocker -eq ($sample.tag -eq 'en-US')) "$($sample.tag) old BitLocker parser negative control"

        Reset-BitLocker
        Check ((Get-BitLockerState) -ceq 'on') "$($sample.tag) protected"
        $script:Protection = [uint32]0; $script:Conversion = [uint32]0
        Check ((Get-BitLockerState) -ceq 'off') "$($sample.tag) fully decrypted"
        $script:Conversion = [uint32]1
        Check ((Get-BitLockerState) -ceq 'suspended') "$($sample.tag) encrypted but unprotected"
        foreach ($conversion in @(2, 3, 4, 5, 99)) {
            $script:Conversion = [uint32]$conversion
            Check ((Get-BitLockerState) -ceq 'unknown') "$($sample.tag) conversion $conversion requires handling"
        }
        foreach ($protection in @(2, 99, $null)) {
            $script:Protection = $protection
            Check ((Get-BitLockerState) -ceq 'unknown') "$($sample.tag) unknown/invalid protection"
        }
        Reset-BitLocker; $script:ProtectionReturn = [uint32]5
        Check ((Get-BitLockerState) -ceq 'unknown') "$($sample.tag) protection method failure"
        $script:ProtectionReturn = $null
        Check ((Get-BitLockerState) -ceq 'unknown') "$($sample.tag) missing protection return code"
        Reset-BitLocker; $script:Protection = [uint32]0; $script:ConversionReturn = [uint32]5
        Check ((Get-BitLockerState) -ceq 'unknown') "$($sample.tag) conversion method failure"
        $script:ConversionReturn = $null
        Check ((Get-BitLockerState) -ceq 'unknown') "$($sample.tag) missing conversion return code"
        Reset-BitLocker; $script:Protection = [uint32]0; $script:Conversion = $null
        Check ((Get-BitLockerState) -ceq 'unknown') "$($sample.tag) missing conversion status"
        foreach ($case in @('throw', 'absent', 'duplicate', 'method-throws')) {
            Reset-BitLocker; $script:CimCase = $case
            Check ((Get-BitLockerState) -ceq 'unknown') "$($sample.tag) CIM $case"
        }
        $script:Admin = $false; $callsBefore = $script:ProviderCalls + $script:CimCalls
        Check ($null -eq (Get-TestSigningConfigured)) "$($sample.tag) non-admin BCD is unknown"
        Check ((Get-BitLockerState) -ceq 'unknown') "$($sample.tag) non-admin protection is unknown"
        Check (($script:ProviderCalls + $script:CimCalls) -eq $callsBefore) "$($sample.tag) non-admin makes no provider calls"
        $script:Admin = $true

        foreach ($status in @('on', 'unknown', 'unexpected', '', 'off', 'suspended')) {
            $script:BitLockerState = $status
            $consents = @()
            . $planGate
            Check (($consents -contains 'bitlocker') -eq ($status -notin @('off', 'suspended'))) "$($sample.tag) plan consent $status"
        }
        foreach ($status in @('on', 'unknown')) {
            $script:BitLockerState = $status; $BitLocker = ''; $state = @{}
            $stopped = $false
            try { . $bootGate } catch { $stopped = $_.Exception.Message -eq 'fixture-exit:4:needs-consent' }
            Check ($stopped -and $state.Count -eq 0) "$($sample.tag) execution refuses $status without consent"
            $BitLocker = 'HaveKey'
            . $bootGate
            Check ($state.bitlocker -ceq 'HaveKey') "$($sample.tag) recovery key decision accepted for $status"
        }
        $script:BitLockerState = 'unknown'; $BitLocker = 'Suspend'; $script:FakeDryRun = $false
        $script:SuspendCalls = 0; $script:SuspendFails = $false; $script:SuspendUnverifiable = $false; $state = @{}
        . $bootGate
        Check ($script:SuspendCalls -eq 1 -and $state.bitlocker -ceq 'Suspend') "$($sample.tag) verified suspension accepted"
        $script:SuspendFails = $true; $state = @{}; $stopped = $false
        try { . $bootGate } catch { $stopped = $_.Exception.Message -eq 'fixture-suspend-failed' }
        Check ($stopped -and $state.Count -eq 0) "$($sample.tag) failed suspension stops before boot change"
        $script:SuspendFails = $false; $script:SuspendUnverifiable = $true; Reset-BitLocker; $state = @{}; $stopped = $false
        try { . $bootGate } catch { $stopped = $_.Exception.Message -like 'BitLocker suspension could not be verified*' }
        Check ($stopped -and $state.Count -eq 0) "$($sample.tag) unverified suspension stops before boot change"
        $script:FakeDryRun = $true; $callsBefore = $script:SuspendCalls; $state = @{}
        . $bootGate
        Check ($script:SuspendCalls -eq $callsBefore) "$($sample.tag) dry run does not suspend"
        $results += [pscustomobject]@{ culture = $sample.tag; checks = $script:Checks - $startChecks; sample = 'synthetic'; old_parser_rejected_true = -not $oldBcd }
        Write-Output "PASS $($sample.tag): $($script:Checks - $startChecks) checks"
    }
    Check ($script:NativeCalls -eq 0) 'no native programs invoked'
    $result = [ordered]@{ schema = 1; ok = $true; checks = $script:Checks; osChanges = 0; nativePrograms = $script:NativeCalls; cultures = $results }
    if ($Out) {
        [void][IO.Directory]::CreateDirectory($Out)
        [IO.File]::WriteAllText((Join-Path $Out 'boot-locale-tests.json'), ($result | ConvertTo-Json -Depth 5), (New-Object Text.UTF8Encoding($false)))
    }
    Write-Output "PASS: $($script:Checks) checks, zero OS changes"
} finally {
    [Threading.Thread]::CurrentThread.CurrentCulture = $priorCulture
    [Threading.Thread]::CurrentThread.CurrentUICulture = $priorUiCulture
    $env:SystemDrive = $priorDrive
}
