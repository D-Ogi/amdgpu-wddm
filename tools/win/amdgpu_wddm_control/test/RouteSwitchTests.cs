// Host tests of the desktop route's kill switch and its two copies (BD-296, tools/release/installer/RouteSwitch.cs):
// the contract with the KMD, the router and the state catalog; the rule that no plan names one copy alone, over every
// action and every snapshot shape, with negative controls of the rule itself; the plans that repair a difference;
// the undo of a backup taken before BD-296; the state line; and RouteSwitch.Write against a scratch registry tree,
// including the refusal that leaves both copies as they were. A source scan proves that no other code of the app or
// the installer writes the value.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using AmdgpuWddm;
using AmdgpuWddmControl;
using Microsoft.Win32;

static partial class UnitTests
{
    static RecoverySnapshot RouteSnapshot(long? router, string driverCopy, long? driver = null)
    {
        var s = Open();
        s.DwmForceCpu = router;
        s.DriverRouteCopy = driverCopy;
        s.DwmForceCpuDriver = driver;
        return s;
    }

    static int RoutePairs(ActionPlan p) { return p.Writes.Count(w => Recovery.IsRouteWrite(w) && !RouteSwitch.IsDriverCopy(w.Path)); }

    static void RouteSwitchTests(string root)
    {
        Strings.Language = "en";
        const string name = RouteSwitch.ValueName;

        // ---- the contract with the KMD, the router and the state catalog --------------------------------------
        Equal(Recovery.RouterPath, RouteSwitch.RouterPath, "the router's copy is the router's key of the app");
        Equal(Recovery.ParametersPath, RouteSwitch.DriverPath, "the driver's copy is in the driver's Parameters key");
        Check(RouteSwitch.Paths.Length == 2 && RouteSwitch.Paths[0] == RouteSwitch.RouterPath && RouteSwitch.Paths[1] == RouteSwitch.DriverPath,
            "two copies, the router's first");
        var guard = File.ReadAllText(Path.Combine(root, @"driver\kmd\guard.c"));
        var read = Regex.Match(guard, @"void GuardReadDesktopKillSwitch\(.*?\n\}", RegexOptions.Singleline).Value;
        Check(read.Contains("IoOpenDriverRegistryKey(DriverObject, DriverRegKeyParameters, KEY_QUERY_VALUE") && read.Contains("L\"" + name + "\""),
            "the KMD reads its copy from its own Parameters key, read only, through IoOpenDriverRegistryKey");
        Check(!Regex.IsMatch(guard, @"\\\\Registry\\\\Machine\\\\SOFTWARE", RegexOptions.IgnoreCase), "the KMD opens no absolute path to the SOFTWARE hive");
        var router = File.ReadAllText(Path.Combine(root, @"driver\umd\router\router.cpp"));
        Check(router.Contains("L\"" + name + "\"") && router.Contains("amdgpu-wddm\\\\DesktopRouter"), "the router reads its copy from DesktopRouter");
        var catalog = File.ReadAllText(Path.Combine(root, @"driver\state\catalog.json"));
        Check(!catalog.Contains("\"" + name + "\""), name + " is not in the state catalog, so both copies are written raw");

        // ---- the allow-list and the raw route ----------------------------------------------------------------
        foreach (var path in RouteSwitch.Paths)
        {
            Check(Recovery.Allowed(path, name), "the allow-list has " + name + " at " + path);
            Check(!DriverStateKey.IsStateValue(path, name), name + " at " + path + " does not go through the state client");
        }
        Check(!Recovery.Allowed(@"SYSTEM\CurrentControlSet\Control\GraphicsDrivers", name), "the allow-list refuses the value under another key");
        Check(!RouteSwitch.Owns(RouteSwitch.DriverPath, "DwmForceCpuX") && !RouteSwitch.Owns(@"SOFTWARE\amdgpu-wddm", name), "Owns names the two values only");

        // ---- the pair rule and its negative controls ------------------------------------------------------------
        Check(Recovery.RoutePairError(Recovery.RouteWrites(1)) == null && Recovery.RoutePairError(Recovery.RouteWrites(null)) == null &&
            Recovery.RoutePairError(Recovery.RouteWrites(0)) == null, "RouteWrites gives a pair the rule admits");
        Check(Recovery.RoutePairError(new[] { RegWrite.Dword(RouteSwitch.RouterPath, name, 1) }) != null, "negative control: the router's copy alone is refused");
        Check(Recovery.RoutePairError(new[] { RegWrite.Dword(RouteSwitch.DriverPath, name, 1) }) != null, "negative control: the driver's copy alone is refused");
        Check(Recovery.RoutePairError(new[] { RegWrite.Dword(RouteSwitch.RouterPath, name, 1), RegWrite.Dword(RouteSwitch.DriverPath, name, 0) }) != null,
            "negative control: two copies with different values are refused");
        Check(Recovery.RoutePairError(new[] { RegWrite.Dword(RouteSwitch.RouterPath, name, 1), RegWrite.Remove(RouteSwitch.DriverPath, name) }) != null,
            "negative control: a write and a removal are refused");
        Check(Recovery.RoutePairError(new[] { RegWrite.Dword(RouteSwitch.DriverPath, name, 1), RegWrite.Dword(RouteSwitch.RouterPath, name, 1) }) != null,
            "negative control: the driver's copy before the router's is refused");
        Check(Recovery.RoutePairError(new[] { RegWrite.Dword(RouteSwitch.RouterPath, name, 1), RegWrite.Dword(Recovery.ParametersPath, "DpmMode", 1),
            RegWrite.Dword(RouteSwitch.DriverPath, name, 1) }) != null, "negative control: another write between the copies is refused");

        // Every action over every snapshot shape: a plan names both copies or neither, and only as a pair.
        var backups = new[]
        {
            new BackupRecord { File = "backup-20260105T100000000Z.json", Utc = "2026-01-05T10:00:00Z", Action = "desktop-cpu", Undoable = true, Args = new string[0],
                Values = new List<BackupValue> { new BackupValue { Path = RouteSwitch.RouterPath, Name = name, Existed = true, Kind = "DWord", Number = 0 } } },
        };
        var shapes = new List<RecoverySnapshot>();
        foreach (var r in new long?[] { null, 0, 1 })
            foreach (var d in new[] { null, "absent", "dword:0", "dword:1", "no-key", "type String" })
            {
                var s = d != null && d.StartsWith("dword:") ? RouteSnapshot(r, "dword", long.Parse(d.Substring(6))) : RouteSnapshot(r, d);
                s.DefaultRouter = new Dictionary<string, long> { { name, 0 } };
                foreach (var route in new[] { "gpu", "cpu" }) { var t = RouteSnapshot(r, s.DriverRouteCopy, s.DwmForceCpuDriver); t.DefaultRouter = s.DefaultRouter; t.DwmRoute = route; shapes.Add(t); }
            }
        shapes.Add(Closed());
        int planned = 0, withRoute = 0;
        foreach (var s in shapes)
            foreach (var a in Recovery.Actions)
            {
                ActionPlan p;
                try { p = Recovery.Plan(a, s, 1, 1600, backups, true); }
                catch (Exception) { continue; }        // an action this snapshot cannot plan has no writes to judge
                planned++;
                if (Recovery.RoutePairError(p.Writes) != null) Check(false, a + ": " + Recovery.RoutePairError(p.Writes));
                int routerCopies = p.Writes.Count(w => Recovery.IsRouteWrite(w) && !RouteSwitch.IsDriverCopy(w.Path));
                int driverCopies = p.Writes.Count(w => Recovery.IsRouteWrite(w) && RouteSwitch.IsDriverCopy(w.Path));
                if (routerCopies != driverCopies) Check(false, a + " plans " + routerCopies + " router and " + driverCopies + " driver copies");
                if (routerCopies > 1) Check(false, a + " plans the switch more than once");
                if (routerCopies > 0) withRoute++;
            }
        Check(planned > 100 && withRoute > 10, "the sweep planned " + planned + " actions, " + withRoute + " of them with the switch (positive control)");
        Check(shapes.Any(s => { var p = Recovery.Plan("desktop-cpu", s); return !p.Refused && RoutePairs(p) == 1; }), "the sweep reaches a desktop-cpu write");

        // ---- the plans --------------------------------------------------------------------------------------------
        // desktop-cpu with both copies on the CPU route and DWM on it: refused as before.
        var both = RouteSnapshot(1, "dword", 1); both.DwmRoute = "cpu";
        Check(Recovery.Plan("desktop-cpu", both).Refusal.Contains("already runs on the CPU route"), "desktop-cpu: both copies agree, refused");
        // The driver's copy differs: the plan writes both, and says why.
        var drift = RouteSnapshot(1, "absent"); drift.DwmRoute = "cpu";
        var plan = Recovery.Plan("desktop-cpu", drift);
        Check(!plan.Refused && Writes(plan, name + "=1", name + "=1") && plan.Writes[0].Path == RouteSwitch.RouterPath && plan.Writes[1].Path == RouteSwitch.DriverPath,
            "desktop-cpu: the router's copy on, the driver's absent: both written");
        Check(plan.Notes.Any(n => n.Contains("driver's copy (absent) differs")), "desktop-cpu: the note names the copy that differs");
        Check(plan.Text().Contains(@"set HKLM\" + RouteSwitch.DriverPath + " " + name + " = 1 (DWord)"), "the plan text names the driver's copy");
        // An older recorded snapshot did not read the driver's copy: judged by the router's copy alone.
        var old = RouteSnapshot(1, null); old.DwmRoute = "cpu";
        Check(Recovery.Plan("desktop-cpu", old).Refusal.Contains("already runs on the CPU route"), "an older snapshot: refused on the router's copy, as before");
        plan = Recovery.Plan("desktop-gpu", RouteSnapshot(1, "dword", 1));
        Check(!plan.Refused && Writes(plan, name + "=0", name + "=0"), "desktop-gpu: both copies to 0");
        var gpuDrift = RouteSnapshot(0, "dword", 1); gpuDrift.DwmRoute = "gpu";
        plan = Recovery.Plan("desktop-gpu", gpuDrift);
        Check(!plan.Refused && Writes(plan, name + "=0", name + "=0"), "desktop-gpu: the driver's copy still on: both written");

        // reset-defaults: both copies, the Change line names the switch once.
        var reset = WithDefaults(); reset.DwmForceCpu = 1; reset.DriverRouteCopy = "dword"; reset.DwmForceCpuDriver = 1;
        plan = Recovery.Plan("reset-defaults", reset);
        Check(!plan.Refused && RoutePairs(plan) == 1 && Recovery.RoutePairError(plan.Writes) == null, "reset-defaults: one pair");
        Check(Regex.Matches(plan.Change, name).Count == 1, "reset-defaults: the change names " + name + " once: " + plan.Change);

        // undo: a backup of a release before BD-296 holds the router's copy only; the undo restores both.
        plan = Recovery.Plan("undo", Open(), null, null, backups);
        Check(!plan.Refused && Writes(plan, name + "=0", name + "=0") && Recovery.RoutePairError(plan.Writes) == null, "undo of a pre-BD-296 backup: both copies");
        var absent = new BackupRecord { File = "backup-20260105T110000000Z.json", Utc = "2026-01-05T11:00:00Z", Action = "desktop-cpu", Undoable = true, Args = new string[0],
            Values = new List<BackupValue> { new BackupValue { Path = RouteSwitch.RouterPath, Name = name, Existed = false }, new BackupValue { Path = RouteSwitch.DriverPath, Name = name, Existed = true, Kind = "DWord", Number = 1 } } };
        plan = Recovery.Plan("undo", Open(), null, null, new[] { absent });
        Check(!plan.Refused && Writes(plan, name + "-", name + "-"), "undo: the router's copy was absent, so both copies are removed");
        Check(Recovery.RouteBackupValue(new[] { new BackupValue { Path = RouteSwitch.RouterPath, Name = name, Existed = true, Kind = "String", Text = "0" } }) == 1,
            "a router copy of another type comes back as the CPU route");
        Check(Recovery.RouteBackupValue(new[] { new BackupValue { Path = RouteSwitch.DriverPath, Name = name, Existed = true, Kind = "DWord", Number = 1 } }) == 1,
            "a backup with the driver's copy only gives its value");

        // ---- the state line (condition 3) ------------------------------------------------------------------------
        Func<RecoverySnapshot, StateLine> switchLine = x => Recovery.Describe(x).FirstOrDefault(l => l.Topic == "Desktop route switch");
        var l1 = switchLine(RouteSnapshot(1, "absent"));
        Check(l1 != null && l1.Severity == "warn" && l1.Action == "desktop-cpu" && l1.Text.Contains("router copy 1, driver copy absent") && l1.Text.Contains("differ"),
            "the state line shows both copies and flags the difference");
        var l2 = switchLine(RouteSnapshot(0, "dword", 1));
        Check(l2 != null && l2.Severity == "warn" && l2.Action == "desktop-gpu", "a driver copy on under a GPU route: the repair is desktop-gpu");
        var l3 = switchLine(RouteSnapshot(1, "dword", 1));
        Check(l3 != null && l3.Severity == "ok" && l3.Action == null && l3.Text.Contains("router copy 1, driver copy 1"), "both copies agree: ok");
        Check(switchLine(RouteSnapshot(null, "absent")).Severity == "ok", "both absent agree");
        Check(switchLine(RouteSnapshot(1, null)) == null, "an older snapshot: no line");

        // ---- RouteSwitch.Write against a scratch tree -------------------------------------------------------------
        string scratch = @"Software\amdgpu-wddm-test-route-" + Guid.NewGuid().ToString("N");
        try
        {
            using (var hk = Registry.CurrentUser.CreateSubKey(scratch))
            {
                using (hk.CreateSubKey(RouteSwitch.RouterPath)) { }
                // The driver's key is missing: refused before anything is written, the router's copy stays.
                using (var r = hk.OpenSubKey(RouteSwitch.RouterPath, true)) r.SetValue(name, 0, RegistryValueKind.DWord);
                bool refused = false;
                try { RouteSwitch.Write(hk, 1); } catch (InvalidOperationException e) { refused = e.Message.Contains("neither copy"); }
                var c = RouteSwitch.Read(hk);
                Check(refused && c.Router.Dword && c.Router.Number == 0 && !c.DriverKey, "Write without the driver's key: refused, the router's copy unchanged");
                using (hk.CreateSubKey(RouteSwitch.DriverPath)) { }
                Check(RouteSwitch.Read(hk).Mismatch, "router 0, driver absent: a mismatch");
                string line = RouteSwitch.Write(hk, 1);
                c = RouteSwitch.Read(hk);
                Check(c.Router.Dword && c.Router.Number == 1 && c.Driver.Dword && c.Driver.Number == 1 && !c.Mismatch && line.Contains("in both copies"), "Write 1: both copies 1");
                RouteSwitch.Write(hk, null);
                c = RouteSwitch.Read(hk);
                Check(!c.Router.Present && !c.Driver.Present && !c.Mismatch, "Write null: both copies removed");
                using (var r = hk.OpenSubKey(RouteSwitch.RouterPath, true)) r.SetValue(name, "1", RegistryValueKind.String);
                c = RouteSwitch.Read(hk);
                Check(c.Router.Present && !c.Router.Dword && c.Router.Cpu && c.Router.Value == 1 && c.Mismatch, "a string in the router's copy is the CPU route, and differs");
                RouteSwitch.Write(hk, 0);
                c = RouteSwitch.Read(hk);
                Check(c.Router.Dword && c.Router.Number == 0 && c.Driver.Dword && c.Driver.Number == 0, "Write 0 over a string: both DWORD 0");
            }
        }
        finally { Registry.CurrentUser.DeleteSubKeyTree(scratch, false); }

        // ---- no other writer (source scan) ----------------------------------------------------------------------------
        // In the app: the only SetValue or DeleteValue of the name is in RouteSwitch.cs, and RecoveryActions.Write
        // refuses a route write that does not come through RouteSwitch.Write.
        var src = Path.Combine(root, @"tools\win\amdgpu_wddm_control\src");
        foreach (var f in Directory.GetFiles(src, "*.cs"))
        {
            var text = Regex.Replace(File.ReadAllText(f), @"//[^\n]*", "");
            Check(!Regex.IsMatch(text, @"(SetValue|DeleteValue)\(\s*""" + name + @""""), Path.GetFileName(f) + " writes no copy of " + name + " itself");
        }
        var actions = File.ReadAllText(Path.Combine(src, "RecoveryActions.cs"));
        Check(actions.Contains("if (Recovery.IsRouteWrite(w)) throw new InvalidOperationException"), "the helper's generic write refuses a copy of the switch");
        Check(actions.Contains("Recovery.RoutePairError(writes)") && actions.Contains("RouteSwitch.Write(SettingsStore.Machine"), "Apply checks the pair rule and writes through RouteSwitch.Write");
        // In the installer: the router's table write leaves the name out, and the step that writes it calls RouteSwitch.
        var installer = Path.Combine(root, @"tools\release\installer");
        foreach (var f in Directory.GetFiles(installer, "*.ps1"))
        {
            var text = Regex.Replace(File.ReadAllText(f), @"#[^\n]*", "");
            Check(!Regex.IsMatch(text, @"(New-ItemProperty|Set-ItemProperty)[^\n]*" + name), Path.GetFileName(f) + " writes no copy of " + name + " with a cmdlet");
        }
        var install = File.ReadAllText(Path.Combine(installer, "install.ps1"));
        Check(install.Contains("Write-RouteSwitch") && Regex.IsMatch(install, @"Where-Object \{ \$_\.name -ne 'DwmForceCpu' \}"), "install.ps1 writes the switch through Write-RouteSwitch only");
        var common = File.ReadAllText(Path.Combine(installer, "common.ps1"));
        Check(common.Contains("Add-Type -Path (Join-Path $PSScriptRoot 'RouteSwitch.cs')") && common.Contains("[AmdgpuWddm.RouteSwitch]::Write("), "the installer compiles RouteSwitch.cs and calls Write");
        var uninstall = File.ReadAllText(Path.Combine(installer, "uninstall.ps1"));
        Check(Regex.IsMatch(uninstall, @"Remove-ItemProperty -LiteralPath \$script:ParametersKey -Name DwmForceCpu"), "the uninstaller removes the driver's copy");
    }
}
