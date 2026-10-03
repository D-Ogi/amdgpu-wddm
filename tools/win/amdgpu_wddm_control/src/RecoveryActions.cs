// The Recovery page's I/O: the snapshot the rules of Recovery.cs look at, and the elevated helper that executes one
// plan. The helper is this exe started with --action <name> (one UAC prompt per action); it plans again from its own
// snapshot, so the window's earlier check is never trusted, and then:
//   1. saves the old value of everything it will write to %ProgramData%\amdgpu-wddm\control\backup-<utc>.json,
//   2. writes, logging each step to control-actions.log in the same directory,
//   3. reads every value back and reports the result (a failed write restores the backup at once),
//   4. never restarts, stops or signals DWM (BD-060): every change takes effect at the next restart of Windows,
//      which the window offers. The one exception is the operator escape restart-compositor, which the window does
//      not offer and which needs --accept-bd060.
// --dry-run prints the states and the plan and writes nothing; --snapshot <json> (dry run only) plans from a
// recorded snapshot instead of this PC.
//
// --status prints the states without an action (the installer's verify, the bug report).
//
// Every reading except the dry run records the active session's DWM (process id and creation time) in
// dwm-observations.json: the user's copy under %LOCALAPPDATA%\amdgpu-wddm, the administrator's copy in the control
// directory. A later reading that finds another DWM in the same session of the same boot is an observed restart.
//
// Exit codes: 0 done, 1 failed, 2 usage, 3 refused, 5 needs administrator (4 was "fell back to the CPU route" of the
// DWM watchdog, removed with the DWM restart).
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
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

        // observer names who reads (window, status, report, helper, escape) and records the DWM it sees; null reads
        // without recording (the dry run).
        public static RecoverySnapshot Read(string observer = null)
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
            ReadCompositor(s, observer);
            var route = Safe(Backups).Where(b => b.Values.Any(v => v.Path == Recovery.RouterPath && v.Name == "DwmForceCpu"))
                .OrderBy(b => Recovery.Utc(b.Utc) ?? DateTime.MinValue).LastOrDefault();
            if (route != null) s.RouteWrittenUtc = Stamp(Recovery.Utc(route.Utc));
            s.ConfirmLogLast = LastLine(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "amdgpu-wddm", "start-confirm.log"));
            return s;
        }

        static List<BackupRecord> Safe(Func<List<BackupRecord>> read)
        {
            try { return read(); } catch (Exception) { return new List<BackupRecord>(); }
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

        [DllImport("kernel32.dll")]
        static extern ulong GetTickCount64();

        [DllImport("kernel32.dll")]
        static extern uint WTSGetActiveConsoleSessionId();

        [DllImport("ntdll.dll")]
        static extern int NtQuerySystemInformation(int informationClass, IntPtr buffer, int length, out int returned);

        const int SystemProcessInformation = 5;
        const int StatusInfoLengthMismatch = unchecked((int)0xC0000004);

        public sealed class Started
        {
            public int Session, Pid;
            public DateTime Utc;
        }

        // Session, process id and start time (UTC) of every process with this image name, from the system's process
        // list (SYSTEM_PROCESS_INFORMATION, x64: CreateTime at 0x20, ImageName at 0x38, UniqueProcessId at 0x50,
        // SessionId at 0x64). No process is opened: a normal user may not open DWM or winlogon at all.
        public static List<Started> Starts(string image)
        {
            var list = new List<Started>();
            int size = 1 << 20;
            for (int attempt = 0; attempt < 4; attempt++)
            {
                var buffer = Marshal.AllocHGlobal(size);
                try
                {
                    int returned;
                    int status = NtQuerySystemInformation(SystemProcessInformation, buffer, size, out returned);
                    if (status == StatusInfoLengthMismatch) { size = Math.Max(size * 2, returned + 65536); continue; }
                    if (status != 0) return list;
                    long offset = 0;
                    while (true)
                    {
                        var entry = new IntPtr(buffer.ToInt64() + offset);
                        int next = Marshal.ReadInt32(entry, 0x00);
                        int nameBytes = Marshal.ReadInt16(entry, 0x38) & 0xFFFF;
                        var name = Marshal.ReadIntPtr(entry, 0x40);
                        if (name != IntPtr.Zero && nameBytes > 0 && string.Equals(Marshal.PtrToStringUni(name, nameBytes / 2), image, StringComparison.OrdinalIgnoreCase))
                            list.Add(new Started { Session = Marshal.ReadInt32(entry, 0x64), Pid = (int)Marshal.ReadIntPtr(entry, 0x50).ToInt64(), Utc = DateTime.FromFileTimeUtc(Marshal.ReadInt64(entry, 0x20)) });
                        if (next == 0) break;
                        offset += next;
                    }
                    return list;
                }
                finally { Marshal.FreeHGlobal(buffer); }
            }
            return list;
        }

        public static string Stamp(DateTime? utc)
        {
            return utc == null ? null : utc.Value.ToUniversalTime().ToString("yyyy-MM-dd'T'HH:mm:ss.fff'Z'", CultureInfo.InvariantCulture);
        }

        // The active interactive session: this process's session, or the console session from session 0.
        public static int ActiveSession()
        {
            using (var self = Process.GetCurrentProcess())
                return self.SessionId != 0 ? self.SessionId : (int)WTSGetActiveConsoleSessionId();
        }

        // The boot, its start (the session's winlogon) and the session's DWM; read only, nothing is opened (BD-060).
        public static DwmReading ReadDwm()
        {
            var r = new DwmReading();
            try
            {
                using (var k = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management\PrefetchParameters"))
                    if (k != null && k.GetValue("BootId") is int) r.BootId = (uint)(int)k.GetValue("BootId");
                int session = ActiveSession();
                r.Session = session;
                // The session's first winlogon starts the session; the newest DWM is the one running now (an old one
                // may still be exiting).
                var logon = Starts("winlogon.exe").Where(x => x.Session == session).OrderBy(x => x.Utc).FirstOrDefault();
                if (logon != null) r.SessionStartUtc = Stamp(logon.Utc);
                var dwm = Starts("dwm.exe").Where(x => x.Session == session).OrderBy(x => x.Utc).LastOrDefault();
                if (dwm != null) { r.Pid = dwm.Pid; r.CreatedUtc = Stamp(dwm.Utc); }
            }
            catch (Exception) { }
            return r;
        }

        // AMDGPU_WDDM_CONTROL_STATE moves both copies to one directory (the build's gates, so that a build records
        // nothing in the profile of the PC that builds).
        static string StateOverride { get { var d = Environment.GetEnvironmentVariable("AMDGPU_WDDM_CONTROL_STATE"); return string.IsNullOrEmpty(d) ? null : d; } }

        static string UserObservations
        {
            get { return Path.Combine(StateOverride ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "amdgpu-wddm"), "dwm-observations.json"); }
        }

        static string MachineObservations { get { return StateOverride == null ? Path.Combine(ControlDirectory, "dwm-observations.json") : UserObservations; } }

        static DwmObservations Load(string path, bool adminOwned)
        {
            try
            {
                if (!File.Exists(path)) return null;
                if (adminOwned && !Trusted(path)) return null;
                var o = new JavaScriptSerializer().Deserialize<DwmObservations>(File.ReadAllText(path));
                return o != null && o.Schema == 1 && o.Instances != null ? o : null;
            }
            catch (Exception) { return null; }
        }

        static bool Trusted(string path)
        {
            var trusted = new[] { new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null), new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null) };
            return trusted.Contains(File.GetAccessControl(path, AccessControlSections.Owner).GetOwner(typeof(SecurityIdentifier)));
        }

        // The DWM of the active session against what observers saw before in this session. With an observer the
        // merged record is saved when it changed: as administrator to the control directory, otherwise to the user's
        // copy.
        public static void ReadCompositor(RecoverySnapshot s, string observer)
        {
            try
            {
                s.DwmNow = ReadDwm();
                var user = Load(UserObservations, false);
                var machine = StateOverride == null ? Load(MachineObservations, true) : null;
                s.DwmHistory = Recovery.Observe(new[] { user, machine }, s.DwmNow, observer ?? "dry-run", Stamp(DateTime.UtcNow));
                if (s.DwmHistory == null || observer == null) return;
                // At most 64 instances: a DWM that restarts in a loop is a defect, and the first and the last tell it.
                if (s.DwmHistory.Instances.Count > 64) s.DwmHistory.Instances.RemoveRange(1, s.DwmHistory.Instances.Count - 64);
                bool admin = Program.IsElevated() && StateOverride == null;
                var mine = admin ? machine : user;
                var json = new JavaScriptSerializer().Serialize(s.DwmHistory);
                if (mine != null && new JavaScriptSerializer().Serialize(mine) == json) return;
                if (admin) RecoveryRunner.PrepareDirectory();
                var path = admin ? MachineObservations : UserObservations;
                Directory.CreateDirectory(Path.GetDirectoryName(path));
                File.WriteAllText(path + ".partial", json);
                if (admin)
                {
                    var owner = new FileSecurity();
                    owner.SetOwner(new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null));
                    File.SetAccessControl(path + ".partial", owner);
                }
                if (File.Exists(path)) File.Delete(path);
                File.Move(path + ".partial", path);
            }
            catch (Exception) { }
        }

        // The states as text: the compositor line for scripts, then every state line.
        public static string StatusText(RecoverySnapshot s)
        {
            var w = new StringBuilder();
            w.AppendLine(Recovery.CompositorStatusLine(s));
            w.AppendLine("states:");
            foreach (var l in Recovery.Describe(s))
                w.AppendLine("  [" + l.Severity + "] " + l.Topic + ": " + l.Text + (l.Action != null ? "  -> recommended: " + l.Action : ""));
            return w.ToString();
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
            foreach (var path in Directory.GetFiles(ControlDirectory, "backup-*.json"))
                try
                {
                    // Only the helper's own files: an elevated process creates them with Administrators as owner.
                    if (!Trusted(path)) continue;
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
        public const int Done = 0, Failed = 1, Usage = 2, Refused = 3, NeedsAdministrator = 5;

        sealed class Options
        {
            public string Action, SnapshotFile, OutFile, RunId;
            public uint? Mode, Ceiling;
            public bool DryRun, ModeGiven, CeilingGiven, AcceptBd060;
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
                else if (a == "--accept-bd060") o.AcceptBd060 = true;
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
                Console.Error.WriteLine("       --action " + Recovery.OperatorEscape + " --accept-bd060   (operator escape for a desktop that does not respond; see README)");
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
            w.Append(RecoveryProbe.StatusText(s));
            var plan = Recovery.Plan(o.Action, s, o.Mode, o.Ceiling, backups, o.AcceptBd060);
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
        public static void PrepareDirectory()
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
                var s = RecoveryProbe.Read(o.Action == Recovery.OperatorEscape ? "escape" : "helper");
                var plan = Recovery.Plan(o.Action, s, o.Mode, o.Ceiling, RecoveryProbe.Backups(), o.AcceptBd060);
                if (plan.Refused) { Log("result: refused: " + plan.Refusal); return Refused; }
                if (plan.RestartCompositor) return StopCompositor(s);
                var backup = Backup(o, plan, s);
                Log("backup " + backup.File);

                if (plan.ConfirmStart)
                {
                    var why = Confirm();
                    if (why != null) { Log("result: failed: " + why); return Failed; }
                }
                if (!Apply(plan.Writes, backup)) { Log("result: failed: a write did not read back; the old values were restored"); return Failed; }
                // A change that applies at the next restart is written, not done: the active route stays until then.
                Log(plan.OfferRestart ? "result: written, pending until the next restart of Windows: " + plan.Change
                    : "result: done: " + plan.Change + " Takes effect " + plan.Effect + ".");
                return Done;
            }
            catch (Exception e) { Log("result: failed: " + e.GetType().Name + ": " + e.Message); return Failed; }
        }

        // The operator escape (restart-compositor --accept-bd060): stops the DWM of the active session and waits up to
        // 20 s for the new one, which Windows starts. The only place in this app that stops DWM (BD-060).
        static int StopCompositor(RecoverySnapshot s)
        {
            var old = s.DwmNow;
            if (old == null || old.Pid == null) { Log("result: failed: the DWM of the active session cannot be read"); return Failed; }
            Log("warning: after a DWM restart some Windows 11 apps can ignore mouse clicks until Windows restarts (BD-060)");
            Log("stopping DWM process " + old.Pid + " (started " + old.CreatedUtc + ") in session " + old.Session);
            if (!RecoveryProbe.Starts("dwm.exe").Any(x => x.Pid == old.Pid && x.Session == old.Session && RecoveryProbe.Stamp(x.Utc) == old.CreatedUtc))
            { Log("result: failed: process " + old.Pid + " is no longer that DWM; nothing was stopped"); return Failed; }
            using (var p = Process.GetProcessById(old.Pid.Value)) p.Kill();
            var clock = Stopwatch.StartNew();
            while (clock.Elapsed.TotalSeconds < 20)
            {
                Thread.Sleep(500);
                var now = RecoveryProbe.ReadDwm();
                if (now.Pid != null && now.CreatedUtc != old.CreatedUtc)
                {
                    var after = new RecoverySnapshot();
                    RecoveryProbe.ReadCompositor(after, "escape");
                    Log("new DWM process " + now.Pid + " started " + now.CreatedUtc + "; " + Recovery.CompositorStatusLine(after));
                    Log("result: done: the desktop compositor was restarted. Restart Windows as soon as you can (BD-060).");
                    return Done;
                }
            }
            Log("result: failed: no new DWM in session " + old.Session + " after 20 s; restart Windows");
            return Failed;
        }

        static BackupRecord Backup(Options o, ActionPlan plan, RecoverySnapshot s)
        {
            var b = new BackupRecord
            {
                Action = o.Action, Utc = DateTime.UtcNow.ToString("o", CultureInfo.InvariantCulture), RunId = o.RunId,
                Args = new[] { o.Mode != null ? "mode=" + o.Mode : null, o.Ceiling != null ? "ceiling=" + o.Ceiling : null }.Where(a => a != null).ToArray(),
                Undoable = plan.Undoable, Undoes = plan.UndoOf,
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
    }
}
