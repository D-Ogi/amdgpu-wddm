// The Recovery page's I/O: the snapshot the rules of Recovery.cs look at, and the elevated helper that executes one
// plan. The helper is this exe started with --action <name> (one UAC prompt per action); it plans again from its own
// snapshot, so the window's earlier check is never trusted, and then:
//   1. saves the old value of everything it will write to %ProgramData%\amdgpu-wddm\control\backup-<utc>.json,
//   2. writes, logging each step to control-actions.log in the same directory,
//   3. reads every value back and reports the result (a failed write restores the backup at once),
//   4. for a desktop route change restarts DWM, and onto the GPU route watches it for 60 s: on a crash
//      (Application Error 1000, dwm.exe) or a replaced DWM it sets DwmForceCpu 1 and restarts DWM again.
// --dry-run prints the states and the plan and writes nothing; --snapshot <json> (dry run only) plans from a
// recorded snapshot instead of this PC.
//
// Exit codes: 0 done, 1 failed, 2 usage, 3 refused, 4 desktop fell back to the CPU route, 5 needs administrator.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Diagnostics.Eventing.Reader;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Security.AccessControl;
using System.Security.Principal;
using System.Text;
using System.Threading;
using System.Web.Script.Serialization;
using Microsoft.Win32;

namespace AmdgpuWddmControl
{
    public static class RecoveryProbe
    {
        public const string TaskName = "amdgpu-wddm start confirm";     // the release installer's logon task

        public static string ControlDirectory
        {
            get { return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "amdgpu-wddm", "control"); }
        }

        public static string ActionsLog { get { return Path.Combine(ControlDirectory, "control-actions.log"); } }

        public static RecoverySnapshot Read()
        {
            var s = new RecoverySnapshot();
            using (var k = Registry.LocalMachine.OpenSubKey(Recovery.ParametersPath))
            {
                s.DriverInstalled = k != null;
                if (k != null)
                    foreach (var name in k.GetValueNames())
                        if (k.GetValueKind(name) == RegistryValueKind.DWord) s.Parameters[name] = (uint)(int)k.GetValue(name);
            }
            using (var k = Registry.LocalMachine.OpenSubKey(Recovery.RouterPath))
            {
                s.RouterInstalled = k != null;
                if (k != null)
                {
                    s.DwmForceCpu = Dword(k, "DwmForceCpu");
                    s.RequireKmdSwitches = Dword(k, "RequireKmdSwitches");
                }
            }
            var interop = Kmd.Interop();
            var health = Kmd.StartHealth();
            var dpm = Kmd.Dpm();
            s.Interop = interop.Value; s.Health = health.Value; s.Dpm = dpm.Value;
            if (s.Interop == null && s.Health == null && s.Dpm == null) s.DriverError = dpm.Error ?? interop.Error ?? "no answer";

            string installDir = "";
            using (var k = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\amdgpu-wddm\Release"))
                if (k != null) installDir = Convert.ToString(k.GetValue("InstallDir") ?? "");
            s.GpuDesktopModules = installDir.Length > 0 &&
                new[] { "bc250d3d_router.dll", "bc250d3d_zink.dll", "amdgpu_wddm_radv.dll" }.All(f => File.Exists(Path.Combine(installDir, "desktop", f)));
            ReadDefaults(s, installDir);

            string error;
            var modules = DwmModules(out error);
            if (error != null) s.DwmRouteDetail = error;
            else
            {
                s.DwmRoute = Recovery.RouteFromModules(modules);
                s.DwmRouteDetail = string.Join(", ", modules.Select(Path.GetFileName).Where(f => f.StartsWith("bc250", StringComparison.OrdinalIgnoreCase) ||
                    f.StartsWith("amdgpu_wddm", StringComparison.OrdinalIgnoreCase)).Distinct(StringComparer.OrdinalIgnoreCase));
            }
            ReadTask(s);
            s.ConfirmLogLast = LastLine(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "amdgpu-wddm", "start-confirm.log"));
            return s;
        }

        static long? Dword(RegistryKey k, string name)
        {
            var v = k.GetValue(name);
            return v is int ? (long?)(uint)(int)v : null;
        }

        static void ReadDefaults(RecoverySnapshot s, string installDir)
        {
            if (installDir.Length == 0) { s.DefaultsError = "the release is not installed"; return; }
            try
            {
                var m = ManifestCheck.Parse(File.ReadAllText(Path.Combine(installDir, "manifest.json")));
                s.DefaultParameters = m.DefaultParameters; s.DefaultRouter = m.DefaultRouter;
                if (m.DefaultParameters == null || m.DefaultRouter == null) s.DefaultsError = "manifest.json has no \"defaults\"";
            }
            catch (Exception e) { s.DefaultsError = "manifest.json: " + e.Message; }
        }

        // The modules of every running DWM. Needs administrator; otherwise error says so.
        public static List<string> DwmModules(out string error)
        {
            error = null;
            var list = new List<string>();
            var dwm = Process.GetProcessesByName("dwm");
            if (dwm.Length == 0) { error = "no DWM is running"; return list; }
            foreach (var p in dwm)
                using (p)
                    try { foreach (ProcessModule m in p.Modules) list.Add(m.FileName); }
                    catch (Exception) { error = "DWM's files can only be checked as administrator"; }
            return list;
        }

        // The scheduled task through the Task Scheduler's scripting objects, late bound (no interop assembly).
        static void ReadTask(RecoverySnapshot s)
        {
            object service = null;
            try
            {
                var type = Type.GetTypeFromProgID("Schedule.Service");
                service = Activator.CreateInstance(type);
                Call(service, "Connect");
                var folder = Call(service, "GetFolder", @"\");
                var task = Call(folder, "GetTask", TaskName);
                s.TaskFound = true;
                s.TaskResult = Convert.ToInt64(Get(task, "LastTaskResult"));
                var last = (DateTime)Get(task, "LastRunTime");
                s.TaskLastRun = last.Year < 2000 ? "never" : last.ToString("yyyy-MM-dd HH:mm", CultureInfo.InvariantCulture);
            }
            catch (Exception) { s.TaskFound = false; }
            finally { if (service != null) System.Runtime.InteropServices.Marshal.FinalReleaseComObject(service); }
        }

        static object Call(object o, string method, params object[] args)
        {
            return o.GetType().InvokeMember(method, BindingFlags.InvokeMethod, null, o, args);
        }

        static object Get(object o, string property)
        {
            return o.GetType().InvokeMember(property, BindingFlags.GetProperty, null, o, null);
        }

        static string LastLine(string path)
        {
            try
            {
                var lines = File.ReadAllLines(path).Where(l => l.Trim().Length > 0).ToArray();
                return lines.Length == 0 ? null : lines[lines.Length - 1].Trim();
            }
            catch (Exception) { return null; }
        }

        public static List<BackupRecord> Backups()
        {
            var list = new List<BackupRecord>();
            if (!Directory.Exists(ControlDirectory)) return list;
            var trusted = new[] { new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null), new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null) };
            foreach (var path in Directory.GetFiles(ControlDirectory, "backup-*.json"))
                try
                {
                    // Only the helper's own files: an elevated process creates them with Administrators as owner.
                    var owner = File.GetAccessControl(path, AccessControlSections.Owner).GetOwner(typeof(SecurityIdentifier));
                    if (!trusted.Contains(owner)) continue;
                    var b = new JavaScriptSerializer().Deserialize<BackupRecord>(File.ReadAllText(path));
                    if (b == null || b.Schema != 1 || b.Values == null) continue;
                    b.File = Path.GetFileName(path);
                    list.Add(b);
                }
                catch (Exception) { }
            return list;
        }

        // The log lines of one run of the helper.
        public static List<string> LogLines(string runId)
        {
            try { return File.ReadAllLines(ActionsLog).Where(l => l.Contains(" " + runId + " ")).ToList(); }
            catch (Exception) { return new List<string>(); }
        }
    }

    public static class RecoveryRunner
    {
        public const int Done = 0, Failed = 1, Usage = 2, Refused = 3, FellBack = 4, NeedsAdministrator = 5;

        sealed class Options
        {
            public string Action, SnapshotFile, OutFile, RunId;
            public uint? Mode, Ceiling;
            public bool DryRun, ModeGiven, CeilingGiven;
        }

        static Options Parse(string[] args)
        {
            if (args.Length < 2 || args[0] != "--action") return null;
            var o = new Options { Action = args[1] };
            for (int i = 2; i < args.Length; i++)
            {
                string a = args[i];
                uint n;
                if (a == "--dry-run") o.DryRun = true;
                else if (i + 1 >= args.Length) return null;
                else if (a == "--snapshot") o.SnapshotFile = args[++i];
                else if (a == "--out") o.OutFile = args[++i];
                else if (a == "--run-id" && System.Text.RegularExpressions.Regex.IsMatch(args[i + 1], "^[a-z0-9]{1,32}$")) o.RunId = args[++i];
                else if (a == "--mode" && args[i + 1] == "unset") { o.ModeGiven = true; i++; }
                else if (a == "--mode" && uint.TryParse(args[i + 1], NumberStyles.None, CultureInfo.InvariantCulture, out n)) { o.Mode = n; o.ModeGiven = true; i++; }
                else if (a == "--ceiling" && args[i + 1] == "unset") { o.CeilingGiven = true; i++; }
                else if (a == "--ceiling" && uint.TryParse(args[i + 1], NumberStyles.None, CultureInfo.InvariantCulture, out n)) { o.Ceiling = n; o.CeilingGiven = true; i++; }
                else return null;
            }
            // set-clocks says what happens to both values ("unset" removes one); no other action takes a mode.
            if (o.Action == "set-clocks" && (!o.ModeGiven || !o.CeilingGiven)) return null;
            if (o.Action != "set-clocks" && o.ModeGiven) return null;
            if (o.SnapshotFile != null && !o.DryRun) return null;     // a recorded snapshot never drives real writes
            if (o.RunId == null) o.RunId = Guid.NewGuid().ToString("N").Substring(0, 12);
            return o;
        }

        public static int Run(string[] args)
        {
            var o = Parse(args);
            if (o == null)
            {
                Console.Error.WriteLine("usage: --action <" + string.Join("|", Recovery.Actions) + "> [--ceiling MHz] [--dry-run [--snapshot file]] [--out file]");
                Console.Error.WriteLine("       --action set-clocks --mode 1|unset --ceiling MHz|unset ...");
                return Usage;
            }
            if (o.DryRun) return DryRun(o);
            // The real run writes its lines (and the exit code) to --out too, as the dry run does.
            _runText = new StringBuilder();
            int code = Execute(o);
            if (o.OutFile != null)
            {
                _runText.AppendLine("exit " + code);
                try { File.WriteAllText(o.OutFile, _runText.ToString()); }
                catch (Exception e) { Console.Error.WriteLine("cannot write " + o.OutFile + ": " + e.Message); }
            }
            return code;
        }

        static int DryRun(Options o)
        {
            var w = new StringBuilder();
            RecoverySnapshot s;
            List<BackupRecord> backups;
            try
            {
                s = o.SnapshotFile != null ? new JavaScriptSerializer().Deserialize<RecoverySnapshot>(File.ReadAllText(o.SnapshotFile)) : RecoveryProbe.Read();
                backups = o.SnapshotFile != null ? new List<BackupRecord>() : RecoveryProbe.Backups();
            }
            catch (Exception e) { Emit(o, "dry run failed: " + e.Message + "\n"); return Failed; }
            w.AppendLine(Program.ProductName + " " + Program.VersionText + " dry run, " + DateTime.UtcNow.ToString("yyyy-MM-dd HH:mm:ss'Z'", CultureInfo.InvariantCulture) +
                ", snapshot " + (o.SnapshotFile ?? "of this PC"));
            w.AppendLine("states:");
            foreach (var l in Recovery.Describe(s))
                w.AppendLine("  [" + l.Severity + "] " + l.Topic + ": " + l.Text + (l.Action != null ? "  -> recommended: " + l.Action : ""));
            var plan = Recovery.Plan(o.Action, s, o.Mode, o.Ceiling, backups);
            w.AppendLine("plan:");
            foreach (var line in plan.Text().TrimEnd().Split('\n')) w.AppendLine("  " + line.TrimEnd('\r'));
            w.AppendLine("dry run: nothing was written");
            Emit(o, w.ToString());
            return plan.Refused ? Refused : Done;
        }

        static void Emit(Options o, string text)
        {
            if (o.OutFile != null) File.WriteAllText(o.OutFile, text);
            else Console.Out.Write(text);
        }

        // ---- the elevated run --------------------------------------------------------------------------------------

        static string _runId;
        static StringBuilder _runText = new StringBuilder();

        static void Log(string text)
        {
            var line = DateTime.UtcNow.ToString("yyyy-MM-dd'T'HH:mm:ss.fff'Z'", CultureInfo.InvariantCulture) + " " + _runId + " " + text;
            try { File.AppendAllText(RecoveryProbe.ActionsLog, line + Environment.NewLine); } catch (Exception) { }
            Console.Out.WriteLine(line);
            _runText.AppendLine(line);
        }

        // An error before the actions log can be written: standard error and --out.
        static void Fail(string text)
        {
            Console.Error.WriteLine(text);
            _runText.AppendLine(text);
        }

        // %ProgramData%\amdgpu-wddm\control: administrators and SYSTEM write, users read. Undo applies these files as
        // administrator, so nobody else may put one there.
        static void PrepareDirectory()
        {
            var sec = new DirectorySecurity();
            sec.SetAccessRuleProtection(true, false);
            const InheritanceFlags all = InheritanceFlags.ContainerInherit | InheritanceFlags.ObjectInherit;
            sec.AddAccessRule(new FileSystemAccessRule(new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null), FileSystemRights.FullControl, all, PropagationFlags.None, AccessControlType.Allow));
            sec.AddAccessRule(new FileSystemAccessRule(new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null), FileSystemRights.FullControl, all, PropagationFlags.None, AccessControlType.Allow));
            sec.AddAccessRule(new FileSystemAccessRule(new SecurityIdentifier(WellKnownSidType.BuiltinUsersSid, null), FileSystemRights.ReadAndExecute, all, PropagationFlags.None, AccessControlType.Allow));
            if (!Directory.Exists(RecoveryProbe.ControlDirectory)) Directory.CreateDirectory(RecoveryProbe.ControlDirectory, sec);
            else Directory.SetAccessControl(RecoveryProbe.ControlDirectory, sec);
        }

        static int Execute(Options o)
        {
            if (!Program.IsElevated()) { Fail("--action needs administrator (or --dry-run)"); return NeedsAdministrator; }
            _runId = o.RunId;
            try { PrepareDirectory(); }
            catch (Exception e) { Fail("cannot prepare " + RecoveryProbe.ControlDirectory + ": " + e.Message); return Failed; }
            Log("start " + o.Action + (o.Mode != null ? " mode " + o.Mode : "") + (o.Ceiling != null ? " ceiling " + o.Ceiling : "") + ", " + Program.ProductName + " " + Program.VersionText);
            try
            {
                var s = RecoveryProbe.Read();
                var plan = Recovery.Plan(o.Action, s, o.Mode, o.Ceiling, RecoveryProbe.Backups());
                if (plan.Refused) { Log("result: refused: " + plan.Refusal); return Refused; }
                var backup = Backup(o, plan, s);
                Log("backup " + backup.File);

                if (plan.ConfirmStart)
                {
                    var why = Confirm();
                    if (why != null) { Log("result: failed: " + why); return Failed; }
                }
                if (!Apply(plan.Writes, backup)) { Log("result: failed: a write did not read back; the old values were restored"); return Failed; }
                if (plan.RestartDwm) return RestartDesktop(plan);
                Log("result: done: " + plan.Change + " Takes effect " + plan.Effect + ".");
                return Done;
            }
            catch (Exception e) { Log("result: failed: " + e.GetType().Name + ": " + e.Message); return Failed; }
        }

        static BackupRecord Backup(Options o, ActionPlan plan, RecoverySnapshot s)
        {
            var b = new BackupRecord
            {
                Action = o.Action, Utc = DateTime.UtcNow.ToString("o", CultureInfo.InvariantCulture), RunId = o.RunId,
                Args = new[] { o.Mode != null ? "mode=" + o.Mode : null, o.Ceiling != null ? "ceiling=" + o.Ceiling : null }.Where(a => a != null).ToArray(),
                Undoable = plan.Undoable, RestartsDwm = plan.RestartDwm, Undoes = plan.UndoOf,
            };
            var names = plan.Writes.Select(w => new[] { w.Path, w.Name }).ToList();
            if (plan.ConfirmStart)
                foreach (var n in new[] { "UnconfirmedStarts", "DpmPending" }) names.Add(new[] { Recovery.ParametersPath, n });   // a record only
            foreach (var pn in names)
                using (var k = Registry.LocalMachine.OpenSubKey(pn[0]))
                {
                    var v = new BackupValue { Path = pn[0], Name = pn[1] };
                    object data = k == null ? null : k.GetValue(pn[1], null, RegistryValueOptions.DoNotExpandEnvironmentNames);
                    if (data != null)
                    {
                        v.Existed = true;
                        var kind = k.GetValueKind(pn[1]);
                        if (kind == RegistryValueKind.DWord) { v.Kind = "DWord"; v.Number = (uint)(int)data; }
                        else if (kind == RegistryValueKind.QWord) { v.Kind = "QWord"; v.Number = (long)data; }
                        else if (kind == RegistryValueKind.String) { v.Kind = "String"; v.Text = (string)data; }
                        else throw new InvalidOperationException(pn[1] + " has registry type " + kind + ", which this app does not restore; nothing was changed");
                    }
                    b.Values.Add(v);
                }
            var file = Recovery.BackupFileName(DateTime.UtcNow);
            var path = Path.Combine(RecoveryProbe.ControlDirectory, file);
            File.WriteAllText(path + ".partial", new JavaScriptSerializer().Serialize(b));
            var owner = new FileSecurity();         // RecoveryProbe.Backups reads only files Administrators own
            owner.SetOwner(new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null));
            File.SetAccessControl(path + ".partial", owner);
            File.Move(path + ".partial", path);
            b.File = file;
            return b;
        }

        static void Write(RegWrite w)
        {
            if (!Recovery.Allowed(w.Path, w.Name)) throw new InvalidOperationException(w.Name + " is not a value this app may write");
            using (var k = Registry.LocalMachine.OpenSubKey(w.Path, true))
            {
                if (k == null) throw new InvalidOperationException(@"HKLM\" + w.Path + " does not exist");
                if (w.Delete) k.DeleteValue(w.Name, false);
                else if (w.Kind == "DWord") k.SetValue(w.Name, unchecked((int)(uint)w.Number), RegistryValueKind.DWord);
                else if (w.Kind == "QWord") k.SetValue(w.Name, w.Number, RegistryValueKind.QWord);
                else if (w.Kind == "String") k.SetValue(w.Name, w.Text ?? "", RegistryValueKind.String);
                else throw new InvalidOperationException("unknown value kind " + w.Kind);
            }
        }

        static bool ReadsBack(RegWrite w)
        {
            using (var k = Registry.LocalMachine.OpenSubKey(w.Path))
            {
                object v = k == null ? null : k.GetValue(w.Name, null, RegistryValueOptions.DoNotExpandEnvironmentNames);
                if (w.Delete) return v == null;
                if (w.Kind == "DWord") return v is int && (uint)(int)v == (uint)w.Number;
                if (w.Kind == "QWord") return v is long && (long)v == w.Number;
                return v is string && (string)v == (w.Text ?? "");
            }
        }

        static bool Apply(List<RegWrite> writes, BackupRecord backup)
        {
            try
            {
                foreach (var w in writes) { Write(w); Log("wrote: " + w); }
                bool ok = true;
                foreach (var w in writes)
                {
                    bool back = ReadsBack(w);
                    Log((back ? "read back OK: " : "read back MISMATCH: ") + w);
                    ok &= back;
                }
                if (ok) return true;
            }
            catch (Exception e) { Log("write failed: " + e.Message); }
            foreach (var v in backup.Values.Where(v => writes.Any(w => w.Path == v.Path && w.Name == v.Name)))
                try
                {
                    var r = v.Existed ? new RegWrite { Path = v.Path, Name = v.Name, Kind = v.Kind, Number = v.Number, Text = v.Text } : RegWrite.Remove(v.Path, v.Name);
                    Write(r); Log("restored: " + r);
                }
                catch (Exception e) { Log("restore failed: " + v.Name + ": " + e.Message); }
            return false;
        }

        // The confirmation with the full rule of start-confirm-core.ps1 (fresh completion included). An idle desktop
        // may present rarely, so the helper retries the reading for up to 10 s before it gives up.
        static string Confirm()
        {
            string why = null;
            var clock = Stopwatch.StartNew();
            while (clock.Elapsed.TotalSeconds < 10)
            {
                var r = Kmd.StartHealth();
                if (r.Value == null) return "no start reading: " + r.Error;
                if (Recovery.Confirmed(r.Value)) { Log("confirmed already: flags " + r.Value.Flags + " generation " + r.Value.Generation); return null; }
                why = Recovery.ConfirmBlocker(r.Value, true);
                if (why == null)
                {
                    Log("eligible: flags " + r.Value.Flags + " completed " + r.Value.Completed + " age_ms " + r.Value.LastCompletionAgeMs + " ready_ms " + r.Value.ReadyAgeMs);
                    var c = Kmd.ConfirmStart(r.Value.Generation, r.Value.Epoch);
                    if (c.Value == null) return "the driver refused the confirmation: " + c.Error;
                    var after = Kmd.StartHealth();
                    if (after.Value == null || !Recovery.Confirmed(after.Value)) return "the start does not read back as confirmed";
                    Log("read back: flags " + after.Value.Flags + " (confirmed), UnconfirmedStarts " +
                        (SettingsStore.ReadDword(Recovery.ParametersPath, "UnconfirmedStarts")?.ToString(CultureInfo.InvariantCulture) ?? "absent") +
                        ", DpmPending " + (SettingsStore.ReadDword(Recovery.ParametersPath, "DpmPending") != null ? "present" : "absent"));
                    return null;
                }
                Thread.Sleep(500);
            }
            return why;
        }

        // ---- the desktop restart and its watchdog ------------------------------------------------------------------

        static int RestartDesktop(ActionPlan plan)
        {
            string route;
            var verdict = RestartDwm(plan.WatchDwm, out route);
            if (verdict == "ok")
            {
                string note = plan.DwmForceCpuAfter == 0 && route == "cpu" ? " DWM stayed on the CPU route: the desktop router chose the CPU UMD." : "";
                Log("result: done: DWM restarted and held " + (plan.WatchDwm ? Recovery.WatchSeconds + " s" : "") + " on the " + route.ToUpperInvariant() + " route." + note);
                return Done;
            }
            Log("watchdog: " + verdict);
            if (plan.DwmForceCpuAfter == 1) { Log("result: failed: " + verdict + " (on the CPU route)"); return Failed; }
            var cpu = RegWrite.Dword(Recovery.RouterPath, "DwmForceCpu", 1);
            Write(cpu); Log("wrote: " + cpu + (ReadsBack(cpu) ? " (read back OK)" : " (read back MISMATCH)"));
            var again = RestartDwm(false, out route);
            Log("result: fell back to CPU: " + verdict + "; DWM restarted on the " + route.ToUpperInvariant() + " route" + (again == "ok" ? "" : " (" + again + ")") + ".");
            return FellBack;
        }

        // Stops every DWM (winlogon starts a new one) and watches the new one: for WatchSeconds with watch, else a few
        // seconds of settling. Returns "ok" or the failure; route is what the new DWM loaded.
        static string RestartDwm(bool watch, out string route)
        {
            route = "unknown";
            var since = DateTime.UtcNow.AddSeconds(-1);
            var old = new HashSet<int>(Ids());
            Log("restarting DWM (" + old.Count + " running)" + (watch ? ", watching " + Recovery.WatchSeconds + " s" : ""));
            foreach (var p in Process.GetProcessesByName("dwm")) using (p) try { p.Kill(); } catch (Exception e) { Log("could not stop DWM " + p.Id + ": " + e.Message); }
            int? first = null;
            var clock = Stopwatch.StartNew();
            string verdict;
            while (true)
            {
                Thread.Sleep(1000);
                var now = Ids().Where(id => !old.Contains(id)).ToList();
                if (first == null && now.Count > 0) { first = now.Min(); Log("new DWM " + first + " after " + clock.Elapsed.TotalSeconds.ToString("0", CultureInfo.InvariantCulture) + " s"); }
                verdict = Recovery.WatchVerdict(first, now, watch ? Crashes(since) : 0, clock.Elapsed.TotalSeconds, watch ? Recovery.WatchSeconds : 5);
                if (verdict != null) break;
            }
            if (first != null)
                try
                {
                    using (var p = Process.GetProcessById(first.Value))
                        route = Recovery.RouteFromModules(p.Modules.Cast<ProcessModule>().Select(m => m.FileName));
                }
                catch (Exception) { }
            return verdict;
        }

        static IEnumerable<int> Ids()
        {
            foreach (var p in Process.GetProcessesByName("dwm")) using (p) yield return p.Id;
        }

        static int Crashes(DateTime sinceUtc)
        {
            string q = "*[System[Provider[@Name='Application Error'] and (EventID=1000) and TimeCreated[@SystemTime>='" +
                sinceUtc.ToString("yyyy-MM-dd'T'HH:mm:ss.fff'Z'", CultureInfo.InvariantCulture) + "']]]";
            int n = 0;
            try
            {
                using (var reader = new EventLogReader(new EventLogQuery("Application", PathType.LogName, q)))
                    for (var e = reader.ReadEvent(); e != null; e = reader.ReadEvent())
                        using (e)
                            if (e.Properties.Count > 0 && string.Equals(e.Properties[0].Value as string, "dwm.exe", StringComparison.OrdinalIgnoreCase)) n++;
            }
            catch (EventLogException) { }
            return n;
        }
    }
}
