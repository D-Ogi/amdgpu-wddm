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
// directory. The installer's record (%ProgramData%\amdgpu-wddm\dwm-baseline.json, written by its start-confirm task
// at each logon) is read too, never written. A later reading that finds another DWM in the same session of the same
// boot is an observed restart.
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
using Microsoft.Win32.SafeHandles;

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
            var s = new RecoverySnapshot { CuBadValues = new List<string>() };
            try
            {
                using (var k = Registry.LocalMachine.OpenSubKey(Recovery.ParametersPath))
                {
                    s.DriverInstalled = k != null;
                    if (k != null)
                        foreach (var name in k.GetValueNames())
                            if (k.GetValueKind(name) == RegistryValueKind.DWord) s.Parameters[name] = (uint)(int)k.GetValue(name);
                            else if (CuMode.ValueNames.Contains(name)) s.CuBadValues.Add(name);
                }
            }
            catch (Exception) { s.CuBadValues.Add("(the driver's settings key cannot be read)"); }
            try { s.GameProfiles = new Dictionary<string, string>(SettingsStore.ReadProfiles(), StringComparer.OrdinalIgnoreCase); }
            catch (Exception) { s.GameProfiles = null; }
            try { s.GfxKeys = SettingsStore.ReadGfxKeys(); }
            catch (Exception) { s.GfxKeys = null; }
            ReadTdr(s);
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
            s.Cu = Kmd.CuMode().Value;
            // The tuning surfaces. Both are software reads of this start; a trial's countdown runs in the driver, so
            // every reading is fresh by construction and the window never keeps a timer of its own.
            s.Curve = Kmd.Curve().Value;
            s.Cpu = Kmd.Cpu().Value;
            s.Fan = Kmd.Fan().Value;
            if (s.Interop == null && s.Health == null && s.Dpm == null) s.DriverError = dpm.Error ?? interop.Error ?? "no answer";

            string installDir = "";
            using (var k = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\amdgpu-wddm\Release"))
                if (k != null) installDir = Convert.ToString(k.GetValue("InstallDir") ?? "");
            s.GpuDesktopModules = installDir.Length > 0 &&
                new[] { "bc250d3d_router.dll", "bc250d3d_zink.dll", "amdgpu_wddm_radv.dll" }.All(f => File.Exists(Path.Combine(installDir, "desktop", f)));
            ReadDefaults(s, installDir);

            ReadTask(s);
            ReadCompositor(s, observer);
            ReadRoute(s);
            var route = LoadRouteRecord();
            if (route != null) { s.RouteWrittenUtc = Stamp(Recovery.Utc(route.Utc)); s.RouteWrittenValue = route.Value; }
            s.CuNotDurable = CuUnflushedThisBoot();
            s.ConfirmLogLast = LastLine(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "amdgpu-wddm", "start-confirm.log"));
            return s;
        }

        // The active route from the modules of the session's DWM that DwmNow names, read through one handle that is
        // checked against that reading before and after the module list; anything else is unknown (reviewer 911).
        static void ReadRoute(RecoverySnapshot s)
        {
            string why;
            using (var h = DwmHandle.Open(s.DwmNow, DwmHandle.QueryInformation | DwmHandle.VmRead, out why))
            {
                if (h == null) { s.DwmRouteDetail = why; return; }
                var modules = DwmHandle.Modules(h);
                if (modules == null) { s.DwmRouteDetail = "the files of DWM process " + s.DwmNow.Pid + " could not be listed completely"; return; }
                if (!DwmHandle.Matches(h, s.DwmNow)) { s.DwmRouteDetail = "DWM process " + s.DwmNow.Pid + " changed while its files were read"; return; }
                s.DwmRoute = Recovery.RouteFromModules(modules);
                s.DwmRouteDetail = string.Join(", ", modules.Select(Path.GetFileName).Where(f => f.StartsWith("bc250", StringComparison.OrdinalIgnoreCase) ||
                    f.StartsWith("amdgpu_wddm", StringComparison.OrdinalIgnoreCase)).Distinct(StringComparer.OrdinalIgnoreCase));
            }
        }

        public static string RouteRecordPath { get { return Path.Combine(StateOverride ?? ControlDirectory, "route-written.json"); } }

        public static string CuUnflushedPath { get { return Path.Combine(StateOverride ?? ControlDirectory, "cu-unflushed.json"); } }

        // True when a setter run of THIS boot could not flush or verify its writes (cu-unflushed.json).
        static bool CuUnflushedThisBoot()
        {
            try
            {
                if (!File.Exists(CuUnflushedPath) || (StateOverride == null && !Trusted(CuUnflushedPath))) return false;
                var r = new JavaScriptSerializer().Deserialize<CuUnflushedRecord>(File.ReadAllText(CuUnflushedPath));
                var boot = DriverCardProbe.BootId();
                return r != null && r.Schema == 1 && boot != null && r.BootId == boot.Value;
            }
            catch (Exception) { return false; }
        }

        static RouteRecord LoadRouteRecord()
        {
            try
            {
                if (!File.Exists(RouteRecordPath) || (StateOverride == null && !Trusted(RouteRecordPath))) return null;
                var r = new JavaScriptSerializer().Deserialize<RouteRecord>(File.ReadAllText(RouteRecordPath));
                return r != null && r.Schema == 1 && Recovery.Utc(r.Utc) != null ? r : null;
            }
            catch (Exception) { return null; }
        }

        static long? Dword(RegistryKey k, string name)
        {
            var v = k.GetValue(name);
            return v is int ? (long?)(uint)(int)v : null;
        }

        // How long Windows waits for the graphics (TdrSetting.cs). Windows' own key, so the read is separate from the
        // driver's Parameters: absent is the normal state of a machine the installer never touched, and a value of
        // another registry type is named in TdrError instead of passing as absent.
        public static void ReadTdr(RecoverySnapshot s)
        {
            try
            {
                using (var k = SettingsStore.Machine.OpenSubKey(TdrSetting.RegistryPath))
                {
                    if (k == null) { s.TdrError = "the graphics settings key of Windows does not exist"; return; }
                    var v = k.GetValue(TdrSetting.ValueName);
                    if (v == null) return;
                    if (k.GetValueKind(TdrSetting.ValueName) != RegistryValueKind.DWord)
                    {
                        s.TdrError = TdrSetting.ValueName + " has registry type " + k.GetValueKind(TdrSetting.ValueName);
                        return;
                    }
                    s.TdrDelay = (uint)(int)v;
                }
            }
            catch (Exception e) { s.TdrError = e.Message; }
        }

        static void ReadDefaults(RecoverySnapshot s, string installDir)
        {
            if (installDir.Length == 0) { s.DefaultsError = "the release is not installed"; return; }
            try
            {
                var m = ManifestCheck.Parse(File.ReadAllText(Path.Combine(installDir, "manifest.json")));
                s.DefaultParameters = m.DefaultParameters; s.DefaultRouter = m.DefaultRouter; s.DefaultApplications = m.DefaultApplications;
                s.DefaultGraphicsDrivers = m.DefaultGraphicsDrivers;
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

        public static string Stamp(DateTime? utc) { return Recovery.Stamp(utc); }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        struct WtsInfo
        {
            public int State, SessionId, IncomingBytes, OutgoingBytes, IncomingFrames, OutgoingFrames, IncomingCompressedBytes, OutgoingCompressedBytes;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string WinStationName;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 17)] public string Domain;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 21)] public string UserName;
            public long ConnectTime, DisconnectTime, LastInputTime, LogonTime, CurrentTime;
        }

        [DllImport("wtsapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern bool WTSQuerySessionInformationW(IntPtr server, int sessionId, int infoClass, out IntPtr buffer, out int bytes);

        [DllImport("wtsapi32.dll")]
        static extern void WTSFreeMemory(IntPtr memory);

        // The session's logon time (WTSSessionInfo, as the installer reads it); null when nobody is logged on.
        static DateTime? LogonTime(int session)
        {
            IntPtr buffer; int bytes;
            if (!WTSQuerySessionInformationW(IntPtr.Zero, session, 24, out buffer, out bytes)) return null;
            try { long t = ((WtsInfo)Marshal.PtrToStructure(buffer, typeof(WtsInfo))).LogonTime; return t > 0 ? DateTime.FromFileTimeUtc(t) : (DateTime?)null; }
            finally { WTSFreeMemory(buffer); }
        }

        // The boot time as Win32_OperatingSystem.LastBootUpTime gives it: SystemTimeOfDayInformation's BootTime (at 0)
        // less BootTimeBias (at 32), the clock corrections since the boot (52 s on the PC that builds this).
        static DateTime? BootTime()
        {
            var buffer = Marshal.AllocHGlobal(64);
            try
            {
                int returned;
                return NtQuerySystemInformation(3, buffer, 48, out returned) == 0 ? DateTime.FromFileTimeUtc(Marshal.ReadInt64(buffer, 0) - Marshal.ReadInt64(buffer, 32)) : (DateTime?)null;
            }
            finally { Marshal.FreeHGlobal(buffer); }
        }

        // The active interactive session: this process's session, or the console session from session 0.
        public static int ActiveSession()
        {
            using (var self = Process.GetCurrentProcess())
                return self.SessionId != 0 ? self.SessionId : (int)WTSGetActiveConsoleSessionId();
        }

        // The boot, its start (the session's winlogon) and the session's DWM; read only, nothing is opened (BD-060).
        // pinned names the session (the escape's wait); otherwise the active one.
        public static DwmReading ReadDwm(int? pinned = null)
        {
            var r = new DwmReading();
            try
            {
                using (var k = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management\PrefetchParameters"))
                    if (k != null && k.GetValue("BootId") is int) r.BootId = (uint)(int)k.GetValue("BootId");
                int session = pinned ?? ActiveSession();
                r.Session = session;
                // The session's first winlogon starts the session; the newest DWM is the one running now (an old one
                // may still be exiting).
                var logon = Starts("winlogon.exe").Where(x => x.Session == session).OrderBy(x => x.Utc).FirstOrDefault();
                if (logon != null) r.SessionStartUtc = Stamp(logon.Utc);
                var dwm = Starts("dwm.exe").Where(x => x.Session == session).OrderBy(x => x.Utc).LastOrDefault();
                if (dwm != null) { r.Pid = dwm.Pid; r.CreatedUtc = Stamp(dwm.Utc); }
                r.BootUtc = Stamp(BootTime());
                r.LogonUtc = Stamp(LogonTime(session));
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

        // The installer's record of the DWM at each logon (start-confirm task). Read only: this app never writes it.
        static string InstallerBaseline
        {
            get { return Path.Combine(StateOverride ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "amdgpu-wddm"), "dwm-baseline.json"); }
        }

        static DwmObservations LoadInstallerBaseline(DwmReading now)
        {
            try { return File.Exists(InstallerBaseline) ? Recovery.FromBaseline(Recovery.ParseBaseline(File.ReadAllText(InstallerBaseline)), now) : null; }
            catch (Exception) { return null; }
        }

        // The records of one copy; empty when it is missing, damaged or (the administrator's copy) not owned by
        // Administrators or SYSTEM. Recovery.Observe validates every entry.
        static List<DwmObservations> Load(string path, bool adminOwned)
        {
            try
            {
                if (!File.Exists(path)) return new List<DwmObservations>();
                if (adminOwned && !Trusted(path)) return new List<DwmObservations>();
                var f = new JavaScriptSerializer().Deserialize<DwmObservationsFile>(File.ReadAllText(path));
                return f != null && f.Schema == 2 && f.Records != null ? f.Records.Where(r => r != null && r.Instances != null).ToList() : new List<DwmObservations>();     // Recovery.PlanCommit sanitizes
            }
            catch (Exception) { return new List<DwmObservations>(); }
        }

        // The mutex that serializes the read, merge and replace of one copy, named after its path.
        static string LockName(string path)
        {
            using (var sha = System.Security.Cryptography.SHA256.Create())
                return @"Global\amdgpu-wddm-dwm-observations-" + BitConverter.ToString(sha.ComputeHash(Encoding.UTF8.GetBytes(Path.GetFullPath(path).ToUpperInvariant())), 0, 8).Replace("-", "");
        }

        // Re-reads the copy, merges this session's history into it and replaces the file atomically, all under one
        // cross-process lock, so that no observer's instance is lost to another's older snapshot (reviewer 911). A lock
        // not taken within 2 s skips this write: the file keeps what it has. Returns the merged history, or null.
        static DwmObservations Commit(string path, bool admin, DwmObservations history, DwmReading now, string observer, string nowUtc)
        {
            string temp = null;
            try
            {
                using (var m = new Mutex(false, LockName(path)))
                {
                    bool held;
                    try { held = m.WaitOne(2000); } catch (AbandonedMutexException) { held = true; }
                    if (!held) return null;
                    try
                    {
                        var plan = Recovery.PlanCommit(Load(path, admin), history, now, observer, nowUtc);
                        if (plan == null) return null;
                        if (!plan.Write) return plan.Merged;
                        if (admin) RecoveryRunner.PrepareDirectory();
                        Directory.CreateDirectory(Path.GetDirectoryName(path));
                        temp = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
                        File.WriteAllText(temp, new JavaScriptSerializer().Serialize(plan.File));
                        if (admin)
                        {
                            var owner = new FileSecurity();
                            owner.SetOwner(new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null));
                            File.SetAccessControl(temp, owner);
                        }
                        if (File.Exists(path)) File.Replace(temp, path, null, true);
                        else File.Move(temp, path);
                        temp = null;
                        return plan.Merged;
                    }
                    finally { m.ReleaseMutex(); }
                }
            }
            catch (Exception) { return null; }
            finally { if (temp != null) try { File.Delete(temp); } catch (Exception) { } }
        }

        static bool Trusted(string path)
        {
            var trusted = new[] { new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null), new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null) };
            return trusted.Contains(File.GetAccessControl(path, AccessControlSections.Owner).GetOwner(typeof(SecurityIdentifier)));
        }

        // The DWM of the active session against what observers saw before in this session. With an observer the
        // merged record is saved when it changed: as administrator to the control directory, otherwise to the user's
        // copy.
        public static void ReadCompositor(RecoverySnapshot s, string observer, int? session = null)
        {
            try
            {
                s.DwmNow = ReadDwm(session);
                string nowUtc = Stamp(DateTime.UtcNow);
                var user = Load(UserObservations, false);
                var machine = StateOverride == null ? Load(MachineObservations, true) : new List<DwmObservations>();
                var installer = LoadInstallerBaseline(s.DwmNow);
                s.DwmHistory = Recovery.Observe(user.Concat(machine).Concat(new[] { installer }), s.DwmNow, observer ?? "dry-run", nowUtc);
                if (s.DwmHistory == null || observer == null) return;
                bool admin = Program.IsElevated() && StateOverride == null;
                s.DwmHistory = Commit(admin ? MachineObservations : UserObservations, admin, s.DwmHistory, s.DwmNow, observer, nowUtc) ?? s.DwmHistory;
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


        // The scheduled task through the Task Scheduler's scripting objects, late bound (no interop assembly).
        // Two sources, the newest run wins (Recovery.NewestTaskRun): the COM object and the WMI provider that
        // Get-ScheduledTaskInfo uses.
        static void ReadTask(RecoverySnapshot s)
        {
            var com = ReadTaskCom();
            var wmi = ReadTaskWmi();
            s.TaskFound = com != null || wmi != null;
            var best = Recovery.NewestTaskRun(new[] { com, wmi });
            if (best == null) return;
            s.TaskResult = best.Result;
            s.TaskLastRun = Recovery.TaskLastRunText(best);
        }

        static TaskRun ReadTaskCom()
        {
            object service = null;
            try
            {
                var type = Type.GetTypeFromProgID("Schedule.Service");
                service = Activator.CreateInstance(type);
                Call(service, "Connect");
                var folder = Call(service, "GetFolder", @"\");
                var task = Call(folder, "GetTask", TaskName);
                return new TaskRun { Result = Convert.ToInt64(Get(task, "LastTaskResult")), LastRun = (DateTime)Get(task, "LastRunTime"), Source = "com" };
            }
            catch (Exception) { return null; }
            finally { if (service != null) System.Runtime.InteropServices.Marshal.FinalReleaseComObject(service); }
        }

        // PS_ScheduledTask.GetInfoByName (root\Microsoft\Windows\TaskScheduler), as Get-ScheduledTaskInfo calls it.
        static TaskRun ReadTaskWmi()
        {
            try
            {
                using (var c = new System.Management.ManagementClass(@"root\Microsoft\Windows\TaskScheduler", "PS_ScheduledTask", null))
                using (var input = c.GetMethodParameters("GetInfoByName"))
                {
                    input["TaskName"] = TaskName;
                    input["TaskPath"] = @"\";
                    using (var output = c.InvokeMethod("GetInfoByName", input, null))
                    {
                        var info = output == null ? null : output["CmdletOutput"] as System.Management.ManagementBaseObject;
                        if (info == null) return null;
                        var last = info["LastRunTime"] as string;
                        var result = info["LastTaskResult"];
                        return new TaskRun
                        {
                            LastRun = string.IsNullOrEmpty(last) ? (DateTime?)null : System.Management.ManagementDateTimeConverter.ToDateTime(last),
                            Result = result == null ? (long?)null : (int)Convert.ToUInt32(result, CultureInfo.InvariantCulture),    // signed, as the COM object gives it
                            Source = "wmi",
                        };
                    }
                }
            }
            catch (Exception) { return null; }
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

    // One DWM process opened by id and checked through the same handle (reviewer 911): while the handle is open the id
    // cannot name another process, so what the checks saw is what the handle reads or stops.
    static class DwmHandle
    {
        public const uint Terminate = 0x0001, VmRead = 0x0010, QueryInformation = 0x0400, QueryLimited = 0x1000, Synchronize = 0x00100000;
        const int StillActive = 259;

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern SafeProcessHandle OpenProcess(uint access, bool inherit, int pid);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool GetProcessTimes(SafeProcessHandle process, out long creation, out long exit, out long kernel, out long user);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool ProcessIdToSessionId(int pid, out int session);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool GetExitCodeProcess(SafeProcessHandle process, out int code);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool TerminateProcess(SafeProcessHandle process, uint code);

        [DllImport("kernel32.dll", SetLastError = true, EntryPoint = "K32EnumProcessModulesEx")]
        static extern bool EnumProcessModulesEx(SafeProcessHandle process, [Out] IntPtr[] modules, int bytes, out int needed, int filter);

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode, EntryPoint = "K32GetModuleFileNameExW")]
        static extern int GetModuleFileNameEx(SafeProcessHandle process, IntPtr module, StringBuilder name, int size);

        // The handle when the process is the reading's DWM (id, session, creation time) and still runs; null otherwise.
        public static SafeProcessHandle Open(DwmReading r, uint access, out string why)
        {
            why = null;
            if (!Recovery.Complete(r)) { why = "the DWM of the active session cannot be read"; return null; }
            var h = OpenProcess(access, false, r.Pid.Value);
            if (h.IsInvalid)
            {
                int error = Marshal.GetLastWin32Error();
                h.Dispose();
                why = error == 5 ? "DWM's files can only be checked as administrator" : "DWM process " + r.Pid + " cannot be opened (error " + error + ")";
                return null;
            }
            if (!Matches(h, r)) { h.Dispose(); why = "process " + r.Pid + " is no longer the DWM that was read"; return null; }
            return h;
        }

        public static bool Matches(SafeProcessHandle h, DwmReading r)
        {
            long creation, exit, kernel, user; int session, code;
            return GetProcessTimes(h, out creation, out exit, out kernel, out user) && Recovery.Stamp(DateTime.FromFileTimeUtc(creation)) == r.CreatedUtc &&
                ProcessIdToSessionId(r.Pid.Value, out session) && session == r.Session.Value && GetExitCodeProcess(h, out code) && code == StillActive;
        }

        // The module files of the process; null when they cannot be listed.
        // The complete module list (Recovery.CompleteModuleList), or null. A name that fills the buffer may be cut, so it
        // counts as unreadable.
        public static List<string> Modules(SafeProcessHandle h)
        {
            return Recovery.CompleteModuleList((IntPtr[] buffer, out int needed) => EnumProcessModulesEx(h, buffer, buffer.Length * IntPtr.Size, out needed, 3),
                module =>
                {
                    var name = new StringBuilder(32768);
                    int length = GetModuleFileNameEx(h, module, name, name.Capacity);
                    return length > 0 && length < name.Capacity - 1 ? name.ToString() : null;
                }, IntPtr.Size);
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
            public readonly Recovery.PlanArgs More = new Recovery.PlanArgs();
        }

        static Options Parse(string[] args)
        {
            if (args.Length < 2 || args[0] != "--action") return null;
            var o = new Options { Action = args[1] };
            for (int i = 2; i < args.Length; i++)
            {
                string a = args[i];
                uint n;
                uint[] fanC, fanPct;
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
                else if (a == "--cu" && (args[i + 1] == "24" || args[i + 1] == "40")) o.More.Cu = uint.Parse(args[++i], CultureInfo.InvariantCulture);
                else if (a == "--games" && (args[i + 1] == "keep" || args[i + 1] == "reset")) o.More.Games = args[++i];
                else if (a == "--image" && Profiles.IsValidImage(args[i + 1])) o.More.Image = args[++i];
                else if (a == "--value" && (args[i + 1].Length == 0 || Profiles.IsValidValue(args[i + 1]))) o.More.Value = args[++i];
                else if (a == "--gfx" && GraphicsSettings.ParseEdits(args[i + 1]) != null) o.More.Gfx = args[++i];
                else if (a == "--curve" && TunerPlan.ParseCurve(args[i + 1]) != null) o.More.Curve = args[++i];
                else if (a == "--window" && uint.TryParse(args[i + 1], NumberStyles.None, CultureInfo.InvariantCulture, out n)) { o.More.Window = n; i++; }
                else if (a == "--cpu-clock" && uint.TryParse(args[i + 1], NumberStyles.None, CultureInfo.InvariantCulture, out n)) { o.More.CpuClock = n; i++; }
                else if (a == "--cpu-uv" && uint.TryParse(args[i + 1], NumberStyles.None, CultureInfo.InvariantCulture, out n)) { o.More.CpuUv = n; i++; }
                else if (a == "--cpu-temp" && uint.TryParse(args[i + 1], NumberStyles.None, CultureInfo.InvariantCulture, out n)) { o.More.CpuTemp = n; i++; }
                else if (a == "--cores" && uint.TryParse(args[i + 1], NumberStyles.None, CultureInfo.InvariantCulture, out n)) { o.More.Cores = n; i++; }
                else if (a == "--fan-profile" && FanPlan.ValidProfileName(args[i + 1])) o.More.FanProfile = args[++i];
                else if (a == "--fan-curve" && FanCurves.ParseCurve(args[i + 1], out fanC, out fanPct)) o.More.FanCurve = args[++i];
                else if (a == "--fan-test-pct" && uint.TryParse(args[i + 1], NumberStyles.None, CultureInfo.InvariantCulture, out n)) { o.More.FanTestPct = n; i++; }
                else if (a == "--tdr" && uint.TryParse(args[i + 1], NumberStyles.None, CultureInfo.InvariantCulture, out n)) { o.More.Tdr = n; i++; }
                else return null;
            }
            // set-clocks says what happens to both values ("unset" removes one); no other action takes a mode.
            if (o.Action == "set-clocks" && (!o.ModeGiven || !o.CeilingGiven)) return null;
            if (o.Action != "set-clocks" && o.ModeGiven) return null;
            // Each of the newer options belongs to its actions only.
            if ((o.More.Cu != null) != (o.Action == "cu-mode")) return null;
            if (o.More.Games != null && o.Action != "reset-defaults") return null;
            if ((o.More.Image != null) != o.Action.StartsWith("game-", StringComparison.Ordinal)) return null;
            // game-profile changes the switches (--value), the graphics settings (--gfx) or both; graphics-defaults
            // takes --gfx only.
            if (o.More.Value != null && o.Action != "game-profile") return null;
            if (o.Action == "game-profile" && o.More.Value == null && o.More.Gfx == null) return null;
            if ((o.More.Gfx != null) != (o.Action == "graphics-defaults") && o.Action != "game-profile") return null;
            // Each tuning option belongs to one action, so a stray value never travels with a different request.
            if ((o.More.Curve != null) != (o.Action == "tune-trial")) return null;
            if (o.More.Window != null && o.Action != "tune-trial" && o.Action != "cpu-trial") return null;
            if ((o.More.CpuClock != null || o.More.CpuUv != null || o.More.CpuTemp != null) && o.Action != "cpu-trial") return null;
            if ((o.More.Cores != null) != (o.Action == "core-mask")) return null;
            // The fan card: a profile for fan-curve only, and a curve only with the custom profile.
            if ((o.More.FanProfile != null) != (o.Action == "fan-curve")) return null;
            if ((o.More.FanCurve != null) != (o.More.FanProfile == "custom")) return null;
            if ((o.More.FanTestPct != null) != (o.Action == "fan-test")) return null;
            // The waiting time for the graphics belongs to its own action only.
            if ((o.More.Tdr != null) != (o.Action == "tdr-delay")) return null;
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
                Console.Error.WriteLine("       --action cu-mode --cu 24|40 ...   --action reset-defaults [--games keep|reset] ...");
                Console.Error.WriteLine("       --action game-profile --image <name.exe> [--value <switches or \"\">] [--gfx <settings>] ...   --action game-undo|game-redo --image <name.exe> ...");
                Console.Error.WriteLine("       --action graphics-defaults --gfx <settings> ...   (settings: Name=value,... or Name=unset, for example FrameRateLimit=60,VSync=unset)");
                Console.Error.WriteLine("       --action tune-trial --curve <11 values in mV> [--window ms] ...   --action tune-keep|tune-stop|tune-reset ...");
                Console.Error.WriteLine("       --action cpu-trial [--cpu-clock MHz] [--cpu-uv steps] [--cpu-temp C] [--window ms] ...   --action core-mask --cores 6|8 ...");
                Console.Error.WriteLine("       --action fan-auto ...   --action fan-curve --fan-profile standard|quiet|performance|custom [--fan-curve C:pct,...] ...");
                Console.Error.WriteLine("       --action fan-test --fan-test-pct 30..100 ...   (one duty for 10 s, then the choice in force again)");
                Console.Error.WriteLine("       --action tdr-delay --tdr " + TdrSetting.Min + ".." + TdrSetting.Max + "   (seconds Windows waits for the graphics; applies at the next restart of Windows)");
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
            var plan = Recovery.Plan(o.Action, s, o.Mode, o.Ceiling, backups, o.AcceptBd060, o.More);
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

        static string _runId, _action;
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
            _runId = o.RunId; _action = o.Action;
            try { PrepareDirectory(); }
            catch (Exception e) { Fail("cannot prepare " + RecoveryProbe.ControlDirectory + ": " + e.Message); return Failed; }
            Log("start " + o.Action + (o.Mode != null ? " mode " + o.Mode : "") + (o.Ceiling != null ? " ceiling " + o.Ceiling : "") +
                (o.More.Cu != null ? " cu " + o.More.Cu : "") + (o.More.Games != null ? " games " + o.More.Games : "") + (o.More.Image != null ? " image " + o.More.Image : "") +
                (o.More.Value != null ? " value \"" + o.More.Value + "\"" : "") + (o.More.Gfx != null ? " gfx " + o.More.Gfx : "") +
                (o.More.Curve != null ? " curve " + o.More.Curve : "") + (o.More.Window != null ? " window " + o.More.Window : "") +
                (o.More.CpuClock != null ? " cpu-clock " + o.More.CpuClock : "") + (o.More.CpuUv != null ? " cpu-uv " + o.More.CpuUv : "") +
                (o.More.CpuTemp != null ? " cpu-temp " + o.More.CpuTemp : "") + (o.More.Cores != null ? " cores " + o.More.Cores : "") +
                (o.More.Tdr != null ? " tdr " + o.More.Tdr : "") +
                ", " + Program.ProductName + " " + Program.VersionText);
            try
            {
                var s = RecoveryProbe.Read(o.Action == Recovery.OperatorEscape ? "escape" : "helper");
                var plan = Recovery.Plan(o.Action, s, o.Mode, o.Ceiling, RecoveryProbe.Backups(), o.AcceptBd060, o.More);
                if (plan.Refused) { Log("result: refused: " + plan.Refusal); return Refused; }
                if (plan.RestartCompositor) return StopCompositor(s);
                if (plan.CuConfirm) return ConfirmCu();
                if (plan.Tune != null) return RunTune(plan);
                var backup = Backup(o, plan, s);
                Log("backup " + backup.File);

                if (plan.ConfirmStart)
                {
                    var why = Confirm();
                    if (why != null) { Log("result: failed: " + why); return Failed; }
                }
                if (!Apply(plan.Writes, backup)) { Log("result: failed: a write did not read back; the old values were restored"); return Failed; }
                if (!ApplyGames(plan.GameWrites, backup)) { Log("result: failed: a game setting did not read back; the old game settings were restored"); return Failed; }
                if (plan.Cu != null && !RunCu(plan.Cu)) return Failed;
                if (plan.TuneSteps.Count > 0 && !RunTuneSteps(plan)) return Failed;
                // A change that applies at the next restart is written, not done: the active route stays until then.
                Log(plan.OfferRestart ? "result: written, pending until the next restart of Windows: " + plan.Change
                    : "result: done: " + plan.Change + " Takes effect " + plan.Effect + ".");
                return Done;
            }
            catch (Exception e) { Log("result: failed: " + e.GetType().Name + ": " + e.Message); return Failed; }
        }

        // The operator escape (restart-compositor --accept-bd060): stops the DWM of the active session and waits up to
        // 20 s for the new one, which Windows starts. The only place in this app that stops DWM (BD-060). One handle is
        // opened, checked against the reading (id, session, creation time, still running) and terminated: the id
        // cannot name another process while that handle is open. The wait stays on the same session and accepts only
        // a DWM created after the stopped one (reviewer 911).
        static int StopCompositor(RecoverySnapshot s)
        {
            var old = s.DwmNow;
            if (!Recovery.Complete(old)) { Log("result: failed: the DWM of the active session cannot be read"); return Failed; }
            int session = old.Session.Value;
            Log("warning: after a DWM restart some Windows 11 apps can ignore mouse clicks until Windows restarts (BD-060)");
            string why;
            using (var h = DwmHandle.Open(old, DwmHandle.QueryLimited | DwmHandle.Terminate | DwmHandle.Synchronize, out why))
            {
                if (h == null) { Log("result: failed: " + why + "; nothing was stopped"); return Failed; }
                Log("stopping DWM process " + old.Pid + " (started " + old.CreatedUtc + ") in session " + session);
                if (!DwmHandle.TerminateProcess(h, 1)) { Log("result: failed: TerminateProcess error " + Marshal.GetLastWin32Error()); return Failed; }
            }
            var stopped = Recovery.Utc(old.CreatedUtc).Value.AddMilliseconds(1);
            var clock = Stopwatch.StartNew();
            while (clock.Elapsed.TotalSeconds < 20)
            {
                Thread.Sleep(500);
                var fresh = Starts("dwm.exe").Where(x => x.Session == session && x.Utc >= stopped && !(x.Pid == old.Pid && Stamp(x.Utc) == old.CreatedUtc))
                    .OrderBy(x => x.Utc).LastOrDefault();
                if (fresh != null)
                {
                    var after = new RecoverySnapshot();
                    RecoveryProbe.ReadCompositor(after, "escape", session);
                    Log("new DWM process " + fresh.Pid + " started " + Stamp(fresh.Utc) + " in session " + session + "; " + Recovery.CompositorStatusLine(after));
                    Log("result: done: the desktop compositor was restarted. Restart Windows as soon as you can (BD-060).");
                    return Done;
                }
            }
            Log("result: failed: no new DWM in session " + session + " after 20 s; restart Windows");
            return Failed;
        }

        static List<RecoveryProbe.Started> Starts(string image) { return RecoveryProbe.Starts(image); }
        static string Stamp(DateTime? utc) { return Recovery.Stamp(utc); }

        // route-written.json: removed before a DwmForceCpu write, written after its read-back (reviewer 911).
        static void RouteRecordBefore(List<RegWrite> writes)
        {
            if (!writes.Any(IsRoute)) return;
            if (File.Exists(RecoveryProbe.RouteRecordPath)) { File.Delete(RecoveryProbe.RouteRecordPath); Log("route record removed before the write"); }
        }

        static void RouteRecordAfter(List<RegWrite> writes)
        {
            var w = writes.LastOrDefault(IsRoute);
            if (w == null) return;
            try
            {
                var r = new RouteRecord { Schema = 1, Utc = Stamp(DateTime.UtcNow), Value = w.Delete ? (long?)null : w.Number, Action = _action, RunId = _runId };
                var path = RecoveryProbe.RouteRecordPath;
                var temp = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
                File.WriteAllText(temp, new JavaScriptSerializer().Serialize(r));
                var owner = new FileSecurity();
                owner.SetOwner(new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null));
                File.SetAccessControl(temp, owner);
                if (File.Exists(path)) File.Replace(temp, path, null, true); else File.Move(temp, path);
                Log("route record: DwmForceCpu " + (r.Value == null ? "removed" : r.Value.Value.ToString(CultureInfo.InvariantCulture)) + " read back at " + r.Utc);
            }
            catch (Exception e) { Log("route record not written (" + e.Message + "): the route timing reads as unknown"); }
        }

        static bool IsRoute(RegWrite w) { return w.Path == Recovery.RouterPath && w.Name == "DwmForceCpu"; }

        static BackupRecord Backup(Options o, ActionPlan plan, RecoverySnapshot s)
        {
            var b = new BackupRecord
            {
                Action = o.Action, Utc = DateTime.UtcNow.ToString("o", CultureInfo.InvariantCulture), RunId = o.RunId,
                Args = new[] { o.Mode != null ? "mode=" + o.Mode : null, o.Ceiling != null ? "ceiling=" + o.Ceiling : null, o.More.Cu != null ? "cu=" + o.More.Cu : null,
                    o.More.Games != null ? "games=" + o.More.Games : null, o.More.Image != null ? "image=" + o.More.Image : null,
                    o.More.Gfx != null ? "gfx=" + o.More.Gfx : null }.Where(a => a != null).ToArray(),
                Undoable = plan.Undoable, Undoes = plan.UndoOf,
            };
            // A key removal has no value of its own: restoring the key's values brings the key back.
            var names = plan.Writes.Where(w => !w.DeleteKey).Select(w => new[] { w.Path, w.Name }).ToList();
            if (plan.ConfirmStart)
                foreach (var n in new[] { "UnconfirmedStarts", "DpmPending" }) names.Add(new[] { Recovery.ParametersPath, n });   // a record only
            names.AddRange(plan.GameWrites.Keys.Select(image => new[] { Recovery.GamePath(image), Profiles.ValueName }));
            foreach (var pn in names) b.Values.Add(Saved(pn[0], pn[1], false));
            // The CU setter's values: for diagnosis only (S6), never restored.
            if (plan.Cu != null)
                foreach (var n in CuMode.ValueNames) b.Diagnosis.Add(Saved(Recovery.ParametersPath, n, true));
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

        // One value as found before an action. diagnosis: an unexpected type is recorded, not refused.
        static BackupValue Saved(string path, string name, bool diagnosis)
        {
            using (var k = SettingsStore.Machine.OpenSubKey(path))
            {
                var v = new BackupValue { Path = path, Name = name };
                object data = k == null ? null : k.GetValue(name, null, RegistryValueOptions.DoNotExpandEnvironmentNames);
                if (data == null) return v;
                v.Existed = true;
                var kind = k.GetValueKind(name);
                if (kind == RegistryValueKind.DWord) { v.Kind = "DWord"; v.Number = (uint)(int)data; }
                else if (kind == RegistryValueKind.QWord) { v.Kind = "QWord"; v.Number = (long)data; }
                else if (kind == RegistryValueKind.String) { v.Kind = "String"; v.Text = (string)data; }
                else if (diagnosis) { v.Kind = kind.ToString(); }
                else throw new InvalidOperationException(name + " has registry type " + kind + ", which this app does not restore; nothing was changed");
                return v;
            }
        }

        // Per-game switches (WU-015, WU-020a): the value set or the game's key removed, read back; a failure restores
        // the games' old values from the backup (they are the app's own values).
        static bool ApplyGames(SortedDictionary<string, string> games, BackupRecord backup)
        {
            if (games.Count == 0) return true;
            try
            {
                foreach (var g in games)
                {
                    if (!Recovery.GameAllowed(g.Key, g.Value)) throw new InvalidOperationException(g.Key + " is not a game this app may change");
                    if (g.Value.Length == 0) SettingsStore.RemoveProfile(g.Key); else SettingsStore.WriteProfile(g.Key, g.Value);
                    Log("wrote: game " + g.Key + " = \"" + g.Value + "\"" + (g.Value.Length == 0 ? " (key removed)" : ""));
                }
                bool ok = true;
                var now = SettingsStore.ReadProfiles();
                foreach (var g in games)
                {
                    string v;
                    bool back = g.Value.Length == 0 ? !now.ContainsKey(g.Key) : now.TryGetValue(g.Key, out v) && v == g.Value;
                    Log((back ? "read back OK: game " : "read back MISMATCH: game ") + g.Key);
                    ok &= back;
                }
                if (ok) return true;
            }
            catch (Exception e) { Log("game write failed: " + e.Message); }
            foreach (var v in backup.Values.Where(x => Recovery.GameImage(x.Path) != null))
                try
                {
                    var image = Recovery.GameImage(v.Path);
                    if (v.Existed && !string.IsNullOrEmpty(v.Text)) SettingsStore.WriteProfile(image, v.Text); else SettingsStore.RemoveProfile(image);
                    Log("restored: game " + image);
                }
                catch (Exception e) { Log("restore failed: game " + v.Path + ": " + e.Message); }
            return false;
        }

        // The CU setter (plan v7 section 7, S1-S6) on the live key: stops at the first failed step, never restores,
        // reports the read-back state.
        static bool RunCu(CuSetterPlan plan)
        {
            Log("CU setter: target " + plan.Target + ", before: " + plan.Before);
            CuSetterResult r;
            try
            {
                using (var api = new Win32RegApi(CuMode.ParametersPath)) r = CuMode.Execute(plan, new CheckedCuRegistry(api));
            }
            catch (Exception e)
            {
                Log("result: failed: the driver's settings key cannot be opened for the graphics-core change (" + e.Message + "). Nothing was changed.");
                return false;
            }
            foreach (var l in r.Log) Log("CU " + l);
            CuUnflushed(r);
            var text = CuMode.ResultText(plan, r);
            if (r.Completed) { Log("CU result: " + text); return true; }
            Log("CU read back: " + (r.ReadBack != null ? r.ReadBack.ToString() : "unknown") + (r.FlushFailed ? " (the flush failed: not durable)" : ""));
            Log("result: failed: " + text + " Error: " + r.Error);
            return false;
        }

        // ---- the tuning page (docs/design/tuner.md) -----------------------------------------------------------------
        // These runs write no registry value and take no backup. Each one reads the surface, sends with the Generation
        // of that read, and reads what came back, so the log carries what the driver did and not what was asked for.
        // The driver owns the trial window and the revert: a helper that exits, a crash and a power cut all end with
        // the stored values back.
        static int RunTune(ActionPlan plan)
        {
            if (plan.Tune.Kind.StartsWith("fan-", StringComparison.Ordinal)) return RunFan(plan);
            return plan.Tune.Kind.StartsWith("curve-", StringComparison.Ordinal) ? RunCurve(plan) : RunCpu(plan);
        }

        // The tuning steps of an action that is not itself a tuning action (reset-defaults, WU-042). Each step is
        // one escape, in the planned order, and the first refusal stops the rest: a half-reset is a state nobody
        // chose, and what already went back stays back.
        static bool RunTuneSteps(ActionPlan plan)
        {
            foreach (var t in plan.TuneSteps)
            {
                var step = new ActionPlan
                {
                    Action = plan.Action, Tune = t, Effect = plan.Effect,
                    Change = "the " + (t.Kind == "curve-reset" ? "voltage curve" : t.Kind == "core-mask" ? "core mask"
                        : t.Kind.StartsWith("fan-", StringComparison.Ordinal) ? "fan choice"
                        : "processor settings") + " step of " + plan.Action + " (" + t.Kind + ")",
                };
                if (RunTune(step) != Done) return false;
            }
            return true;
        }

        static string CurveLine(CurveState c)
        {
            return "flags " + c.Flags + ", mode " + c.Mode + ", trial " + c.TrialRemainingMs + " ms left of " + c.TrialMs +
                ", active " + TunerPlan.CurveText(c.Active) + ", stored " + (c.Has(CurveState.FlagStored) ? TunerPlan.CurveText(c.Stored) : "none") +
                ", level " + c.Level + " (" + c.LevelMHz + " MHz, " + c.LevelMv + " mV), observed " + c.ObservedMHz + " MHz vid " + c.ObservedVid +
                ", last reason " + c.Error + " at level " + c.ErrorLevel + ", sets " + c.Sets + " keeps " + c.Keeps + " cancels " + c.Cancels + " reverts " + c.Reverts;
        }

        static string CpuLine(CpuState u)
        {
            return "flags " + u.Flags + ", applied " + u.AppliedMaxMHz + " MHz/" + u.AppliedUvSteps + " steps/" + u.AppliedTempC + " C" +
                ", stored " + u.StoredMaxMHz + "/" + u.StoredUvSteps + "/" + u.StoredTempC +
                ", baseline " + u.BaselineMaxMHz + "/" + u.BaselineUvSteps + "/" + u.BaselineTempC +
                ", voltage " + u.VoltageMv + " mV, cap " + u.CapC + " C, cores " + u.Cores + "/" + u.Threads +
                ", mask " + u.CoreMask + " stored " + u.CoreMaskStored + ", trial " + u.TrialRemainingMs + " ms left" +
                ", last queue " + u.LastQueue + " message " + u.LastMessage + " status " + u.LastStatus +
                ", reads " + u.Reads + " writes " + u.Writes + " refusals " + u.Refusals + " reverts " + u.Reverts;
        }

        static int RunCurve(ActionPlan plan)
        {
            var t = plan.Tune;
            var before = Kmd.Curve();
            if (before.Value == null) { Log("result: failed: no voltage-curve reading: " + before.Error); return Failed; }
            Log("curve before: " + CurveLine(before.Value));
            KmdResult<CurveState> sent;
            switch (t.Kind)
            {
                case "curve-trial":
                    Log("curve trial: " + TunerPlan.CurveText(t.Mv) + ", window " + t.WindowMs + " ms, generation " + before.Value.Generation);
                    sent = Kmd.CurveTrial(before.Value.Generation, t.Mv, t.WindowMs);
                    break;
                case "curve-keep": sent = Kmd.CurveOp(Kmd.CurveOpKeep, before.Value.Generation); break;
                case "curve-stop": sent = Kmd.CurveOp(Kmd.CurveOpCancel, before.Value.Generation); break;
                default: sent = Kmd.CurveOp(Kmd.CurveOpReset, before.Value.Generation); break;
            }
            if (sent.Value == null) { Log("result: failed: the driver did not take the voltage change: " + sent.Error); return Failed; }
            Log("curve after: " + CurveLine(sent.Value));
            var c = sent.Value;
            string wrong = null;
            switch (t.Kind)
            {
                case "curve-trial":
                    if (!c.Has(CurveState.FlagOnTrial)) wrong = "no trial is running after the request";
                    else if (TunerPlan.CurveText(c.Candidate) != TunerPlan.CurveText(t.Mv)) wrong = "the driver put another curve on trial: " + TunerPlan.CurveText(c.Candidate);
                    break;
                case "curve-keep":
                    if (c.Has(CurveState.FlagOnTrial) || !c.Has(CurveState.FlagStored)) wrong = "the curve was not stored";
                    break;
                case "curve-stop":
                    if (c.Has(CurveState.FlagOnTrial)) wrong = "the trial is still running";
                    break;
                default:
                    if (c.Has(CurveState.FlagStored) || c.Has(CurveState.FlagOnTrial)) wrong = "a stored curve is still there";
                    break;
            }
            if (wrong != null) { Log("result: failed: " + wrong + " (last reason " + c.Error + ")"); return Failed; }
            Log("result: done: " + plan.Change + " Takes effect " + plan.Effect + ".");
            return Done;
        }

        static int RunCpu(ActionPlan plan)
        {
            var t = plan.Tune;
            var before = Kmd.Cpu();
            if (before.Value == null) { Log("result: failed: no processor reading: " + before.Error); return Failed; }
            Log("processor before: " + CpuLine(before.Value));
            var r = new Kmd.CpuRequest { ExpectedGeneration = before.Value.Generation };
            switch (t.Kind)
            {
                case "cpu-readback": r.Op = Kmd.CpuOpReadback; break;
                case "cpu-trial":
                    r.Op = Kmd.CpuOpSet;
                    r.TrialMs = t.WindowMs;
                    if (t.MaxMHz != null) { r.Given |= CpuState.GivenMax; r.MaxMHz = t.MaxMHz.Value; }
                    if (t.UvSteps != null) { r.Given |= CpuState.GivenUv; r.UvSteps = t.UvSteps.Value; }
                    if (t.TempC != null) { r.Given |= CpuState.GivenTemp; r.TempC = t.TempC.Value; }
                    Log("processor trial: given " + r.Given + ", " + r.MaxMHz + " MHz, " + r.UvSteps + " steps, " + r.TempC +
                        " C, window " + r.TrialMs + " ms, generation " + r.ExpectedGeneration);
                    break;
                case "cpu-keep": r.Op = Kmd.CpuOpKeep; break;
                case "cpu-stop": r.Op = Kmd.CpuOpCancel; break;
                case "core-mask": r.Op = Kmd.CpuOpCores; r.CoreMask = t.CoreMask; break;
                default: r.Op = Kmd.CpuOpReset; break;
            }
            var sent = Kmd.CpuRequestOp(r);
            if (sent.Value == null) { Log("result: failed: the driver did not take the processor request: " + sent.Error); return Failed; }
            Log("processor after: " + CpuLine(sent.Value));
            var u = sent.Value;
            string wrong = null;
            switch (t.Kind)
            {
                case "cpu-readback":
                    // The readback is what admits every later write: without an answer from the processor's own queue
                    // nothing else on this card may run (docs/design/tuner.md).
                    if (!u.Has(CpuState.FlagQueue3Proven)) wrong = "the processor did not answer its getters";
                    break;
                case "cpu-trial":
                    if (!u.Has(CpuState.FlagOnTrial)) wrong = "no trial is running after the request";
                    break;
                case "cpu-keep":
                    if (u.Has(CpuState.FlagOnTrial) || !u.Has(CpuState.FlagStored)) wrong = "the processor settings were not stored";
                    break;
                case "cpu-stop":
                    if (u.Has(CpuState.FlagOnTrial)) wrong = "the trial is still running";
                    break;
                case "core-mask":
                    if (u.CoreMaskStored != t.CoreMask) wrong = "the core count was not written";
                    break;
                default:
                    if (u.Has(CpuState.FlagStored) || u.Has(CpuState.FlagOnTrial)) wrong = "stored processor settings are still there";
                    break;
            }
            if (wrong != null) { Log("result: failed: " + wrong + " (reason " + u.Error + ")"); return Failed; }
            Log(plan.OfferRestart ? "result: written, pending until the next restart of Windows: " + plan.Change
                : "result: done: " + plan.Change + " Takes effect " + plan.Effect + ".");
            return Done;
        }

        // The fan card (docs/design/fan.md Part B). The request is stored by the driver (Store 1) and applied at its next
        // step, at most a second later; the reply already carries the choice in force and the stored one.
        static int RunFan(ActionPlan plan)
        {
            var t = plan.Tune;
            var before = Kmd.Fan();
            if (before.Value == null) { Log("result: failed: no fan reading: " + before.Error); return Failed; }
            Log("fan before: " + FanCurves.ReportLine(before.Value));
            if (t.Kind == "fan-fixed") return RunFanTest(plan, before.Value);
            var r = Kmd.NewFanRequest(t.Kind == "fan-board" ? Kmd.FanOpBoard : Kmd.FanOpCurve);
            r.ExpectedGeneration = before.Value.Generation;
            r.Store = 1;
            if (t.Kind == "fan-curve")
            {
                r.Profile = t.FanProfile;
                if (t.FanC != null)
                {
                    r.Points = (uint)t.FanC.Length;
                    for (int i = 0; i < t.FanC.Length; i++) { r.CurveC[i] = t.FanC[i]; r.CurvePct[i] = t.FanPct[i]; }
                }
            }
            Log("fan request: " + t.Kind + ", profile " + r.Profile + ", points " + r.Points + ", generation " + r.ExpectedGeneration);
            var sent = Kmd.FanRequestOp(r);
            if (sent.Value == null) { Log("result: failed: the driver did not take the fan choice: " + sent.Error); return Failed; }
            var f = sent.Value;
            Log("fan after: " + FanCurves.ReportLine(f));
            string wrong = null;
            if (!f.Has(FanState.FlagStored)) wrong = "the choice was not stored";
            else if (t.Kind == "fan-board" && (f.StoredMode != FanState.ModeBoard || f.Mode != FanState.ModeBoard)) wrong = "the board does not have the fan";
            else if (t.Kind == "fan-curve" && (f.StoredMode != FanState.ModeCurve || f.Mode != FanState.ModeCurve || f.Profile != t.FanProfile))
                wrong = "another fan choice is in force";
            else if (t.FanC != null && FanCurves.CurveText(FanCurves.ShownC(f), FanCurves.ShownPct(f)) != FanCurves.CurveText(t.FanC, t.FanPct))
                wrong = "the driver runs another curve: " + FanCurves.CurveText(FanCurves.ShownC(f), FanCurves.ShownPct(f));
            if (wrong != null) { Log("result: failed: " + wrong + " (error " + f.Error + ")"); return Failed; }
            Log("result: done: " + plan.Change + " Takes effect " + plan.Effect + ".");
            return Done;
        }

        // The card's short test: FIXED under a lease (as `bc250kmd_cli fan set` sends it), one reading a second while it
        // holds, then the choice that was in force again, durable and not stored (it is stored already). The lease
        // outlives the test, so a helper that dies here leaves the fan with the board, which is rule 9 of fan.md.
        static int RunFanTest(ActionPlan plan, FanState before)
        {
            var t = plan.Tune;
            var r = Kmd.NewFanRequest(Kmd.FanOpFixed);
            r.ExpectedGeneration = before.Generation;
            r.FixedPct = t.FixedPct;
            r.LeaseMs = t.LeaseMs;
            r.Store = 0;
            Log("fan request: fixed " + r.FixedPct + " %, lease " + r.LeaseMs + " ms, generation " + r.ExpectedGeneration);
            var sent = Kmd.FanRequestOp(r);
            if (sent.Value == null) { Log("result: failed: the driver did not take the test speed: " + sent.Error); return Failed; }
            Log("fan after: " + FanCurves.ReportLine(sent.Value));
            uint maxRpm = 0, minRpm = uint.MaxValue;
            bool held = false;
            for (uint waited = 0; waited < t.TestMs; waited += 1000)
            {
                System.Threading.Thread.Sleep(1000);
                var now = Kmd.Fan();
                if (now.Value == null) { Log("fan test: no reading at " + (waited + 1000) + " ms: " + now.Error); continue; }
                var f = now.Value;
                if (f.Mode == FanState.ModeFixed) held = true;
                maxRpm = Math.Max(maxRpm, f.Rpm); minRpm = Math.Min(minRpm, f.Rpm);
                Log("fan test: " + (waited + 1000) + " ms, state " + f.State + ", target " + f.TargetPct + " % applied " + f.AppliedPct + " %, " + f.Rpm + " rpm");
            }
            var back = Kmd.NewFanRequest(t.Then.Kind == "fan-board" ? Kmd.FanOpBoard : Kmd.FanOpCurve);
            back.ExpectedGeneration = before.Generation;
            back.Store = 0;
            back.LeaseMs = 0;
            if (t.Then.Kind == "fan-curve")
            {
                back.Profile = t.Then.FanProfile;
                if (t.Then.FanC != null)
                {
                    back.Points = (uint)t.Then.FanC.Length;
                    for (int i = 0; i < t.Then.FanC.Length; i++) { back.CurveC[i] = t.Then.FanC[i]; back.CurvePct[i] = t.Then.FanPct[i]; }
                }
            }
            Log("fan request: back to " + t.Then.Kind + ", profile " + back.Profile + ", points " + back.Points);
            var after = Kmd.FanRequestOp(back);
            if (after.Value == null) { Log("result: failed: the driver did not take the choice back: " + after.Error + "; the lease gives the fan to the board within " + t.LeaseMs / 1000 + " s"); return Failed; }
            var g = after.Value;
            // The governor applies a request at its next step, within a second; give it three.
            for (int i = 0; i < 3 && (g.Mode == FanState.ModeFixed || g.Has(FanState.FlagLeased)); i++)
            {
                System.Threading.Thread.Sleep(1000);
                var again = Kmd.Fan();
                if (again.Value != null) g = again.Value;
            }
            Log("fan after: " + FanCurves.ReportLine(g));
            string wrong = null;
            if (!held) wrong = "the test speed never ran";
            else if (t.Then.Kind == "fan-board" && g.Mode != FanState.ModeBoard) wrong = "the board does not have the fan again";
            else if (t.Then.Kind == "fan-curve" && (g.Mode != FanState.ModeCurve || g.Profile != t.Then.FanProfile || g.Has(FanState.FlagLeased)))
                wrong = "the curve is not in force again";
            if (wrong != null) { Log("result: failed: " + wrong + " (error " + g.Error + ")"); return Failed; }
            Log("fan test rpm: " + (minRpm == uint.MaxValue ? "no reading" : minRpm + " to " + maxRpm));
            Log("result: done: " + plan.Change + ".");
            return Done;
        }

        // cu-unflushed.json (plan 497 decision 1): written when a flush or the verifying read of this run failed, so
        // that the window predicts no next start in this boot; removed by a completed run. A run that failed before
        // any flush leaves an earlier record as it is.
        static void CuUnflushed(CuSetterResult r)
        {
            var path = RecoveryProbe.CuUnflushedPath;
            try
            {
                if (r.Completed) { if (File.Exists(path)) { File.Delete(path); Log("CU durability record removed: the run completed"); } return; }
                if (!r.FlushFailed && !r.VerifyFailed) return;
                var boot = DriverCardProbe.BootId();
                if (boot == null) { Log("CU durability record not written: the boot cannot be read; the window predicts from the values it reads"); return; }
                var rec = new CuUnflushedRecord { Schema = 1, BootId = boot.Value, Utc = Stamp(DateTime.UtcNow), RunId = _runId };
                var temp = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
                File.WriteAllText(temp, new JavaScriptSerializer().Serialize(rec));
                var owner = new FileSecurity();
                owner.SetOwner(new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null));
                File.SetAccessControl(temp, owner);
                if (File.Exists(path)) File.Replace(temp, path, null, true); else File.Move(temp, path);
                Log("CU durability record: boot " + rec.BootId + ", the next start is not predicted until a restart or a completed run");
            }
            catch (Exception e) { Log("CU durability record not written (" + e.Message + ")"); }
        }

        // [Confirm now] (Waiting only): CU CONFIRM with the Generation of a fresh READ, then READ again. One request,
        // never retried here: the user may press the button again.
        static int ConfirmCu()
        {
            var read = Kmd.CuMode();
            var health = Kmd.StartHealth();
            if (read.Value == null) { Log("result: failed: no graphics-core reading: " + read.Error); return Failed; }
            ulong? generation = health.Value != null ? (ulong?)health.Value.Generation : null;
            if (CuMode.Classify(read.Value, generation) != CuClass.Waiting) { Log("result: refused: no 40-core start is waiting for confirmation"); return Refused; }
            var c = Kmd.CuConfirm(read.Value.Generation);
            Log("CU CONFIRM generation " + read.Value.Generation + ": " + (c.Value != null ? "accepted" : "refused (" + c.Error + ")"));
            var after = Kmd.CuMode();
            var health2 = Kmd.StartHealth();
            var cls = CuMode.Classify(after.Value, health2.Value != null ? (ulong?)health2.Value.Generation : null);
            if (after.Value != null) Log("CU read back: flags " + after.Value.Flags + ", applied " + after.Value.Applied + ", active " + after.Value.ActiveCus + ", class " + cls);
            switch (cls)
            {
                case CuClass.Confirmed:
                case CuClass.ConfirmedFewer: Log("result: done: the 40-core start is confirmed"); return Done;
                case CuClass.Waiting: Log("result: failed: still pending (deleting Pending failed); the user may confirm again"); return Failed;
                case CuClass.NotSaved: Log("result: failed: the confirmation could not be saved (Pending deleted, Confirmed not stored)"); return Failed;
                default: Log("result: failed: the graphics-core state cannot be read after the confirmation"); return Failed;
            }
        }

        static void Write(RegWrite w)
        {
            if (w.DeleteKey) { RemoveEmptyKey(w.Path); return; }
            if (!Recovery.Allowed(w.Path, w.Name)) throw new InvalidOperationException(w.Name + " is not a value this app may write");
            // The graphics settings keys are the app's own: a write creates its key, a removal from a key that does
            // not exist has nothing to do. The driver's keys must exist.
            bool gfx = GraphicsSettings.Allowed(w.Path, w.Name);
            var key = SettingsStore.Machine.OpenSubKey(w.Path, true);
            if (key == null && gfx && !w.Delete) key = SettingsStore.Machine.CreateSubKey(w.Path, true);
            using (var k = key)
            {
                if (k == null && gfx && w.Delete) return;
                if (k == null) throw new InvalidOperationException(@"HKLM\" + w.Path + " does not exist");
                if (w.Delete) k.DeleteValue(w.Name, false);
                else if (w.Kind == "DWord") k.SetValue(w.Name, unchecked((int)(uint)w.Number), RegistryValueKind.DWord);
                else if (w.Kind == "QWord") k.SetValue(w.Name, w.Number, RegistryValueKind.QWord);
                else if (w.Kind == "String") k.SetValue(w.Name, w.Text ?? "", RegistryValueKind.String);
                else throw new InvalidOperationException("unknown value kind " + w.Kind);
            }
        }

        // A game's Graphics or Vulkan key that its last value left (the settings rule): removed only when it has no
        // value and no subkey now, else the action fails and its writes are restored.
        static void RemoveEmptyKey(string path)
        {
            if (!GraphicsSettings.KeyRemovalAllowed(path)) throw new InvalidOperationException("the key " + path + " is not one this app may remove");
            int cut = path.LastIndexOf('\\');
            string parentPath = path.Substring(0, cut), name = path.Substring(cut + 1);
            using (var parent = SettingsStore.Machine.OpenSubKey(parentPath, true))
            {
                if (parent == null) return;
                using (var k = parent.OpenSubKey(name))
                {
                    if (k == null) return;
                    if (k.ValueCount > 0 || k.SubKeyCount > 0) throw new InvalidOperationException(@"HKLM\" + path + " is not empty, so it is not removed");
                }
                parent.DeleteSubKey(name, false);
            }
        }

        static bool ReadsBack(RegWrite w)
        {
            if (w.DeleteKey) using (var gone = SettingsStore.Machine.OpenSubKey(w.Path)) return gone == null;
            using (var k = SettingsStore.Machine.OpenSubKey(w.Path))
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
                RouteRecordBefore(writes);
                foreach (var w in writes) { Write(w); Log("wrote: " + w); }
                bool ok = true;
                foreach (var w in writes)
                {
                    bool back = ReadsBack(w);
                    Log((back ? "read back OK: " : "read back MISMATCH: ") + w);
                    ok &= back;
                }
                if (ok) { RouteRecordAfter(writes); return true; }
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
