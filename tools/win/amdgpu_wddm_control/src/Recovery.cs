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

        public RecoverySnapshot() { Parameters = new Dictionary<string, long>(); DwmRoute = "unknown"; }

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
        public bool RestartDwm, WatchDwm, ConfirmStart, OfferRestart, Undoable = true;
        public string UndoOf;                  // undo: the backup file it restores
        public uint DwmForceCpuAfter = 1;      // RestartDwm: the route the restart should end on (0 GPU, 1 CPU)

        public bool Refused { get { return Refusal != null; } }

        public string Text()
        {
            var w = new StringBuilder();
            w.AppendLine("action: " + Action);
            if (Refused) { w.AppendLine("refused: " + Refusal); return w.ToString(); }
            w.AppendLine("change: " + Change);
            w.AppendLine("takes effect: " + Effect);
            foreach (var x in Writes) w.AppendLine("  " + x);
            if (ConfirmStart) w.AppendLine("  confirm this driver start (start-health CONFIRM with this start's generation and epoch)");
            if (RestartDwm) w.AppendLine("  restart DWM" + (WatchDwm ? ", watch it 60 s, on a crash or replacement set DwmForceCpu 1 and restart DWM again" : ""));
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
        public bool RestartsDwm { get; set; }
        public string Undoes { get; set; }      // the backup file an undo restored
        public List<BackupValue> Values { get; set; }
        public string File { get; set; }        // set when read, not stored

        public BackupRecord() { Schema = 1; Values = new List<BackupValue>(); }
    }

    public static class Recovery
    {
        public const string ParametersPath = DpmSettings.RegistryPath;
        public const string RouterPath = KmdReply.DesktopRouterPath;
        public const uint MaxUnconfirmedStarts = 2;         // BC250_MAX_UNCONFIRMED_STARTS, driver/kmd/bc250kmd.h
        // start-confirm-core.ps1: $StartConfirmMinReadyMs, $StartConfirmFreshMs, Test-StartConfirmEligible. The unit
        // tests compare these with that file when the build is given it.
        public const ulong ConfirmMinReadyMs = 60000, ConfirmFreshMs = 5000;
        public const uint ConfirmRequiredFlags = 7;         // BC250_START_HEALTH_REQUIRED (FULL | READY | VISIBLE)
        public const int WatchSeconds = 60, DwmStartSeconds = 20;

        // The only values an action or an undo may write. Never: the temperature limits, firmware paths, the other
        // KMD service values, BIOS or firmware settings, test signing.
        public static readonly string[] ParameterNames = { "EnableGpuPresentBlit", "EnableCddDwmInterop", "InteropClosedReason", "DpmMode", "DpmMaxMHz" };
        public static readonly string[] RouterNames = { "DwmForceCpu" };
        // Defaults the reset takes from manifest.json (the rest of the release's table is not the app's to touch).
        public static readonly string[] DefaultParameterNames = { "EnableGpuPresentBlit", "EnableCddDwmInterop", "DpmMode", "DpmMaxMHz" };

        public static readonly string[] Actions = { "reopen-gpu-path", "desktop-gpu", "desktop-cpu", "confirm-start", "enable-dpm", "set-clocks", "reset-defaults", "undo" };

        public static bool Allowed(string path, string name)
        {
            if (string.Equals(path, ParametersPath, StringComparison.OrdinalIgnoreCase)) return ParameterNames.Contains(name);
            if (string.Equals(path, RouterPath, StringComparison.OrdinalIgnoreCase)) return RouterNames.Contains(name);
            return false;
        }

        // ---- the decision rules ----------------------------------------------------------------------------------

        // Test-StartConfirmEligible of start-confirm-core.ps1, field for field.
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

        // The DWM watchdog's verdict after a restart: null while it waits, "ok" when DWM held for the whole window,
        // else the failure. firstPid: the first DWM that started after the restart; dwmNow: the DWMs started since.
        // windowSeconds: WatchSeconds onto the GPU route, a short settle time onto the CPU route.
        public static string WatchVerdict(int? firstPid, ICollection<int> dwmNow, int crashes, double elapsedSeconds, double windowSeconds)
        {
            if (crashes > 0) return "DWM crashed (Application Error 1000, dwm.exe)";
            if (firstPid == null) return elapsedSeconds >= DwmStartSeconds ? "no DWM started within " + DwmStartSeconds + " s" : null;
            if (!dwmNow.Contains(firstPid.Value)) return "DWM exited and was replaced";
            if (dwmNow.Any(p => p != firstPid.Value)) return "a second DWM started";
            return elapsedSeconds >= windowSeconds ? "ok" : null;
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
                add("GPU desktop path", "Closed by the driver: it took the last shutdown as unclean (reason " + closed + "). A normal restart can cause this (BD-059)." + session,
                    "warn", "reopen-gpu-path", "Reopen the GPU desktop path");
            else
            {
                uint reason = s.Interop != null ? s.Interop.Reason : (s.P("InteropLastReason") ?? 1);
                add("GPU desktop path", "Closed: " + InteropReasonText(reason == 0 ? 1 : reason) + "." + session, "info", "reopen-gpu-path", "Reopen the GPU desktop path");
            }

            // Desktop composition: the router's choice and the route DWM really took.
            string seen = s.DwmRoute == "unknown" ? "" : " DWM now runs on the " + s.DwmRoute.ToUpperInvariant() + " route" + (string.IsNullOrEmpty(s.DwmRouteDetail) ? "" : " (" + s.DwmRouteDetail + ")") + ".";
            if (!s.RouterInstalled)
                add("Desktop composition", "The release's desktop router is not installed." + seen, "info", null, null);
            else if ((s.DwmForceCpu ?? 0) != 0)
                add("Desktop composition", "CPU route (GPU route disabled, BD-058). This is the release default." + seen, "ok", null, null);
            else if (!openNow)
                add("Desktop composition", "GPU route selected, but the GPU desktop path is closed, so DWM stays on the CPU route." + seen, "warn",
                    requested && closed == null ? "restart" : "reopen-gpu-path", requested && closed == null ? "Restart Windows" : "Reopen the GPU desktop path");
            else
                add("Desktop composition", "GPU route selected. If the desktop goes black or restarts, switch it back to the CPU route." + seen,
                    s.DwmRoute == "cpu" ? "warn" : "info", "desktop-cpu", "Desktop on the CPU route");

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

        // The plan of one action. ceiling: enable-dpm and set-clocks; mode: set-clocks; backups: undo.
        public static ActionPlan Plan(string action, RecoverySnapshot s, uint? mode = null, uint? ceiling = null, IEnumerable<BackupRecord> backups = null)
        {
            var p = new ActionPlan { Action = action };
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
                    p.Notes.Add("Known issue BD-059: a restart while the desktop runs on the GPU route can close the path again. The desktop stays on the route set under Desktop composition.");
                    break;

                case "desktop-gpu":
                case "desktop-cpu":
                    bool gpu = action == "desktop-gpu";
                    p.Title = gpu ? "Desktop composition on the GPU" : "Desktop composition on the CPU";
                    p.Change = "Sets DwmForceCpu " + (gpu ? 0 : 1) + " and restarts the desktop (DWM): the screen goes black for a few seconds." +
                        (gpu ? " The app watches DWM for " + WatchSeconds + " s; if it crashes or is replaced, it sets the CPU route again and restarts DWM once more." : "");
                    p.Effect = "at once (DWM restart); open windows stay open";
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
                    p.RestartDwm = true; p.WatchDwm = gpu; p.DwmForceCpuAfter = gpu ? 0u : 1u;
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
                    uint m = action == "enable-dpm" ? 1u : mode ?? 99;
                    uint c = ceiling ?? DpmSettings.DefaultMaxMHz;
                    p.Title = m == 1 ? "Automatic clocks" : "Fixed clock";
                    if (!DpmSettings.IsValidMode(m)) return Refuse(p, "Clock mode " + m + " is not 0 (fixed) or 1 (automatic).");
                    if (!DpmSettings.IsValidCeiling(c)) return Refuse(p, "The ceiling " + c + " MHz is not one of 1000, 1100 ... 2000 MHz.");
                    p.Change = m == 1 ? "Automatic clocks (DpmMode 1) with a ceiling of " + c + " MHz (DpmMaxMHz)." : "Fixed clock of 1000 MHz (DpmMode 0); the stored ceiling becomes " + c + " MHz.";
                    p.Effect = "at the next restart of Windows";
                    p.Writes.Add(RegWrite.Dword(ParametersPath, "DpmMode", m));
                    p.Writes.Add(RegWrite.Dword(ParametersPath, "DpmMaxMHz", c));
                    if (p.Writes.All(w => Same(s, w))) return Refuse(p, "These clock settings are stored already.");
                    if (m == 1 && c > DpmSettings.DefaultMaxMHz) p.Notes.Add("A ceiling above " + DpmSettings.DefaultMaxMHz + " MHz makes the GPU hotter and uses more power. Make sure the case has good air flow.");
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
                    foreach (var kv in s.DefaultParameters.Where(kv => !DefaultParameterNames.Contains(kv.Key)))
                        p.Notes.Add("Not reset by this app: " + kv.Key + " (reinstall the release for the other driver settings).");
                    if (p.Writes.All(w => Same(s, w))) return Refuse(p, "All these settings have their release defaults already.");
                    p.Change = "Sets " + string.Join(", ", p.Writes.Select(w => w.Delete ? "removes " + w.Name : w.Name + " " + w.Number)) + ".";
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
                        if (!Allowed(v.Path, v.Name)) return Refuse(p, "The backup names " + v.Name + ", which this app does not change.");
                        p.Writes.Add(v.Existed ? new RegWrite { Path = v.Path, Name = v.Name, Kind = v.Kind, Number = v.Number, Text = v.Text } : RegWrite.Remove(v.Path, v.Name));
                    }
                    p.Change = "Restores the values found before \"" + target.Action + "\" of " + target.Utc + ".";
                    var router = p.Writes.FirstOrDefault(w => w.Name == "DwmForceCpu");
                    if (target.RestartsDwm && router != null)
                    {
                        uint after = router.Delete ? 0u : (uint)router.Number;
                        if (after == 0 && !SwitchesOpenNow(s))
                            return Refuse(p, "Undo would put the desktop on the GPU route, but the GPU desktop path is closed for this start.");
                        p.RestartDwm = true; p.WatchDwm = after == 0; p.DwmForceCpuAfter = after;
                        p.Effect = "at once (DWM restart)";
                    }
                    else { p.Effect = "at the next restart of Windows"; p.OfferRestart = true; }
                    break;
            }
            foreach (var w in p.Writes)
                if (!Allowed(w.Path, w.Name)) return Refuse(p, "internal: " + w.Name + " is not on the list of values this app may write");
            return p;
        }

        // The newest backup an undo has not restored yet, among the undoable ones.
        public static BackupRecord UndoTarget(IEnumerable<BackupRecord> backups)
        {
            var list = backups.ToList();
            var undone = new HashSet<string>(list.Where(b => b.Undoes != null).Select(b => b.Undoes), StringComparer.OrdinalIgnoreCase);
            return list.Where(b => b.Undoable && b.File != null && !undone.Contains(b.File))
                .OrderByDescending(b => b.Utc, StringComparer.Ordinal).ThenByDescending(b => b.File, StringComparer.Ordinal).FirstOrDefault();
        }

        public static string BackupFileName(DateTime utc)
        {
            return "backup-" + utc.ToString("yyyyMMdd'T'HHmmssfff'Z'", CultureInfo.InvariantCulture) + ".json";
        }
    }
}
