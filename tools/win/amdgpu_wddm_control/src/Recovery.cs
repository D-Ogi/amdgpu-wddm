// The driver's start and recovery states and the five safe actions on them, as pure functions over one snapshot:
// what each state means in plain English, the one action recommended for it, and for every action either a refusal
// or the exact registry writes it makes. The window, the dry run (--action X --dry-run) and the elevated helper
// (--action X) all plan through this file, so what a tester confirms, what a dry run prints and what the helper
// writes are the same list. RecoveryActions.cs reads the snapshot and executes a plan; nothing here touches the
// registry, the driver or a process.
//
// Sources of the rules (KMD 0.7.197, driver/kmd):
//   interop.c / interop_policy.c   EnableGpuPresentBlit, EnableCddDwmInterop (absent = 1), InteropClosedReason,
//                                  InteropLastState (effective | requested << 8), read at the next driver start
//   guard.c                        UnconfirmedStarts: at BC250_MAX_UNCONFIRMED_STARTS (2) the driver refuses to start
//   dpm.c                          DpmMode, DpmMaxMHz, DpmPending/DpmConfirmed, read at the next driver start
//   start_health.c                 the confirmation milestone; the release's start-confirm-core.ps1
//                                  (Test-StartConfirmEligible) is the rule this app applies before it confirms
// and the release's desktop router (HKLM\SOFTWARE\amdgpu-wddm\DesktopRouter): with RequireKmdSwitches 1 DWM loads
// the GPU desktop UMD only when DwmForceCpu is 0 and InteropLastState has both effective bits.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text;

namespace AmdgpuWddmControl
{
    // Everything the rules look at, read once. Serializable (JavaScriptSerializer) for --snapshot and the dry run.
    public sealed class RecoverySnapshot
    {
        public bool DriverInstalled { get; set; }                   // Services\bc250kmd\Parameters exists
        public bool RouterInstalled { get; set; }                   // SOFTWARE\amdgpu-wddm\DesktopRouter exists
        public Dictionary<string, long> Parameters { get; set; }    // REG_DWORD values of Parameters (absent: not listed)
        public long? DwmForceCpu { get; set; }
        public long? RequireKmdSwitches { get; set; }
        public string DriverError { get; set; }                     // null when the escapes answered
        public bool ReadFailed { get; set; }                        // the probe itself failed: nothing is known (497.4)
        public InteropState Interop { get; set; }
        public StartHealthState Health { get; set; }
        public DpmState Dpm { get; set; }
        public string DwmRoute { get; set; }                        // gpu, cpu or unknown
        public string DwmRouteDetail { get; set; }
        public bool GpuDesktopModules { get; set; }                 // the GPU route's UMDs are installed
        public bool TaskFound { get; set; }
        public long? TaskResult { get; set; }
        public string TaskLastRun { get; set; }
        public string ConfirmLogLast { get; set; }
        public Dictionary<string, long> DefaultParameters { get; set; }     // manifest.json "defaults"; null: none
        public Dictionary<string, long> DefaultRouter { get; set; }
        public string DefaultsError { get; set; }
        // The desktop compositor of the active interactive session (BD-060): what is running now, and every DWM the
        // observers saw in this session of this boot, this reading included.
        public DwmReading DwmNow { get; set; }
        public DwmObservations DwmHistory { get; set; }
        // The newest DwmForceCpu write that read back (route-written.json, written by the helper after the read-back):
        // its time and the value written (null: removed). A backup alone is an attempt, not a write.
        public string RouteWrittenUtc { get; set; }
        public long? RouteWrittenValue { get; set; }
        // The CU snapshot of this start (null: READ failed) and the CU values that exist with another type than
        // REG_DWORD (Parameters lists DWORDs only), so that such a value is unreadable, never absent.
        public CuModeState Cu { get; set; }
        public List<string> CuBadValues { get; set; }
        // A setter run of this boot could not flush or verify its writes (cu-unflushed.json): no next-start prediction.
        public bool CuNotDurable { get; set; }
        // Per-game switches: image -> Experiment as stored ("" for a key without the value), and the release's
        // recommended ones (manifest.json defaults.d3d12_applications; null: none).
        public Dictionary<string, string> GameProfiles { get; set; }
        public Dictionary<string, string> DefaultApplications { get; set; }

        public RecoverySnapshot() { Parameters = new Dictionary<string, long>(); DwmRoute = "unknown"; }

        public CuStored StoredCu()
        {
            if (CuBadValues != null && CuBadValues.Count > 0) return new CuStored { Unreadable = true };
            return new CuStored { Mode = P("CuMode"), Disable = P("CuDisableWgp"), Confirmed = P("CuModeConfirmed"), Pending = P("CuModePending"), NotDurable = CuNotDurable };
        }

        public uint? P(string name)
        {
            long v;
            return Parameters != null && Parameters.TryGetValue(name, out v) ? (uint?)(uint)v : null;
        }

        public bool DriverRunning { get { return DriverError == null && (Interop != null || Health != null || Dpm != null); } }
    }

    public sealed class RegWrite
    {
        public string Path { get; set; }
        public string Name { get; set; }
        public bool Delete { get; set; }
        public string Kind { get; set; }        // DWord (all the app's own writes), QWord or String (only an undo)
        public long Number { get; set; }
        public string Text { get; set; }

        public static RegWrite Dword(string path, string name, uint value) { return new RegWrite { Path = path, Name = name, Kind = "DWord", Number = value }; }
        public static RegWrite Remove(string path, string name) { return new RegWrite { Path = path, Name = name, Delete = true }; }

        public override string ToString()
        {
            string where = @"HKLM\" + Path + " " + Name;
            if (Delete) return "delete " + where;
            return "set " + where + " = " + (Kind == "String" ? "\"" + Text + "\"" : Number.ToString(CultureInfo.InvariantCulture)) + " (" + Kind + ")";
        }
    }

    public sealed class ActionPlan
    {
        public string Action, Refusal, Title, Change, Effect;
        public readonly List<RegWrite> Writes = new List<RegWrite>();
        public readonly List<string> Notes = new List<string>();
        public bool ConfirmStart, OfferRestart, RestartCompositor, Undoable = true;
        public string UndoOf;                  // undo: the backup file it restores
        // The CU setter's steps (section 7 S1-S6), run after Writes, never restored; CuConfirm: the CU CONFIRM escape.
        public CuSetterPlan Cu;
        public bool CuConfirm;
        // Per-game switches: image -> new Experiment value, "" = remove the game's key. Backed up and undoable per game.
        public readonly SortedDictionary<string, string> GameWrites = new SortedDictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        // What the dialog shows (in the window's language): one line per change, then the notes.
        public readonly List<string> Preview = new List<string>();

        public bool Refused { get { return Refusal != null; } }

        public string Text()
        {
            var w = new StringBuilder();
            w.AppendLine("action: " + Action);
            if (Refused) { w.AppendLine("refused: " + Refusal); return w.ToString(); }
            w.AppendLine("change: " + Change);
            w.AppendLine("takes effect: " + Effect);
            foreach (var x in Writes) w.AppendLine("  " + x);
            foreach (var g in GameWrites)
                w.AppendLine("  " + (g.Value.Length == 0 ? "delete HKLM\\" + Profiles.RegistryPath + "\\" + g.Key
                    : "set HKLM\\" + Profiles.RegistryPath + "\\" + g.Key + " " + Profiles.ValueName + " = \"" + g.Value + "\" (String)"));
            if (Cu != null)
            {
                w.AppendLine("  CU setter, target " + Cu.Target + ", stored before: " + Cu.Before + "; backup for diagnosis only, never restored");
                foreach (var s in Cu.Steps) w.AppendLine("  " + s);
            }
            if (CuConfirm) w.AppendLine("  confirm the 40-core start (CU CONFIRM with the Generation of a fresh READ), then READ again");
            if (ConfirmStart) w.AppendLine("  confirm this driver start (start-health CONFIRM with this start's generation and epoch)");
            if (RestartCompositor) w.AppendLine("  stop DWM in the active session; Windows starts a new one (operator escape)");
            foreach (var n in Notes) w.AppendLine("note: " + n);
            w.AppendLine("undo: " + (Undoable ? "yes (Undo last action)" : "no"));
            return w.ToString();
        }
    }

    public sealed class StateLine
    {
        public string Topic, Text, Severity, Action, ActionLabel;   // Severity ok | info | warn; Action null: none
    }

    // One backup file, %ProgramData%\amdgpu-wddm\control\backup-<utc>.json: the values an action found before its
    // writes. Undo restores them.
    public sealed class BackupValue
    {
        public string Path { get; set; }
        public string Name { get; set; }
        public bool Existed { get; set; }
        public string Kind { get; set; }
        public long Number { get; set; }
        public string Text { get; set; }
    }

    public sealed class BackupRecord
    {
        public int Schema { get; set; }
        public string Action { get; set; }
        public string[] Args { get; set; }
        public string Utc { get; set; }
        public string RunId { get; set; }
        public bool Undoable { get; set; }
        public bool RestartsDwm { get; set; }    // written by 0.3 and earlier, which restarted DWM; not used now
        public string Undoes { get; set; }      // the backup file an undo restored
        public List<BackupValue> Values { get; set; }
        // Values saved for diagnosis only (the CU setter's, section 7 S6): never restored, never offered by undo.
        public List<BackupValue> Diagnosis { get; set; }
        public string File { get; set; }        // set when read, not stored

        public BackupRecord() { Schema = 1; Values = new List<BackupValue>(); Diagnosis = new List<BackupValue>(); }
    }

    // One DWM process: its id and creation time (a reused id has another creation time), and who saw it first.
    public sealed class DwmInstance
    {
        public int Pid { get; set; }
        public string CreatedUtc { get; set; }      // yyyy-MM-ddTHH:mm:ss.fffZ, so that ordinal order is time order
        public string FirstSeenUtc { get; set; }
        public string Observer { get; set; }        // window, status, report, helper, escape
    }

    // What an observer reads now: the boot (BootId), the active interactive session, the session's start (the
    // creation time of its winlogon) and the session's DWM. A null member was not readable.
    public sealed class DwmReading
    {
        public long? BootId { get; set; }
        public int? Session { get; set; }
        public string SessionStartUtc { get; set; }
        public int? Pid { get; set; }
        public string CreatedUtc { get; set; }
        // The epoch of the installer's record: the boot time and the session's logon time (WTSSessionInfo).
        public string BootUtc { get; set; }
        public string LogonUtc { get; set; }
    }

    // One record of the installer's %ProgramData%\amdgpu-wddm\dwm-baseline.json (tools\release\installer\dwm-session.ps1):
    // the DWM the start-confirm task saw at a logon, keyed by boot time, session and logon time. This app only reads it.
    public sealed class DwmBaselineRecord
    {
        public string BootUtc, LogonUtc, CreatedUtc, RecordedUtc, RecordedBy;
        public int Session, Pid;
    }

    // The DWM instances observers saw in one session of one boot (BD-060). Only a replacement an observer saw counts
    // as a restart; before the first observation the history is unknown. A new boot or session starts a new record.
    public sealed class DwmObservations
    {
        public int Schema { get; set; }
        public long BootId { get; set; }
        public int Session { get; set; }
        public string SessionStartUtc { get; set; }
        public List<DwmInstance> Instances { get; set; }

        public DwmObservations() { Schema = 1; Instances = new List<DwmInstance>(); }
    }

    // dwm-observations.json: one record per session epoch, the newest 8 (another user's session must not erase this
    // one's history).
    public sealed class DwmObservationsFile
    {
        public int Schema { get; set; }
        public List<DwmObservations> Records { get; set; }

        public DwmObservationsFile() { Schema = 2; Records = new List<DwmObservations>(); }
    }

    // route-written.json: the newest DwmForceCpu write that read back. The helper removes it before such a write and
    // writes it after the read-back, so a failed or rolled-back attempt leaves none.
    public sealed class RouteRecord
    {
        public int Schema { get; set; }
        public string Utc { get; set; }
        public long? Value { get; set; }      // null: the value was removed
        public string Action { get; set; }
        public string RunId { get; set; }
    }

    // cu-unflushed.json: the boot in which a CU setter run could not flush or verify its writes (plan 497 decision 1).
    public sealed class CuUnflushedRecord
    {
        public int Schema { get; set; }
        public long BootId { get; set; }
        public string Utc { get; set; }
        public string RunId { get; set; }
    }

    // What one write of dwm-observations.json does: the merged history of this session, the file to write (only valid
    // epochs and instances, this one last) and whether to write it.
    public sealed class CommitPlan
    {
        public DwmObservations Merged;
        public DwmObservationsFile File;
        public bool Write;
    }

    // Lists one process's modules into the buffer and reports the bytes the full list needs (EnumProcessModulesEx).
    public delegate bool EnumModules(IntPtr[] buffer, out int neededBytes);

    public static class Recovery
    {
        public const string ParametersPath = DpmSettings.RegistryPath;
        public const string RouterPath = KmdReply.DesktopRouterPath;
        public const uint MaxUnconfirmedStarts = 2;         // BC250_MAX_UNCONFIRMED_STARTS, driver/kmd/bc250kmd.h
        // start-confirm-core.ps1: $StartConfirmMinReadyMs, $StartConfirmFreshMs, Test-StartConfirmEligible. The unit
        // tests compare these with that file when the build is given it.
        public const ulong ConfirmMinReadyMs = 60000, ConfirmFreshMs = 5000;
        public const uint ConfirmRequiredFlags = 7;         // BC250_START_HEALTH_REQUIRED (FULL | READY | VISIBLE)
        // Why no action restarts the desktop compositor (DWM) any more.
        public const string Bd060Note = "The app does not restart the desktop compositor (DWM): on Windows 11 a DWM restart leaves some apps, for example the Explorer command bar and Task Manager, ignoring mouse clicks until Windows restarts (BD-060).";

        // The only values an action or an undo may write. Never: the temperature limits, firmware paths, the other
        // KMD service values, BIOS or firmware settings, test signing.
        public static readonly string[] ParameterNames = { "EnableGpuPresentBlit", "EnableCddDwmInterop", "InteropClosedReason", "DpmMode", "DpmMaxMHz" };
        public static readonly string[] RouterNames = { "DwmForceCpu" };
        // Defaults the reset takes from manifest.json (the rest of the release's table is not the app's to touch).
        public static readonly string[] DefaultParameterNames = { "EnableGpuPresentBlit", "EnableCddDwmInterop", "DpmMode", "DpmMaxMHz" };

        // Not offered in the window, refused without --accept-bd060: for a desktop that does not respond.
        public const string OperatorEscape = "restart-compositor";

        public static readonly string[] Actions = { "reopen-gpu-path", "desktop-gpu", "desktop-cpu", "confirm-start", "enable-dpm", "set-clocks", "reset-defaults", "undo",
            "cu-mode", "cu-confirm", "game-profile", "game-undo", "game-redo" };

        public static bool Allowed(string path, string name)
        {
            if (string.Equals(path, ParametersPath, StringComparison.OrdinalIgnoreCase)) return ParameterNames.Contains(name);
            if (string.Equals(path, RouterPath, StringComparison.OrdinalIgnoreCase)) return RouterNames.Contains(name);
            return false;
        }

        // The CU setter's set only (G-PLAN): CuMode written as 40 or removed, CuDisableWgp and CuModeConfirmed removed.
        public static bool CuAllowed(RegWrite w)
        {
            if (!string.Equals(w.Path, ParametersPath, StringComparison.OrdinalIgnoreCase)) return false;
            if (w.Name == "CuMode") return w.Delete || (w.Kind == "DWord" && w.Number == CuMode.Full);
            return (w.Name == "CuDisableWgp" || w.Name == "CuModeConfirmed") && w.Delete;
        }

        // A game's Experiment value (WU-015, WU-020a): HKLM\SOFTWARE\amdgpu-wddm\D3D12\Applications\<image> only.
        public static bool GameAllowed(string image, string value)
        {
            return Profiles.IsValidImage(image) && (value.Length == 0 || Profiles.IsValidValue(value));
        }

        public static string GamePath(string image) { return Profiles.RegistryPath + "\\" + image; }

        // ---- the decision rules ----------------------------------------------------------------------------------

        // Test-StartConfirmEligible of start-confirm-core.ps1, field for field.
        // KMD 0.7.198 (BC250_KMD_VERSION 0x000700C6) fixed BD-059: a normal restart keeps the GPU desktop path open,
        // so a close mark from it means the last session really ended unclean. null: no driver reply gives the version.
        public const uint Bd059FixedVersion = 0x000700C6;

        public static bool? Bd059Fixed(RecoverySnapshot s)
        {
            uint v = s.Interop != null && s.Interop.Version != 0 ? s.Interop.Version
                : s.Health != null && s.Health.Version != 0 ? s.Health.Version
                : s.Dpm != null ? s.Dpm.Version : 0;
            if (v == 0) return null;
            return v >= Bd059FixedVersion;
        }

        public static bool ConfirmEligible(StartHealthState h)
        {
            return h != null && (h.Flags & ConfirmRequiredFlags) == ConfirmRequiredFlags && h.Completed > 0 &&
                h.ReadyAgeMs >= ConfirmMinReadyMs && h.LastCompletionAgeMs <= ConfirmFreshMs;
        }

        public static bool Confirmed(StartHealthState h)
        {
            return h != null && (h.Flags & (StartHealthState.Full | StartHealthState.Confirmed)) == (StartHealthState.Full | StartHealthState.Confirmed);
        }

        // Why a reading is not eligible; null when it is. requireFresh false: the window's check, where the last
        // presentation may be a few seconds old; the helper applies the full rule (and waits briefly for freshness).
        public static string ConfirmBlocker(StartHealthState h, bool requireFresh)
        {
            if (h == null) return "the driver gave no start reading";
            if ((h.Flags & StartHealthState.Full) == 0) return "this is not a full driver start (flags " + h.Flags + ")";
            if ((h.Flags & StartHealthState.Ready) == 0) return "the driver start has not finished";
            if ((h.Flags & StartHealthState.Visible) == 0) return "no picture has reached the screen through the driver yet";
            if (h.Completed == 0) return "the GPU has not completed any work yet";
            if (h.ReadyAgeMs < ConfirmMinReadyMs)
                return "the driver must run for 60 seconds first (" + ((ConfirmMinReadyMs - h.ReadyAgeMs + 999) / 1000) + " s left)";
            if (requireFresh && h.LastCompletionAgeMs > ConfirmFreshMs)
                return "the GPU has not completed work in the last 5 seconds (last " + (h.LastCompletionAgeMs / 1000) + " s ago); move the mouse or open a window and try again";
            return null;
        }

        // Both interop switches open for the start that runs now: the escape when the driver answers (a decided
        // start), and the value the router itself reads (InteropLastState). Either one closed is closed.
        public static bool SwitchesOpenNow(RecoverySnapshot s)
        {
            uint both = InteropState.SwitchBlit | InteropState.SwitchCdd;
            var last = s.P("InteropLastState");
            if (last == null || (last.Value & both) != both) return false;
            if (s.Interop != null && ((s.Interop.Flags & InteropState.FlagValid) == 0 || (s.Interop.Effective & both) != both)) return false;
            return true;
        }

        // The switches as the next start will read them: absent = 1, 1 = on.
        public static bool SwitchesRequested(RecoverySnapshot s)
        {
            var blit = s.P("EnableGpuPresentBlit");
            var cdd = s.P("EnableCddDwmInterop");
            return (blit == null || blit == 1) && (cdd == null || cdd == 1);
        }

        public static string InteropReasonText(uint reason)
        {
            switch (reason)
            {
                case 0: return "open as requested";
                case 1: return "switched off in the settings";
                case 2: return "a switch holds an invalid value";
                case 4: return "closed by the driver after a shutdown it took as unclean";
                case 5: return "a setting could not be read";
                case 7: return "no full driver start";
                default: return "reason " + reason;
            }
        }

        public static string InteropEndText(uint end)
        {
            return end == 1 ? "the device stopped with the path in use" : end == 2 ? "the last desktop device on the path closed" : "none recorded";
        }

        // enum _BC250_STAGE, driver/kmd/bc250kmd.h.
        public static string StageText(uint stage)
        {
            switch (stage)
            {
                case 0: return "none";
                case 10: return "driver loaded";
                case 20: return "device added";
                case 30: return "start begun";
                case 31: return "start: boot-loop guard passed";
                case 32: return "start: device information";
                case 33: return "start: display taken over from the firmware";
                case 34: return "start: frame buffer mapped";
                case 35: return "start: registers mapped";
                case 39: return "start finished";
                case 50: return "first display mode set";
                case 60: return "first frame presented";
                case 61: return "first frame finished";
                case 70: return "stop begun";
                case 79: return "stopped";
                case 90: return "refused to start (boot-loop guard)";
                case 91: return "start failed";
                default: return "stage " + stage;
            }
        }

        // LastTaskResult of the release's "amdgpu-wddm start confirm" task (start-confirm-core.ps1 exit codes).
        public static string TaskResultText(long? result)
        {
            if (result == null) return "unknown";
            switch (result.Value)
            {
                case 0: return "confirmed the start";
                case 1: return "gave up after 2 minutes without a healthy start reading";
                case 2: return "left the start to a lab kit";
                case 3: return "the driver restarted while it waited";
                case 4: return "stopped by the operator";
                case 5: return "found no full driver start";
                case 0x41300: return "ready, has not run yet";
                case 0x41301: return "running now";
                case 0x41303: return "has not run yet";
                default: return "result 0x" + result.Value.ToString("X", CultureInfo.InvariantCulture);
            }
        }

        // The route of a running DWM, from the modules it loaded (the release's desktop directory).
        public static string RouteFromModules(IEnumerable<string> modules)
        {
            bool gpu = false, cpu = false;
            foreach (var m in modules ?? new string[0])
            {
                var file = System.IO.Path.GetFileName(m ?? "").ToLowerInvariant();
                var dir = System.IO.Path.GetFileName(System.IO.Path.GetDirectoryName(m ?? "") ?? "").ToLowerInvariant();
                if (file == "bc250d3d_zink.dll" || file == "amdgpu_wddm_radv.dll" && dir == "desktop") gpu = true;
                if (file == "bc250d3d.dll") cpu = true;
            }
            return gpu ? "gpu" : cpu ? "cpu" : "unknown";
        }

        // ---- the desktop compositor (BD-060) ----------------------------------------------------------------------

        // A reading counts only when every identity field is valid: boot, a session above 0, a session start and a
        // creation time that parse, a process id above 0.
        public static bool Complete(DwmReading r)
        {
            return r != null && r.BootId != null && r.Session != null && r.Session.Value > 0 && Utc(r.SessionStartUtc) != null &&
                r.Pid != null && r.Pid.Value > 0 && Utc(r.CreatedUtc) != null;
        }

        // A stored instance counts only with a process id above 0 and creation and first-seen times that parse: a
        // damaged entry can never make a second instance (reviewer 911).
        public static bool Valid(DwmInstance i)
        {
            return i != null && i.Pid > 0 && Utc(i.CreatedUtc) != null && Utc(i.FirstSeenUtc) != null;
        }

        static bool SameSession(DwmObservations o, DwmReading r)
        {
            return o != null && Complete(r) && o.BootId == r.BootId.Value && o.Session == r.Session.Value && o.SessionStartUtc == r.SessionStartUtc;
        }

        // The stored records (the user's and the machine's) merged for this reading's session, with this reading
        // added. Records of another boot or session are dropped: a new session starts a new baseline. Null without a
        // complete reading.
        public static DwmObservations Observe(IEnumerable<DwmObservations> records, DwmReading now, string observer, string nowUtc)
        {
            if (!Complete(now)) return null;
            var o = new DwmObservations { BootId = now.BootId.Value, Session = now.Session.Value, SessionStartUtc = now.SessionStartUtc };
            foreach (var r in (records ?? Enumerable.Empty<DwmObservations>()).Where(r => SameSession(r, now)))
                foreach (var i in r.Instances ?? new List<DwmInstance>())
                    if (Valid(i)) AddInstance(o, i);
            AddInstance(o, new DwmInstance { Pid = now.Pid.Value, CreatedUtc = now.CreatedUtc, FirstSeenUtc = nowUtc, Observer = observer });
            o.Instances = o.Instances.OrderBy(i => i.CreatedUtc, StringComparer.Ordinal).ToList();
            return o;
        }

        // A stored epoch with only its valid instances; null when its identity is damaged or no instance is valid.
        public static DwmObservations Sanitize(DwmObservations r)
        {
            if (r == null || r.Session <= 0 || Utc(r.SessionStartUtc) == null || r.Instances == null) return null;
            var valid = r.Instances.Where(Valid).ToList();
            if (valid.Count == 0) return null;
            var o = new DwmObservations { BootId = r.BootId, Session = r.Session, SessionStartUtc = r.SessionStartUtc };
            o.Instances.AddRange(valid.Select(i => new DwmInstance { Pid = i.Pid, CreatedUtc = i.CreatedUtc, FirstSeenUtc = i.FirstSeenUtc, Observer = i.Observer }));
            return o;
        }

        static bool SameEpoch(DwmObservations a, DwmObservations b)
        {
            return a.BootId == b.BootId && a.Session == b.Session && a.SessionStartUtc == b.SessionStartUtc;
        }

        // One write of dwm-observations.json, from what the file holds now (as read, damaged entries included). Every
        // epoch and instance is sanitized before anything is ordered or kept, so a damaged entry is dropped instead of
        // stopping every later write (reviewer 914); a file that held one is rewritten. Other epochs: the 7 newest.
        public static CommitPlan PlanCommit(IEnumerable<DwmObservations> stored, DwmObservations history, DwmReading now, string observer, string nowUtc)
        {
            var raw = (stored ?? Enumerable.Empty<DwmObservations>()).ToList();
            var clean = raw.Select(Sanitize).Where(r => r != null).ToList();
            bool damaged = clean.Count != raw.Count || raw.Any(r => r.Instances.Count != r.Instances.Count(Valid));     // raw.Count == clean.Count: none is null
            var merged = Observe(clean.Concat(new[] { history }), now, observer, nowUtc);
            if (merged == null) return null;
            // At most 64 instances: a DWM that restarts in a loop is a defect, and the first and the last tell it.
            if (merged.Instances.Count > 64) merged.Instances.RemoveRange(1, merged.Instances.Count - 64);
            var plan = new CommitPlan { Merged = merged, File = new DwmObservationsFile() };
            plan.File.Records.AddRange(clean.Where(r => !SameEpoch(r, merged)).OrderBy(r => r.Instances.Max(i => i.FirstSeenUtc), StringComparer.Ordinal)
                .Reverse().Take(7).Reverse());
            plan.File.Records.Add(merged);
            var mine = clean.FirstOrDefault(r => SameEpoch(r, merged));
            var json = new System.Web.Script.Serialization.JavaScriptSerializer();
            plan.Write = damaged || mine == null || json.Serialize(mine) != json.Serialize(merged);
            return plan;
        }

        // The complete module list of a process, or null: the buffer grows to the size the system reports (at most 4
        // tries, at most 16384 modules), and an enumeration that fails or a name that cannot be read makes the list
        // incomplete. A partial list never decides a route (reviewer 914).
        public static List<string> CompleteModuleList(EnumModules enumerate, Func<IntPtr, string> name, int pointerSize)
        {
            int count = 1024;
            for (int attempt = 0; attempt < 4; attempt++)
            {
                var buffer = new IntPtr[count];
                int needed;
                if (!enumerate(buffer, out needed) || needed <= 0 || needed % pointerSize != 0) return null;
                int n = needed / pointerSize;
                if (n > 16384) return null;
                if (n > buffer.Length) { count = n + 64; continue; }      // more modules than the buffer: again, with the reported size
                var list = new List<string>(n);
                for (int i = 0; i < n; i++)
                {
                    var file = name(buffer[i]);
                    if (string.IsNullOrEmpty(file)) return null;
                    list.Add(file);
                }
                return list;
            }
            return null;
        }

        // The same process: the same id and creation times at most 1 s apart (the installer reads the creation time
        // through WMI, at another precision).
        public static bool SameDwm(int pidA, string createdA, int pidB, string createdB)
        {
            var a = Utc(createdA); var b = Utc(createdB);
            return pidA == pidB && a != null && b != null && Math.Abs((a.Value - b.Value).TotalSeconds) <= 1;
        }

        static void AddInstance(DwmObservations o, DwmInstance i)
        {
            var known = o.Instances.FirstOrDefault(x => SameDwm(x.Pid, x.CreatedUtc, i.Pid, i.CreatedUtc));
            if (known == null)
                o.Instances.Add(new DwmInstance { Pid = i.Pid, CreatedUtc = i.CreatedUtc, FirstSeenUtc = i.FirstSeenUtc, Observer = i.Observer });
            else if (string.CompareOrdinal(i.FirstSeenUtc ?? "", known.FirstSeenUtc ?? "") < 0) { known.FirstSeenUtc = i.FirstSeenUtc; known.Observer = i.Observer; }
        }

        // The installer's records, parsed. A damaged file gives none; a damaged record (a missing or invalid process id,
        // session or time) is skipped, so it reads as no record (unknown history), never as a replacement.
        public static List<DwmBaselineRecord> ParseBaseline(string json)
        {
            var list = new List<DwmBaselineRecord>();
            object root;
            try { root = new System.Web.Script.Serialization.JavaScriptSerializer().DeserializeObject(json ?? ""); }
            catch (Exception) { return list; }
            var d = root as IDictionary<string, object>;
            object records;
            if (d == null || !d.ContainsKey("schema") || Convert.ToString(d["schema"], CultureInfo.InvariantCulture) != "1" || !d.TryGetValue("records", out records)) return list;
            var items = records is object[] ? (object[])records : records is System.Collections.ArrayList ? ((System.Collections.ArrayList)records).ToArray() : new[] { records };
            foreach (var item in items)
                try
                {
                    var r = item as IDictionary<string, object>;
                    if (r == null) continue;
                    Func<string, string> text = k => r.ContainsKey(k) && r[k] is string ? (string)r[k] : null;
                    // Whole JSON numbers only: a missing, null, text, fractional or out-of-range id is a damaged record.
                    Func<string, int> number = k => !r.ContainsKey(k) ? 0 : r[k] is int ? (int)r[k] : r[k] is long && (long)r[k] > 0 && (long)r[k] <= int.MaxValue ? (int)(long)r[k] : 0;
                    var x = new DwmBaselineRecord
                    {
                        BootUtc = text("boot_utc"), LogonUtc = text("logon_utc"), CreatedUtc = text("dwm_created_utc"), RecordedUtc = text("recorded_utc"),
                        RecordedBy = text("recorded_by"), Session = number("session"), Pid = number("dwm_pid"),
                    };
                    if (Utc(x.BootUtc) == null || Utc(x.LogonUtc) == null || Utc(x.CreatedUtc) == null || Utc(x.RecordedUtc) == null || x.Pid <= 0 || x.Session <= 0) continue;
                    list.Add(x);
                }
                catch (Exception) { }
            return list;
        }

        // The installer's record of this reading's epoch (the same session, boot and logon times within 2 s, the
        // installer's own rule) as an observation of this session; null when none matches. Another epoch is never a
        // replacement.
        public static DwmObservations FromBaseline(IEnumerable<DwmBaselineRecord> records, DwmReading now)
        {
            var boot = Utc(now != null ? now.BootUtc : null);
            var logon = Utc(now != null ? now.LogonUtc : null);
            if (!Complete(now) || boot == null || logon == null || records == null) return null;
            var r = records.FirstOrDefault(x => x != null && x.Session == now.Session.Value &&
                Math.Abs((Utc(x.BootUtc).Value - boot.Value).TotalSeconds) <= 2 && Math.Abs((Utc(x.LogonUtc).Value - logon.Value).TotalSeconds) <= 2);
            if (r == null) return null;
            var o = new DwmObservations { BootId = now.BootId.Value, Session = now.Session.Value, SessionStartUtc = now.SessionStartUtc };
            o.Instances.Add(new DwmInstance { Pid = r.Pid, CreatedUtc = Stamp(Utc(r.CreatedUtc)), FirstSeenUtc = Stamp(Utc(r.RecordedUtc)), Observer = "start-confirm" });
            return o;
        }

        public static string Stamp(DateTime? utc)
        {
            return utc == null ? null : utc.Value.ToUniversalTime().ToString("yyyy-MM-dd'T'HH:mm:ss.fff'Z'", CultureInfo.InvariantCulture);
        }

        // observed: an observer saw more than one DWM in this session. unknown-history: one DWM seen so far; a
        // replacement before its first observation would not be seen. unknown: no complete reading.
        public static string DwmVerdict(RecoverySnapshot s)
        {
            var h = s.DwmHistory;
            if (h == null || !SameSession(h, s.DwmNow) || h.Instances == null || h.Instances.Count == 0 || !h.Instances.All(Valid)) return "unknown";
            return h.Instances.Count > 1 ? "observed" : "unknown-history";
        }

        public static DateTime? Utc(string text)
        {
            DateTime t;
            return text != null && DateTime.TryParse(text, CultureInfo.InvariantCulture, DateTimeStyles.AdjustToUniversal | DateTimeStyles.AssumeUniversal, out t) ? (DateTime?)t : null;
        }

        static string Show(string utc)
        {
            var t = Utc(utc);
            return t == null ? "an unknown time" : t.Value.ToString("yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture) + " UTC";
        }

        static string Duration(double seconds)
        {
            Func<double, string> n = v => Math.Round(v).ToString(CultureInfo.InvariantCulture);
            return seconds < 120 ? n(seconds) + " s" : seconds < 7200 ? n(seconds / 60) + " min"
                : seconds < 172800 ? n(seconds / 3600) + " h" : n(seconds / 86400) + " days";
        }

        static string ObserverText(string observer)
        {
            switch (observer)
            {
                case "window": return "this app's window";
                case "status": return "--status";
                case "report": return "a bug report";
                case "helper": return "a Recovery action";
                case "escape": return "the operator escape";
                case "start-confirm": return "the installer's start-confirm task at logon";
                default: return observer ?? "an observer";
            }
        }

        // The compositor state of the Recovery page, --status and the bug report. It helps to attribute a failure: a
        // DWM crash can still be a driver defect, so it never says the driver is not involved.
        public static string CompositorText(RecoverySnapshot s)
        {
            switch (DwmVerdict(s))
            {
                case "observed":
                {
                    var list = s.DwmHistory.Instances;
                    var now = list[list.Count - 1];
                    int n = list.Count - 1;
                    return "The desktop compositor (DWM) was replaced in this session: " + n + (n == 1 ? " replacement" : " replacements") +
                        " seen; the DWM now running (process " + now.Pid + ") started " + Show(now.CreatedUtc) + ". After a DWM restart some Windows 11 apps, " +
                        "for example the Explorer command bar and Task Manager, can ignore mouse clicks until Windows restarts (BD-060): restart Windows. " +
                        "If nobody restarted DWM on purpose, create a bug report: a DWM crash can be a driver defect.";
                }
                case "unknown-history":
                {
                    var only = s.DwmHistory.Instances[0];
                    var seen = Utc(only.FirstSeenUtc);
                    var start = Utc(s.DwmHistory.SessionStartUtc);
                    string after = seen != null && start != null && seen.Value >= start.Value ? ", " + Duration((seen.Value - start.Value).TotalSeconds) + " after the session began" : "";
                    return "No replacement of the desktop compositor (DWM) seen since " + Show(only.FirstSeenUtc) + " (first seen by " + ObserverText(only.Observer) + after +
                        "). A replacement before that would not be seen.";
                }
                default:
                    return "The desktop compositor (DWM) of the active session cannot be read.";
            }
        }

        // One line for scripts (the installer's verify): observed, unknown-history or unknown, then the readings.
        public static string CompositorStatusLine(RecoverySnapshot s)
        {
            var r = s.DwmNow ?? new DwmReading();
            var h = DwmVerdict(s) == "unknown" ? null : s.DwmHistory;
            Func<object, string> v = x => x == null ? "-" : Convert.ToString(x, CultureInfo.InvariantCulture);
            return "dwm-restart: " + DwmVerdict(s) + " (boot " + v(r.BootId) + ", session " + v(r.Session) + ", session start " + v(r.SessionStartUtc) +
                ", DWM process " + v(r.Pid) + " started " + v(r.CreatedUtc) + ", instances seen " + (h == null ? "-" : h.Instances.Count.ToString(CultureInfo.InvariantCulture)) +
                ", watched since " + (h == null ? "-" : h.Instances[0].FirstSeenUtc + " by " + h.Instances[0].Observer) + ")";
        }

        public static string CompositorOverview(RecoverySnapshot s)
        {
            switch (DwmVerdict(s))
            {
                case "observed": return "Replaced in this session: restart Windows (BD-060, see Recovery)";
                case "unknown-history": return "No replacement seen since " + Show(s.DwmHistory.Instances[0].FirstSeenUtc) + " (earlier: not known)";
                default: return "-";
            }
        }

        // ---- the states ------------------------------------------------------------------------------------------

        public static List<StateLine> Describe(RecoverySnapshot s)
        {
            var lines = new List<StateLine>();
            Func<string, string, string, string, string, StateLine> add = (topic, text, severity, action, label) =>
            {
                var l = new StateLine { Topic = topic, Text = text, Severity = severity, Action = action, ActionLabel = label };
                lines.Add(l);
                return l;
            };
            // The desktop compositor first: it matters with or without the driver (BD-060).
            bool replaced = DwmVerdict(s) == "observed";
            add("Desktop compositor", CompositorText(s), replaced ? "warn" : "info", replaced ? "restart" : null, replaced ? "Restart Windows" : null);
            if (!s.DriverInstalled)
            {
                add("Driver", "The bc250kmd driver is not installed on this PC. There is nothing to recover.", "info", null, null);
                return lines;
            }

            // Driver start and the boot-loop guard.
            uint starts = s.P("UnconfirmedStarts") ?? 0;
            uint stage = s.P("LastStage") ?? 0;
            string guard = "Unconfirmed starts: " + starts + " of " + MaxUnconfirmedStarts + ". Last driver step: " + StageText(stage) + ".";
            if (!s.DriverRunning)
            {
                if (starts >= MaxUnconfirmedStarts || stage == 90)
                    add("Driver start", "The driver refused to start: " + starts + " starts in a row were not confirmed, so Windows uses Microsoft Basic Display. " + guard +
                        " Create a bug report, then run the release installer again: it resets the counter.", "warn", null, null);
                else
                    add("Driver start", "The driver is not running (" + (s.DriverError ?? "no answer") + "). " + guard, "warn", null, null);
            }
            else if (Confirmed(s.Health))
                add("Driver start", "This start is confirmed. " + guard, "ok", null, null);
            else
            {
                var blocker = ConfirmBlocker(s.Health, false);
                if (blocker == null)
                    add("Driver start", "This start is healthy but not confirmed yet. After " + MaxUnconfirmedStarts +
                        " unconfirmed starts the driver refuses to start and Windows falls back to Microsoft Basic Display. " + guard, "warn", "confirm-start", "Confirm this start");
                else
                    add("Driver start", "This start is not confirmed yet: " + blocker + ". The logon task confirms it by itself. " + guard, "info", null, null);
            }

            // The GPU desktop path (interop switches).
            bool requested = SwitchesRequested(s), openNow = SwitchesOpenNow(s);
            var closed = s.P("InteropClosedReason");
            string session = "";
            if (s.Interop != null)
                session = " Desktop devices on the path: " + s.Interop.Users + (s.Interop.LastEnd != 0 ? ", last session end: " + InteropEndText(s.Interop.LastEnd) : "") + ".";
            else if (s.P("InteropLastEnd") != null)
                session = " Last session end: " + InteropEndText(s.P("InteropLastEnd").Value) + ".";
            if (openNow && requested && closed == null)
                add("GPU desktop path", "Open: both switches are on for this start." + session, "ok", null, null);
            else if (requested && closed == null)
                add("GPU desktop path", "Reopened in the settings; the driver opens it at the next restart of Windows." + session, "info", "restart", "Restart Windows");
            else if (closed != null)
            {
                var fixedNow = Bd059Fixed(s);
                add("GPU desktop path", (fixedNow == true ? "Closed by the driver: the last session ended without a clean shutdown (power loss, crash or reset; reason " + closed + ")."
                    : fixedNow == false ? "Closed by the driver: it took the last shutdown as unclean (reason " + closed + "). A normal restart can cause this (BD-059)."
                    : "Closed by the driver: the last session ended without a clean shutdown (reason " + closed + "): power loss, crash or reset, or with a driver before 0.7.198 also a normal restart (BD-059).") + session,
                    "warn", "reopen-gpu-path", "Reopen the GPU desktop path");
            }
            else
            {
                uint reason = s.Interop != null ? s.Interop.Reason : (s.P("InteropLastReason") ?? 1);
                add("GPU desktop path", "Closed: " + InteropReasonText(reason == 0 ? 1 : reason) + "." + session, "info", "reopen-gpu-path", "Reopen the GPU desktop path");
            }

            // Desktop composition: the selected route (DwmForceCpu, read by the router when DWM starts) and the active
            // route (the modules the running DWM loaded) are separate. A change is pending until the next restart; a
            // DWM that started after the change shows whether it took.
            string seen = s.DwmRoute == "unknown" ? "" : " Active now: the " + s.DwmRoute.ToUpperInvariant() + " route" + (string.IsNullOrEmpty(s.DwmRouteDetail) ? "" : " (" + s.DwmRouteDetail + ")") + ".";
            string selected = (s.DwmForceCpu ?? 0) != 0 ? "cpu" : "gpu";
            var dwmStart = Utc(s.DwmNow != null ? s.DwmNow.CreatedUtc : null);
            // Timing only from a write that read back the value selected now; anything else: no timing (unknown).
            var written = s.RouteWrittenValue == s.DwmForceCpu ? Utc(s.RouteWrittenUtc) : null;
            bool pending = dwmStart != null && written != null && written.Value > dwmStart.Value;      // written after this DWM started
            bool afterWrite = dwmStart != null && written != null && dwmStart.Value > written.Value;   // this DWM started after the write
            string sel = selected.ToUpperInvariant(), act = s.DwmRoute.ToUpperInvariant();
            if (!s.RouterInstalled)
                add("Desktop composition", "The release's desktop router is not installed." + seen, "info", null, null);
            else if (s.DwmRoute != "unknown" && s.DwmRoute != selected && (selected == "cpu" || openNow))
            {
                if (pending)
                    add("Desktop composition", "Selected for the next start: the " + sel + " route, pending until Windows restarts." + seen, "info", "restart", "Restart Windows");
                else if (afterWrite)
                    add("Desktop composition", "The " + sel + " route was selected at " + Show(s.RouteWrittenUtc) + ", but the DWM that started after it loaded the " + act +
                        " route." + seen + " Create a bug report.", "warn", null, null);
                else
                    add("Desktop composition", "Selected: the " + sel + " route; the running DWM loaded the " + act + " route. A route chosen in this session applies at the next restart of Windows." + seen,
                        "info", "restart", "Restart Windows");
            }
            else if (pending && s.DwmRoute == "unknown")
                add("Desktop composition", "Selected for the next start: the " + sel + " route, pending until Windows restarts. The active route cannot be read.", "info", "restart", "Restart Windows");
            else if ((s.DwmForceCpu ?? 0) != 0)
            {
                // The release's own choice comes from manifest.json, never from this app.
                long def;
                bool releaseCpu = s.DefaultRouter != null && s.DefaultRouter.TryGetValue("DwmForceCpu", out def) && def != 0;
                bool releaseGpu = s.DefaultRouter != null && s.DefaultRouter.TryGetValue("DwmForceCpu", out def) && def == 0;
                string verified = afterWrite && s.DwmRoute == "cpu" ? " The DWM that started after the change loaded it." : "";
                if (releaseCpu)
                    add("Desktop composition", "CPU route (GPU route disabled, BD-058). This is the release default." + seen + verified, "ok", null, null);
                else
                    add("Desktop composition", "CPU route (DwmForceCpu 1)." + (releaseGpu ? " The release default is the GPU route." : "") + seen + verified, "info",
                        releaseGpu && openNow ? "desktop-gpu" : null, releaseGpu && openNow ? "Desktop on the GPU route" : null);
            }
            else if (!openNow)
                add("Desktop composition", "GPU route selected, but the GPU desktop path is closed, so DWM stays on the CPU route." + seen, "warn",
                    requested && closed == null ? "restart" : "reopen-gpu-path", requested && closed == null ? "Restart Windows" : "Reopen the GPU desktop path");
            else if (DwmVerdict(s) == "observed")
                // The one sign of a failing GPU route this app can see: a DWM replaced in this session on that route.
                add("Desktop composition", (s.DwmRoute == "gpu" ? "GPU route: selected and active" : "GPU route selected; the active route cannot be read") +
                    ", and the desktop compositor (DWM) was replaced in this session. If nobody restarted it on purpose, the GPU route may be failing: " +
                    "put the desktop on the CPU route and restart Windows." + seen, "warn", "desktop-cpu", "Desktop on the CPU route");
            else
                // A healthy state recommends nothing; the CPU route stays an available action on the Actions page.
                add("Desktop composition", (s.DwmRoute == "gpu" ? "GPU route: selected and active" + (afterWrite ? " (the DWM that started after the change loaded it)" : "") + "."
                    : "GPU route selected; the active route cannot be read.") +
                    " If the desktop goes black or restarts, put it back on the CPU route (desktop-cpu) and restart Windows." + seen, "info", null, null);

            // Clock control.
            uint? mode = s.P("DpmMode"), max = s.P("DpmMaxMHz"), lastMode = s.P("DpmLastMode"), lastReason = s.P("DpmLastReason");
            uint runReason = s.Dpm != null ? s.Dpm.Reason : lastReason ?? 0;
            string stored = "Setting for the next start: " + DpmSettings.Describe(mode, max) + ".";
            bool fellBack = mode != 1 && (runReason == 3 || runReason == 4 || runReason == 8);
            if (fellBack)
                add("Clock control", "The driver went back to the fixed clock: " + KmdReply.ReasonText(runReason) + ". " + stored, "warn", "enable-dpm", "Re-enable automatic clocks");
            else if (mode == 1 && s.P("DpmPending") != null && s.P("DpmConfirmed") == null)
                add("Clock control", "Automatic clocks are on trial for this start; confirming the start keeps them. " + stored, "info",
                    s.DriverRunning && !Confirmed(s.Health) && ConfirmBlocker(s.Health, false) == null ? "confirm-start" : null,
                    s.DriverRunning && !Confirmed(s.Health) && ConfirmBlocker(s.Health, false) == null ? "Confirm this start" : null);
            else if (s.Dpm != null && s.Dpm.Mode == 1)
                add("Clock control", "Automatic clocks, ceiling " + s.Dpm.MaxMHz + " MHz. " + stored, "ok", null, null);
            else
                add("Clock control", (s.Dpm != null ? KmdReply.ModeText(s.Dpm.Mode) + ": " + KmdReply.ReasonText(runReason) + ". " :
                    lastMode != null ? "Last start: " + KmdReply.ModeText(lastMode.Value) + ". " : "") + stored, "info", null, null);

            // Graphics cores (CU mode, plan v7 section 7): the running start, the stored choice and the next start apart.
            var cuView = CuMode.View(s.Cu, s.Health != null ? (ulong?)s.Health.Generation : null, s.StoredCu());
            bool cuWarn = cuView.Class == CuClass.Fallback || cuView.Class == CuClass.NotSaved || cuView.Class == CuClass.Waiting ||
                (cuView.Class == CuClass.Unknown && cuView.ChoiceMode == CuMode.Full);
            add("Graphics cores", cuView.Running + " " + cuView.Choice + " " + cuView.NextStart +
                (s.Cu != null ? " (snapshot: applied " + s.Cu.Applied + ", flags " + s.Cu.Flags + ", reason " + CuMode.ReasonName(s.Cu.Reason) + ", active " + s.Cu.ActiveCus +
                    ", mask 0x" + s.Cu.DisableMask.ToString("X", CultureInfo.InvariantCulture) + ", generation " + s.Cu.Generation + ")" : ""),
                cuWarn ? "warn" : cuView.Class == CuClass.Standard || cuView.Class == CuClass.Confirmed ? "ok" : "info",
                cuView.OfferConfirm ? "cu-confirm" : null, cuView.OfferConfirm ? "Confirm now" : null);

            // The installer's start-confirm task.
            if (!s.TaskFound)
                add("Start confirmation task", "The release's logon task is not installed. Run the release installer again.", "warn", null, null);
            else
                add("Start confirmation task", "Last run " + (s.TaskLastRun ?? "unknown") + ": " + TaskResultText(s.TaskResult) + "." +
                    (string.IsNullOrEmpty(s.ConfirmLogLast) ? "" : " Log: " + s.ConfirmLogLast), s.TaskResult == 0 || s.TaskResult >= 0x41300 ? "ok" : "warn", null, null);
            return lines;
        }

        // ---- the plans -------------------------------------------------------------------------------------------

        static ActionPlan Refuse(ActionPlan p, string why) { p.Refusal = why; p.Writes.Clear(); return p; }

        static bool Same(RecoverySnapshot s, RegWrite w)
        {
            if (!string.Equals(w.Path, ParametersPath, StringComparison.OrdinalIgnoreCase))
                return w.Name == "DwmForceCpu" && !w.Delete && s.DwmForceCpu == w.Number;
            var now = s.P(w.Name);
            return w.Delete ? now == null : now != null && now.Value == w.Number;
        }

        // The plan of one action. ceiling: enable-dpm and set-clocks; mode: set-clocks; backups: undo, game-undo,
        // game-redo; more: cu-mode (Cu), reset-defaults (Games), game-* (Image, Value).
        public static ActionPlan Plan(string action, RecoverySnapshot s, uint? mode = null, uint? ceiling = null, IEnumerable<BackupRecord> backups = null, bool operatorAccepted = false,
            PlanArgs more = null)
        {
            more = more ?? new PlanArgs();
            var p = new ActionPlan { Action = action };
            if (action == OperatorEscape)
            {
                // The operator escape for a desktop that does not respond: not in Actions, so the window never offers it.
                p.Title = "Restart the desktop compositor (operator escape)";
                p.Change = "Stops the desktop compositor (DWM) of the active session; Windows starts a new one. Only for a desktop that does not respond when Windows cannot be restarted normally.";
                p.Effect = "at once";
                p.Undoable = false;
                if (!operatorAccepted)
                    return Refuse(p, "restart-compositor is an operator escape for a desktop that does not respond; the window does not offer it. Add --accept-bd060 to confirm the warning: after a DWM restart some Windows 11 apps can ignore mouse clicks until Windows restarts (BD-060).");
                p.Notes.Add("After a DWM restart some Windows 11 apps, for example the Explorer command bar and Task Manager, can ignore mouse clicks until Windows restarts (BD-060). Restart Windows as soon as you can.");
                p.Notes.Add("No setting changes. For the CPU route after the restart, run desktop-cpu first.");
                p.RestartCompositor = true;
                return p;
            }
            if (!Actions.Contains(action)) return Refuse(p, "unknown action " + action);
            if (!s.DriverInstalled) return Refuse(p, "The bc250kmd driver is not installed: there are no settings to change.");
            switch (action)
            {
                case "reopen-gpu-path":
                    p.Title = "Reopen the GPU desktop path";
                    p.Change = "Turns both GPU desktop switches on (EnableGpuPresentBlit 1, EnableCddDwmInterop 1) and removes the driver's close mark (InteropClosedReason).";
                    p.Effect = "at the next restart of Windows";
                    if (SwitchesRequested(s) && s.P("InteropClosedReason") == null)
                        return Refuse(p, SwitchesOpenNow(s) ? "The GPU desktop path is open already." : "The GPU desktop path is reopened already: restart Windows to use it.");
                    p.Writes.Add(RegWrite.Dword(ParametersPath, "EnableGpuPresentBlit", 1));
                    p.Writes.Add(RegWrite.Dword(ParametersPath, "EnableCddDwmInterop", 1));
                    if (s.P("InteropClosedReason") != null) p.Writes.Add(RegWrite.Remove(ParametersPath, "InteropClosedReason"));
                    p.OfferRestart = true;
                    var bd059 = Bd059Fixed(s);
                    if (bd059 == true)
                        p.Notes.Add("If a normal restart closes the path again, create a bug report on the Diagnostics page.");
                    else
                        p.Notes.Add((bd059 == false ? "Known issue BD-059" : "Known issue BD-059 of drivers before 0.7.198") +
                            ": a restart while the desktop runs on the GPU route can close the path again. The desktop stays on the route set under Desktop composition.");
                    break;

                case "desktop-gpu":
                case "desktop-cpu":
                    bool gpu = action == "desktop-gpu";
                    p.Title = gpu ? "Desktop composition on the GPU" : "Desktop composition on the CPU";
                    p.Change = "Sets DwmForceCpu " + (gpu ? 0 : 1) + ": the desktop (DWM) uses the " + (gpu ? "GPU" : "CPU") + " route from the next restart of Windows.";
                    p.Effect = "at the next restart of Windows";
                    if (!s.RouterInstalled) return Refuse(p, "The release's desktop router is not installed.");
                    if (gpu)
                    {
                        if (!s.DriverRunning) return Refuse(p, "The driver is not running, so the GPU route cannot start.");
                        if (!SwitchesOpenNow(s))
                            return Refuse(p, "The GPU desktop path is closed for this start. Use \"Reopen the GPU desktop path\" first, then restart Windows.");
                        if (!s.GpuDesktopModules) return Refuse(p, "The GPU desktop files of the release are missing. Run the release installer again.");
                    }
                    p.Writes.Add(RegWrite.Dword(RouterPath, "DwmForceCpu", gpu ? 0u : 1u));
                    if (Same(s, p.Writes[0]) && s.DwmRoute == (gpu ? "gpu" : "cpu"))
                        return Refuse(p, "The desktop already runs on the " + (gpu ? "GPU" : "CPU") + " route.");
                    if (Same(s, p.Writes[0]))
                        return Refuse(p, "The " + (gpu ? "GPU" : "CPU") + " route is selected already: restart Windows to use it.");
                    p.Notes.Add(Bd060Note);
                    p.OfferRestart = true;
                    break;

                case "confirm-start":
                    p.Title = "Confirm this start";
                    p.Change = "Tells the driver that this start is healthy. The driver resets its unconfirmed-start counter and keeps automatic clocks if they are on trial.";
                    p.Effect = "at once";
                    if (!s.DriverRunning) return Refuse(p, "The driver is not running: there is no start to confirm.");
                    if (Confirmed(s.Health)) return Refuse(p, "This start is confirmed already.");
                    var blocker = ConfirmBlocker(s.Health, false);
                    if (blocker != null) return Refuse(p, "This start cannot be confirmed now: " + blocker + ".");
                    p.ConfirmStart = true; p.Undoable = false;
                    p.Notes.Add("A confirmation cannot be undone: it only clears the driver's own counter.");
                    break;

                case "enable-dpm":
                case "set-clocks":
                    // set-clocks: the Performance page's two boxes, mode 1 or null (unchecked: removed), ceiling a
                    // grid value or null (unchecked: removed). enable-dpm: DpmMode 1, and the ceiling only when chosen.
                    bool automatic = action == "enable-dpm" || mode == 1;
                    p.Title = automatic ? "Automatic clocks" : "Clock settings";
                    if (action == "set-clocks" && mode != null && mode != 1)
                        return Refuse(p, "Only DpmMode 1 is written; leave automatic clocks unchecked for the fixed clock (driver default).");
                    if (ceiling != null && !DpmSettings.IsValidCeiling(ceiling.Value)) return Refuse(p, "The ceiling " + ceiling + " MHz is not one of 1000, 1100 ... 2000 MHz.");
                    var stored = s.P("DpmMaxMHz");
                    var clock = DpmSettings.PlanWrites(s.P("DpmMode"), stored, automatic, action == "enable-dpm" ? ceiling ?? stored : ceiling);
                    if (action == "enable-dpm" && ceiling == null) clock.RemoveAll(w => w.Name == "DpmMaxMHz");
                    p.Writes.AddRange(clock);
                    if (p.Writes.Count == 0) return Refuse(p, "These clock settings are stored already.");
                    p.Change = string.Join(" ", p.Writes.Select(w => w.Name == "DpmMode" ? (w.Delete ? "Removes DpmMode: the fixed clock of 1000 MHz (driver default)." : "Automatic clocks (DpmMode 1).")
                        : w.Delete ? "Removes the clock ceiling: " + DpmSettings.DefaultMaxMHz + " MHz (driver default)." : "Clock ceiling " + w.Number + " MHz (DpmMaxMHz)."));
                    p.Effect = "at the next restart of Windows";
                    if (ceiling > DpmSettings.DefaultMaxMHz && automatic) p.Notes.Add("A ceiling above " + DpmSettings.DefaultMaxMHz + " MHz makes the GPU hotter and uses more power. Make sure the case has good air flow.");
                    p.OfferRestart = true;
                    break;

                case "reset-defaults":
                    p.Title = "Reset driver settings to the release defaults";
                    p.Effect = "at the next restart of Windows";
                    if (s.DefaultParameters == null || s.DefaultRouter == null)
                        return Refuse(p, "The installed release has no list of default settings" + (s.DefaultsError != null ? " (" + s.DefaultsError + ")" : "") + ". Run the release installer again to reset.");
                    foreach (var name in DefaultParameterNames)
                    {
                        long v;
                        if (!s.DefaultParameters.TryGetValue(name, out v)) return Refuse(p, "The release defaults do not name " + name + ".");
                        bool ok = name == "DpmMaxMHz" ? DpmSettings.IsValidCeiling((uint)v) : v == 0 || v == 1;
                        if (v < 0 || v > uint.MaxValue || !ok) return Refuse(p, "The release default " + name + " = " + v + " is out of range.");
                        p.Writes.Add(RegWrite.Dword(ParametersPath, name, (uint)v));
                    }
                    long cpu;
                    if (!s.DefaultRouter.TryGetValue("DwmForceCpu", out cpu) || (cpu != 0 && cpu != 1)) return Refuse(p, "The release defaults have no valid DwmForceCpu.");
                    p.Writes.Add(RegWrite.Dword(RouterPath, "DwmForceCpu", (uint)cpu));
                    if (s.DefaultParameters["EnableGpuPresentBlit"] == 1 && s.DefaultParameters["EnableCddDwmInterop"] == 1 && s.P("InteropClosedReason") != null)
                        p.Writes.Add(RegWrite.Remove(ParametersPath, "InteropClosedReason"));
                    int others = s.DefaultParameters.Keys.Count(k => !DefaultParameterNames.Contains(k));
                    if (others > 0) p.Notes.Add("The other " + others + " driver settings of the release are left to the installer: run it again to reset them.");
                    // The CU part goes through the same setter as "Standard (24)" (WU-042, WU-055).
                    var cu = CuMode.Plan(s.StoredCu(), CuMode.Stock);
                    if (!cu.Refused) { p.Cu = cu; p.Preview.AddRange(cu.Preview); }
                    else if (s.StoredCu().Unreadable) p.Notes.Add("The graphics-core setting cannot be read and is left as it is.");
                    // Per-game switches: kept unless the user asked to reset them too (WU-055).
                    if (more.Games == "reset")
                    {
                        if (s.DefaultApplications == null) return Refuse(p, "The installed release has no list of recommended game settings. Keep the game settings or run the release installer again.");
                        foreach (var g in (s.GameProfiles ?? new Dictionary<string, string>()))
                        {
                            string def;
                            bool recommended = s.DefaultApplications.TryGetValue(g.Key, out def);
                            if (recommended && !SameSet(g.Value, def)) p.GameWrites[g.Key] = def;
                            else if (!recommended) p.GameWrites[g.Key] = "";
                        }
                        foreach (var d in s.DefaultApplications)
                            if (s.GameProfiles == null || !s.GameProfiles.ContainsKey(d.Key)) p.GameWrites[d.Key] = d.Value;
                    }
                    else p.Notes.Add("Per-game settings are kept.");
                    if (p.Writes.All(w => Same(s, w)) && p.Cu == null && p.GameWrites.Count == 0) return Refuse(p, "All these settings have their release defaults already.");
                    p.Change = "Sets " + string.Join(", ", p.Writes.Select(w => w.Delete ? "removes " + w.Name : w.Name + " " + w.Number)) +
                        (p.Cu != null ? "; graphics cores: Standard (24)" : "") + (p.GameWrites.Count > 0 ? "; game settings: " + p.GameWrites.Count + " games" : "") + ".";
                    if (p.Cu != null) p.Notes.Add("The graphics-core part cannot be undone: its old values are kept for diagnosis only.");
                    p.OfferRestart = true;
                    break;

                case "undo":
                    var target = UndoTarget(backups ?? new BackupRecord[0]);
                    p.Title = "Undo last action";
                    p.Undoable = false;
                    if (target == null) return Refuse(p, "There is no action to undo.");
                    p.UndoOf = target.File;
                    foreach (var v in target.Values)
                    {
                        string image = GameImage(v.Path);
                        if (image != null && v.Name == Profiles.ValueName) { p.GameWrites[image] = v.Existed ? v.Text ?? "" : ""; continue; }
                        if (!Allowed(v.Path, v.Name)) return Refuse(p, "The backup names " + v.Name + ", which this app does not change.");
                        p.Writes.Add(v.Existed ? new RegWrite { Path = v.Path, Name = v.Name, Kind = v.Kind, Number = v.Number, Text = v.Text } : RegWrite.Remove(v.Path, v.Name));
                    }
                    p.Change = "Restores the values found before \"" + target.Action + "\" of " + target.Utc + ".";
                    var router = p.Writes.FirstOrDefault(w => w.Name == "DwmForceCpu");
                    if (router != null && (router.Delete || router.Number == 0) && !SwitchesOpenNow(s))
                        return Refuse(p, "Undo would put the desktop on the GPU route, but the GPU desktop path is closed for this start.");
                    if (router != null) p.Notes.Add(Bd060Note);
                    if (target.Diagnosis != null && target.Diagnosis.Count > 0) p.Notes.Add("The graphics-core part of that action is not undone (S6).");
                    p.Effect = "at the next restart of Windows";
                    p.OfferRestart = true;
                    break;

                case "cu-mode":
                    p.Title = "Graphics cores";
                    p.Effect = "at the next restart of Windows";
                    p.Undoable = false;
                    if (more.Cu == null) return Refuse(p, "No graphics-core choice given (--cu 24 or 40).");
                    p.Cu = CuMode.Plan(s.StoredCu(), more.Cu.Value);
                    if (p.Cu.Refused) { var why = p.Cu.Refusal; p.Cu = null; return Refuse(p, why); }
                    p.Preview.AddRange(p.Cu.Preview);
                    p.Change = string.Join(" ", p.Cu.Preview);
                    if (more.Cu == CuMode.Full) { p.Preview.Add(CuMode.EffectText()); p.Notes.Add("Effect: more power and heat, reference about +30 W at 1500 MHz on the lab unit, not a promise."); }
                    if (p.Cu.Before.Disable != null) p.Notes.Add("CuDisableWgp was 0x" + p.Cu.Before.Disable.Value.ToString("X", CultureInfo.InvariantCulture) + " (an earlier diagnostic core limit) and is removed.");
                    p.Notes.Add("The old values are saved for diagnosis only; they are never restored and undo does not offer them (S6).");
                    p.OfferRestart = true;
                    break;

                case "cu-confirm":
                    p.Title = "Confirm 40 graphics cores";
                    p.Change = "Tells the driver that this 40-core start is healthy, so the next start keeps 40 cores.";
                    p.Effect = "at once";
                    p.Undoable = false;
                    if (!s.DriverRunning) return Refuse(p, "The driver is not running: there is no start to confirm.");
                    if (CuMode.Classify(s.Cu, s.Health != null ? (ulong?)s.Health.Generation : null) != CuClass.Waiting)
                        return Refuse(p, "No 40-core start is waiting for confirmation.");
                    p.CuConfirm = true;
                    break;

                case "game-profile":
                {
                    p.Title = "Game settings";
                    p.Undoable = true;
                    if (more.Image == null || !GameAllowed(more.Image, more.Value ?? "")) return Refuse(p, "Not a game file name or not a valid list of switches.");
                    string current = null;
                    if (s.GameProfiles != null) s.GameProfiles.TryGetValue(more.Image, out current);
                    var names = (more.Value ?? "").Split(new[] { ',' }, StringSplitOptions.RemoveEmptyEntries);
                    ProfileWrite pw;
                    try { pw = Profiles.PlanWrite(more.Image, string.IsNullOrEmpty(current) ? null : current, names); }
                    catch (ArgumentException e) { return Refuse(p, e.Message); }
                    if (pw.Kind == ProfileWriteKind.None) return Refuse(p, "These settings of " + more.Image + " are stored already.");
                    p.GameWrites[more.Image] = pw.Kind == ProfileWriteKind.Set ? pw.Value : "";
                    p.Effect = "the next time " + more.Image + " starts";
                    p.Change = pw.Kind == ProfileWriteKind.Set ? "Sets the switches of " + more.Image + " to " + pw.Value + "." : "Removes the settings of " + more.Image + ".";
                    p.Notes.Add("These settings are shared by every game named " + more.Image + " on this PC.");
                    break;
                }

                case "game-undo":
                case "game-redo":
                {
                    bool redo = action == "game-redo";
                    p.Title = redo ? "Redo the game change" : "Undo the game change";
                    p.Undoable = true;
                    if (more.Image == null || !Profiles.IsValidImage(more.Image)) return Refuse(p, "Not a game file name.");
                    var t = redo ? GameRedoTarget(backups ?? new BackupRecord[0], more.Image) : GameUndoTarget(backups ?? new BackupRecord[0], more.Image);
                    if (t == null) return Refuse(p, redo ? "There is no undone change of " + more.Image + " to redo." : "There is no change of " + more.Image + " to undo.");
                    p.UndoOf = t.File;
                    var v = t.Values.FirstOrDefault(x => GameImage(x.Path) != null && string.Equals(GameImage(x.Path), more.Image, StringComparison.OrdinalIgnoreCase) && x.Name == Profiles.ValueName);
                    if (v == null) return Refuse(p, "The saved change does not name " + more.Image + ".");
                    p.GameWrites[more.Image] = v.Existed ? v.Text ?? "" : "";
                    p.Effect = "the next time " + more.Image + " starts";
                    p.Change = (redo ? "Restores the settings of " + more.Image + " from before the undo of " : "Restores the settings of " + more.Image + " from before the change of ") + t.Utc + ".";
                    break;
                }
            }
            foreach (var w in p.Writes)
                if (!Allowed(w.Path, w.Name)) return Refuse(p, "internal: " + w.Name + " is not on the list of values this app may write");
            foreach (var g in p.GameWrites)
                if (!GameAllowed(g.Key, g.Value)) return Refuse(p, "internal: " + g.Key + " is not a game this app may change");
            if (p.Cu != null && !p.Cu.Steps.All(CuMode.Allowed)) return Refuse(p, "internal: a graphics-core step is not allowed");
            return p;
        }

        public sealed class PlanArgs
        {
            public uint? Cu;
            public string Games, Image, Value;
        }

        static bool SameSet(string a, string b)
        {
            Func<string, HashSet<string>> set = x => new HashSet<string>((x ?? "").Split(new[] { ',' }, StringSplitOptions.RemoveEmptyEntries), StringComparer.Ordinal);
            return set(a).SetEquals(set(b));
        }

        // The image of a per-game backup value's path, or null for any other path.
        public static string GameImage(string path)
        {
            var prefix = Profiles.RegistryPath + "\\";
            if (path == null || !path.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)) return null;
            var image = path.Substring(prefix.Length);
            return Profiles.IsValidImage(image) ? image : null;
        }

        static bool IsGameRecord(BackupRecord b) { return b.Action == "game-profile" || b.Action == "game-undo" || b.Action == "game-redo"; }

        static List<BackupRecord> GameRecords(IEnumerable<BackupRecord> backups, string image)
        {
            return backups.Where(b => IsGameRecord(b) && b.File != null && b.Args != null && b.Args.Any(a => string.Equals(a, "image=" + image, StringComparison.OrdinalIgnoreCase)))
                .OrderByDescending(b => b.Utc, StringComparer.Ordinal).ThenByDescending(b => b.File, StringComparer.Ordinal).ToList();
        }

        // Undo: the newest change (or redo) of this game not undone yet.
        public static BackupRecord GameUndoTarget(IEnumerable<BackupRecord> backups, string image)
        {
            var list = GameRecords(backups, image);
            var undone = new HashSet<string>(list.Where(b => b.Undoes != null).Select(b => b.Undoes), StringComparer.OrdinalIgnoreCase);
            return list.FirstOrDefault(b => (b.Action == "game-profile" || b.Action == "game-redo") && !undone.Contains(b.File));
        }

        // Redo: the newest undo of this game not redone yet, when no change came after it (a new change clears the redo).
        public static BackupRecord GameRedoTarget(IEnumerable<BackupRecord> backups, string image)
        {
            var list = GameRecords(backups, image);
            var undone = new HashSet<string>(list.Where(b => b.Undoes != null).Select(b => b.Undoes), StringComparer.OrdinalIgnoreCase);
            foreach (var b in list)
            {
                if (b.Action == "game-profile") return null;
                if (b.Action == "game-undo" && !undone.Contains(b.File)) return b;
            }
            return null;
        }

        // The newest backup an undo has not restored yet, among the undoable ones. Per-game changes have their own undo.
        public static BackupRecord UndoTarget(IEnumerable<BackupRecord> backups)
        {
            var list = backups.ToList();
            var undone = new HashSet<string>(list.Where(b => b.Undoes != null).Select(b => b.Undoes), StringComparer.OrdinalIgnoreCase);
            return list.Where(b => b.Undoable && b.File != null && !undone.Contains(b.File) && !IsGameRecord(b))
                .OrderByDescending(b => b.Utc, StringComparer.Ordinal).ThenByDescending(b => b.File, StringComparer.Ordinal).FirstOrDefault();
        }

        public static string BackupFileName(DateTime utc)
        {
            return "backup-" + utc.ToString("yyyyMMdd'T'HHmmssfff'Z'", CultureInfo.InvariantCulture) + ".json";
        }
    }
}
