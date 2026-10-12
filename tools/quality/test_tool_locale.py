"""Offline locale regression tests for lab tooling. No OS inventory or settings calls.

Culture tags and translated pnputil labels below are synthetic fixtures, not captured
Windows output. Production PowerShell functions and C# calculations run against fakes.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import unittest
import uuid

ROOT = Path(__file__).resolve().parents[2]
WIN = ROOT / "tools/win"


def text(path):
    return path.read_text(encoding="utf-8-sig")


class ToolLocaleTests(unittest.TestCase):
    def test_english_counter_copies_are_identical(self):
        copies = []
        for name in ("lab-runner/kmdlog-stream.ps1", "cpupower/probe-cpu-power.ps1"):
            source = text(WIN / name)
            copies.append(re.search(r"function Initialize-EnglishCounter \{.*?\n'@\n\}", source, re.S).group())
        self.assertEqual(*copies)
        self.assertIn("PdhAddEnglishCounterW", copies[0])
        self.assertNotIn("PdhAddCounterW", copies[0])
        self.assertIn("0x200 | 0x8000", copies[0])  # double, without a 100% cap

    def test_no_localized_counter_or_account_lookup(self):
        for name in ("bc250mon/src/Providers.cs", "lab-runner/kmdlog-stream.ps1",
                     "lab-runner/game-runtime.ps1", "cpupower/probe-cpu-power.ps1"):
            with self.subTest(file=name):
                source = text(WIN / name)
                self.assertNotRegex(source, r"(?:new|New-Object)\s+(?:Diagnostics\.)?PerformanceCounter\b")
                self.assertNotRegex(source, r"(?m)^\s*\$\w+\s*=\s*Get-Counter\b")
        install = text(WIN / "lab-emerg/install.ps1")
        listener = text(WIN / "lab-emerg/listener.ps1")
        self.assertNotIn("'Administrators", install + listener)
        self.assertIn("-UserId 'S-1-5-18'", install)
        for name in ("bc250mon/test-telemetry.ps1", "bc250mon/test-lab-state.ps1"):
            self.assertIn("'SystemMetrics.cs'", text(WIN / name))

    def test_train_inventories_share_structured_contract(self):
        copies = []
        for name in ("clean-slate.ps1", "slots.ps1"):
            source = text(WIN / "train-validate/lab" / name)
            self.assertNotRegex(source, r"Published Name:|Original Name:|Driver Version:")
            copies.append(re.search(r"function Get-KmdDriverStorePackages \{.*?^\}", source, re.S | re.M).group())
        self.assertEqual(*copies)
        self.assertIn("Get-WindowsDriver -Online -ErrorAction Stop", copies[0])

    @unittest.skipUnless(os.name == "nt", "executes Windows PowerShell with fake native APIs")
    def test_five_cultures_with_mocked_apis(self):
        ps = shutil.which("powershell.exe")
        self.assertIsNotNone(ps, "Windows PowerShell 5.1 is required")
        # All compiler/temp output remains next to this isolated worktree, never on C:.
        temp_root = Path(os.environ.get("BC250_TEST_OUT", str(ROOT.parent / "tmp")))
        temp_root.mkdir(parents=True, exist_ok=True)
        tmp = temp_root / ("tool-locale-" + uuid.uuid4().hex)
        tmp.mkdir()  # Retain the small fixture/compiler output for a failed gate.
        script = tmp / "check.ps1"
        script.write_text(POWERSHELL, encoding="utf-8-sig")
        env = dict(os.environ, TEMP=str(tmp), TMP=str(tmp))
        run = subprocess.run([ps, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(script),
                              "-Root", str(ROOT)], env=env, capture_output=True, text=True, timeout=90)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        for culture in ("en-US", "pl-PL", "es-ES", "ja-JP", "ko-KR"):
            self.assertIn("PASS " + culture, run.stdout)


POWERSHELL = r'''
param([string]$Root)
$ErrorActionPreference='Stop'
function Check([bool]$Ok,[string]$What) { if(!$Ok){throw $What} }
function Refuses([scriptblock]$Action,[string]$What) {
 $refused=$false;try{& $Action}catch{$refused=$true};Check $refused $What
}
# Import only named function declarations. Never dot-source any executable script body.
function Import-Function([string]$Relative,[string]$Name) {
 $tokens=$null;$errors=$null
 $ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $Root $Relative),[ref]$tokens,[ref]$errors)
 Check (!$errors.Count) ('Parse errors: '+$Relative)
 $fn=@($ast.FindAll({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq $Name},$true))
 Check ($fn.Count -eq 1) ('Missing/duplicate function '+$Name)
 . ([scriptblock]::Create($fn[0].Extent.Text.Replace('function '+$Name,'function global:'+$Name)))
}
Import-Function 'tools/win/lab-runner/kmdlog-stream.ps1' 'Initialize-EnglishCounter'
Import-Function 'tools/win/lab-runner/game-runtime.ps1' 'Get-AvailableMemoryMiB'
Import-Function 'tools/win/lab-emerg/install.ps1' 'Protect-EmergencyKey'
Import-Function 'tools/win/lab-emerg/listener.ps1' 'Owner-Admins'
Import-Function 'tools/win/lab-emerg/listener.ps1' 'Acl'
Import-Function 'tools/win/kmd-deploy/ops/attempt-result.ps1' 'Select-DriverStoreRecords'
Add-Type -Path (Join-Path $Root 'tools/win/bc250mon/src/SystemMetrics.cs')
# Fake calls record their arguments. No PDH, kernel, CIM, ACL or driver-store call runs.
$productionType=[regex]::Match((Get-Command Initialize-EnglishCounter).Definition,"(?s)-TypeDefinition @'\r?\n(.*?)\r?\n'@").Groups[1].Value
Check ($productionType.Length -gt 100) 'Production counter type missing'
$fakeType=@'
public sealed class FakeCounterApi : Bc250Tools.ICounterApi {
 public uint OpenStatus, AddStatus, CollectStatus, ReadStatus, DataStatus;
 public double Value=137.5;
 public int Opens, Collects, Reads, Closes;
 public string Path;
 public uint Open(out IntPtr q) { Opens++; q=new IntPtr(1); return OpenStatus; }
 public uint Add(IntPtr q,string path,out IntPtr c) { Path=path;c=new IntPtr(2);return AddStatus; }
 public uint Collect(IntPtr q) { Collects++;return CollectStatus; }
 public uint Read(IntPtr c,out Bc250Tools.CounterValue v) { Reads++;v=new Bc250Tools.CounterValue{Status=DataStatus,Value=Value};return ReadStatus; }
 public void Close(IntPtr q) { Closes++; }
}
'@
Add-Type -TypeDefinition ($productionType+"`n"+$fakeType)
Check ([Runtime.InteropServices.Marshal]::SizeOf([type][Bc250Tools.CounterValue]) -eq 16) 'PDH_FMT_COUNTERVALUE size'
Check ([Runtime.InteropServices.Marshal]::OffsetOf([type][Bc250Tools.CounterValue],'Value').ToInt32() -eq 8) 'PDH double union offset'
Check ([Runtime.InteropServices.Marshal]::SizeOf([type][Bc250Mon.SystemMetrics].Assembly.GetType('Bc250Mon.SystemMetrics+MemoryStatus')) -eq 64) 'MEMORYSTATUSEX size'
function global:Get-CimInstance {
 param($ClassName,$Property,$OperationTimeoutSec,$ErrorAction)
 Check ($ClassName -eq 'Win32_OperatingSystem' -and $Property -eq 'FreePhysicalMemory' -and $OperationTimeoutSec -eq 5) 'CIM query contract'
 if($script:CimFail){throw 'synthetic CIM refusal'}
 [pscustomobject]@{FreePhysicalMemory=$script:FreeMemory}
}
function global:icacls.exe {
 $script:AclArgs=@($args);$global:LASTEXITCODE=$script:AclExit
 $script:AclText
}
function global:Get-WindowsDriver {
 param([switch]$Online,$ErrorAction)
 Check ($Online -and $ErrorAction -eq 'Stop') 'Driver inventory query contract'
 if($script:DriverStoreFail){throw 'synthetic driver inventory refusal'}
 $script:DriverPackages
}
$fixtures=@{
 'en-US'=@('Published Name','Original Name','Driver Version','Administrators')
 'pl-PL'=@('Nazwa opublikowana','Nazwa oryginalna','Wersja sterownika','Administratorzy')
 'es-ES'=@('Nombre publicado','Nombre original','Versión del controlador','Administradores')
 'ja-JP'=@('公開名','元の名前','ドライバーのバージョン','管理者')
 'ko-KR'=@('게시된 이름','원래 이름','드라이버 버전','관리자')
}
foreach($culture in @('en-US','pl-PL','es-ES','ja-JP','ko-KR')) {
 [Threading.Thread]::CurrentThread.CurrentCulture=[Globalization.CultureInfo]::GetCultureInfo($culture)
 [Threading.Thread]::CurrentThread.CurrentUICulture=[Globalization.CultureInfo]::GetCultureInfo($culture)
 $native=New-Object FakeCounterApi
 $counter=[Bc250Tools.EnglishCounter]::new('\Processor Information(_Total)\% Processor Performance',$native)
 Check ($native.Path -ceq '\Processor Information(_Total)\% Processor Performance') 'English path changed'
 Check ($native.Collects -eq 1 -and $native.Reads -eq 0) 'Rate counter not primed'
 Check ($counter.NextValue() -eq 137.5 -and $native.Collects -eq 2) 'Uncapped rate'
 $native.DataStatus=1;Check ($counter.NextValue() -eq 137.5) 'New data status'
 $native.DataStatus=2147485653;Refuses {$counter.NextValue()} 'Invalid data became a reading'
 $native.DataStatus=0;$native.Value=[double]::NaN;Refuses {$counter.NextValue()} 'NaN became a reading'
 $native.Value=[double]::PositiveInfinity;Refuses {$counter.NextValue()} 'Infinity became a reading'
 $native.Value=0;Check ($counter.NextValue() -eq 0) 'Genuine zero lost'
 $native.CollectStatus=5;Refuses {$counter.NextValue()} 'Collect error ignored';$native.CollectStatus=0
 $native.ReadStatus=5;Refuses {$counter.NextValue()} 'Read error ignored';$native.ReadStatus=0
 $counter.Dispose();$counter.Dispose();Check ($native.Closes -eq 1) 'Query not closed exactly once'
 Refuses {$counter.NextValue()} 'Disposed query read'
 foreach($step in @('AddStatus','CollectStatus')) {
  $n=New-Object FakeCounterApi;$n.$step=5
  Refuses {[Bc250Tools.EnglishCounter]::new('\Memory\Pages Input/sec',$n)} ('Constructor '+$step)
  Check ($n.Closes -eq 1) 'Failed constructor leaked query'
 }
 $before=[Bc250Mon.SystemCpuTimes]::new(100,200,300)
 $after=[Bc250Mon.SystemCpuTimes]::new(150,300,400)
 Check ([Bc250Mon.SystemMetrics]::CpuPercent($before,$after) -eq 75) 'Idle counted as busy'
 Check ($null -eq [Bc250Mon.SystemMetrics]::CpuPercent($before,$before)) 'Zero interval fabricated'
 Check ($null -eq [Bc250Mon.SystemMetrics]::CpuPercent($after,$before)) 'Reversed times fabricated'
 Check ($null -eq [Bc250Mon.SystemMetrics]::CpuPercent($before,[Bc250Mon.SystemCpuTimes]::new(1000,201,301))) 'Impossible idle accepted'
 $script:CimFail=$false;$script:FreeMemory=[uint64]3585024
 Check ((Get-AvailableMemoryMiB) -eq 3501) 'KiB conversion'
 $script:FreeMemory=[uint64]3584512;Check ((Get-AvailableMemoryMiB) -eq 3500) 'Floor at threshold'
 $script:FreeMemory=$null;Refuses {Get-AvailableMemoryMiB} 'Missing memory became zero'
 $script:CimFail=$true;Refuses {Get-AvailableMemoryMiB} 'CIM error ignored'
 $script:AclExit=0;$script:AclText=$fixtures[$culture][3]
 Protect-EmergencyKey 'synthetic-key.bin'
 Check (($script:AclArgs -join '|') -ceq 'synthetic-key.bin|/inheritance:r|/grant:r|*S-1-5-18:F|*S-1-5-32-544:F') 'Key ACL depends on account name'
 Owner-Admins 'synthetic-file'
 Check (($script:AclArgs -join '|') -ceq 'synthetic-file|/setowner|*S-1-5-32-544') 'Owner depends on account name'
 Check ((Acl 'synthetic-file') -ceq $script:AclText) 'ACL diagnostic changed'
 $script:AclExit=5
 Refuses {Protect-EmergencyKey 'synthetic-key.bin'} 'Key ACL failure ignored'
 Refuses {Owner-Admins 'synthetic-file'} 'Owner failure ignored'
 Refuses {Acl 'synthetic-file'} 'ACL read failure ignored'
 $labels=$fixtures[$culture]
 $records=@('synthetic header','',($labels[0]+': oem42.inf'),($labels[1]+': bc250kmd.inf'),($labels[2]+': 10/10/2026 0.7.216.100'),'')
 $selected=@(Select-DriverStoreRecords $records)
 Check ($selected.Count -eq 1 -and $selected[0].Contains('bc250kmd.inf') -and $selected[0].Contains('0.7.216.100')) 'Localized driver record lost detail'
 Check (@(Select-DriverStoreRecords @('not-oem42.inf-more')).Count -eq 0) 'Filename boundary ignored'
 $many=1..6 | ForEach-Object { 'name: oem'+$_+'.inf'; 'version: 1.0'; '' }
 $tail=@(Select-DriverStoreRecords $many)
 Check ($tail.Count -eq 4 -and $tail[0].Contains('oem3.inf') -and $tail[3].Contains('oem6.inf')) 'Driver report bound'
 foreach($trainFile in @('clean-slate.ps1','slots.ps1')) {
  Import-Function ('tools/win/train-validate/lab/'+$trainFile) 'Get-KmdDriverStorePackages'
  $script:DriverStoreFail=$false
  $script:DriverPackages=@(
   [pscustomobject]@{Driver='oem42.inf';OriginalFileName=('C:\synthetic\'+$labels[0]+'\BC250KMD.INF');Version=[version]'0.7.216.100';Date=[datetime]::new(2026,10,10);ProviderName=$labels[3]},
   [pscustomobject]@{Driver='oem43.inf';OriginalFileName='C:\synthetic\unrelated.inf';Version='1.2.3.4';Date=[datetime]::new(2026,9,1)})
  $store=@(Get-KmdDriverStorePackages)
  Check ($store.Count -eq 1 -and $store[0].Published -ceq 'oem42.inf' -and $store[0].Version -ceq '0.7.216.100' -and $store[0].Date -ceq '2026-10-10') 'Structured train package inventory'
  $script:DriverPackages=@();Check (@(Get-KmdDriverStorePackages).Count -eq 0) 'Genuine empty driver inventory'
  $script:DriverStoreFail=$true;Refuses {Get-KmdDriverStorePackages} 'Failed driver query reported empty';$script:DriverStoreFail=$false
  $script:DriverPackages=@([pscustomobject]@{Driver='oem42.inf'})
  Refuses {Get-KmdDriverStorePackages} 'Unreadable original INF reported empty'
  $script:DriverPackages=@([pscustomobject]@{Driver='oem42.inf';OriginalFileName='bc250kmd.inf';Version='0.7.216.100';Date='10/10/2026'})
  Refuses {Get-KmdDriverStorePackages} 'Localized date accepted as structured date'
  $script:DriverPackages[0].Date=[datetime]::new(2026,10,10);$script:DriverPackages[0].Version=$null
  Refuses {Get-KmdDriverStorePackages} 'Missing version reported complete'
  $script:DriverPackages[0].Version='0.7.216.100';$script:DriverPackages[0].Driver='..\oem42.inf'
  Refuses {Get-KmdDriverStorePackages} 'Invalid published INF reported complete'
 }
 'PASS '+$culture
}
'''


if __name__ == "__main__":
    unittest.main()
