# Host test of the stick steering functions of listener.ps1 (Usb-Signed, Usb-Default, Usb-SetSignedDefault).
# No stick and no lab: the functions are taken from listener.ps1 through the parser (the listener itself is not
# started) and run on a directory tree under -Work that has the stick's file layout.
# Usage: usb-steer.test.ps1 -Work <new empty directory>
param([Parameter(Mandatory)][string]$Work)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\stick-env.ps1"
$errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile("$PSScriptRoot\listener.ps1", [ref]$null, [ref]$errors)
if ($errors.Count) { throw "listener.ps1 has $($errors.Count) parse errors" }
foreach ($name in 'Usb-Signed', 'Usb-Default', 'Usb-SetSignedDefault') {
    $f = $ast.Find({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq $name }, $true)
    if (-not $f) { throw "listener.ps1 has no function $name" }
    . ([scriptblock]::Create($f.Extent.Text))
}
$script:fail = 0
function Check([string]$What, [bool]$Ok) { if ($Ok) { "  PASS $What" } else { "  FAIL $What"; $script:fail++ } }
function Sha([string]$p) { (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash }

if (Get-ChildItem -LiteralPath $Work -Force) { throw '-Work must be an empty directory' }
$grubDir = Join-Path $Work 'boot\grub'
New-Item -ItemType Directory -Force $grubDir | Out-Null
$u = [ordered]@{ root = $Work; grub = Join-Path $grubDir 'grub.cfg'; env = Join-Path $grubDir 'grubenv' }
[IO.File]::WriteAllText($u.grub, "set timeout=5`nset default=6`n")
[IO.File]::WriteAllBytes($u.env, (Format-StickEnv ([ordered]@{ bc250_try = '1' })))

Check 'a stick without grub.cfg.sig is not signed' (-not (Usb-Signed $u))
Check 'unsigned: the default comes from grub.cfg' ((Usb-Default $u) -eq '6')

[IO.File]::WriteAllText($u.grub + '.sig', 'x')
$cfgHash = Sha $u.grub
Check 'a stick with grub.cfg.sig is signed' (Usb-Signed $u)
Check 'signed, no variable: the default reads as Windows' ((Usb-Default $u) -like 'windows*')
Usb-SetSignedDefault $u '4'
Check 'signed: linux:4 is written into grubenv' ((Usb-Default $u) -eq '4')
Check 'signed: grub.cfg is not changed' ((Sha $u.grub) -eq $cfgHash)
Check 'signed: the X3 counter line survives' ((Read-StickEnv ([IO.File]::ReadAllBytes($u.env)))['bc250_try'] -eq '1')
Check 'the first block is kept as grubenv.orig' (Test-Path -LiteralPath ($u.env + '.orig'))
$threw = $false
try { Usb-SetSignedDefault $u '8' } catch { $threw = $true }
Check 'signed: an entry outside 0 to 5 is refused' $threw
Check 'signed: a refused value leaves the old one' ((Usb-Default $u) -eq '4')
Usb-SetSignedDefault $u ''
Check 'signed: windows removes the variable' ((Usb-Default $u) -like 'windows*')
Check 'the block is 1024 bytes after the changes' ((Get-Item -LiteralPath $u.env).Length -eq 1024)
Check 'the block equals grubenv.orig again' ((Sha $u.env) -eq (Sha ($u.env + '.orig')))

if ($script:fail) { "USB-STEER TEST: $($script:fail) FAIL"; exit 1 }
'USB-STEER TEST: ALL PASS'
