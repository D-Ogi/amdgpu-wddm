// Pure contract for the optional Windows settings. No registry, service, task or process access here.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Web.Script.Serialization;

namespace AmdgpuWddmControl
{
    public sealed class SystemTuningEntry
    {
        public string Id, Key, Kind, Scope = "Machine";
        public bool Recommended;
        public string Name { get { return Strings.T("system.item." + Key); } }
    }

    public sealed class SystemTuningItem
    {
        public string Id, State = "unavailable", Note = "", RequestedUntil, ObservedState = "unknown";
        public bool Present, Managed, CanApply, CanRestore, Recommended;
    }

    public sealed class SystemTuningSnapshot
    {
        public bool Ok;
        public readonly List<SystemTuningItem> Items = new List<SystemTuningItem>();
        public SystemTuningItem Find(string id) { return Items.FirstOrDefault(x => x.Id == id); }
    }

    public static class SystemTuning
    {
        public const int MaxJson = 1024 * 1024;
        public static readonly SystemTuningEntry[] Catalog =
        {
            E("service.SysMain", "sysmain", "service", false),
            E("service.WSearch", "search", "service", false),
            E("service.DiagTrack", "diagnostics", "service", true),
            E("service.MapsBroker", "maps", "service", true),
            E("task.ScheduledDefrag", "drive", "task", false),
            E("task.CompatibilityAppraiser", "compatibility", "task", false),
            E("task.ProgramDataUpdater", "program-data", "task", true),
            E("task.Consolidator", "experience", "task", true),
            E("task.UsbCeip", "usb", "task", true),
            E("autostart.TeamsMachineInstaller", "teams", "autostart", false),
            E("policy.DriverUpdates", "drivers", "driverPolicy", false),
            E("policy.UpdatePause", "updates", "pausePolicy", false),
            new SystemTuningEntry { Id = "autostart.OneDrive", Key = "onedrive", Kind = "autostart", Scope = "User" },
            new SystemTuningEntry { Id = "autostart.Teams", Key = "teams-user", Kind = "autostart", Scope = "User" },
        };

        static SystemTuningEntry E(string id, string key, string kind, bool recommended)
        { return new SystemTuningEntry { Id = id, Key = key, Kind = kind, Recommended = recommended }; }

        public static SystemTuningEntry Entry(string id) { return Catalog.FirstOrDefault(x => x.Id == id); }

        public static string[] Arguments(string action, string item = null, int days = 0, string scope = "Machine")
        {
            if (scope != "Machine" && scope != "User") throw new ArgumentException("scope");
            var args = new List<string> { "--system-tuning", scope, action };
            switch (action)
            {
                case "Apply":
                case "Restore":
                    if (Entry(item) == null || Entry(item).Scope != scope || item == "policy.UpdatePause" || days != 0) throw new ArgumentException("item");
                    args.Add(item); break;
                case "PauseUpdates":
                    if (scope != "Machine" || item != null || days < 1 || days > 35) throw new ArgumentException("days");
                    args.Add(days.ToString(CultureInfo.InvariantCulture)); break;
                case "List":
                case "RestoreAll":
                case "ApplyRecommended":
                case "ResumeUpdates":
                    if (item != null || days != 0) throw new ArgumentException("arguments");
                    if (action == "ResumeUpdates" && scope != "Machine") throw new ArgumentException("scope");
                    break;
                default: throw new ArgumentException("action");
            }
            return args.ToArray();
        }

        // Accept only the exact command-line forms emitted above. Neither paths nor arbitrary shell text are inputs.
        public static bool TryArguments(string[] args, out string action, out string item, out int days, out string scope)
        {
            action = item = scope = null; days = 0;
            if (args == null || args.Length < 3 || args.Length > 4 || args[0] != "--system-tuning") return false;
            scope = args[1]; action = args[2];
            if (args.Length == 4)
            {
                if (action == "PauseUpdates")
                {
                    if (!int.TryParse(args[3], NumberStyles.None, CultureInfo.InvariantCulture, out days)) return false;
                }
                else item = args[3];
            }
            try { return Arguments(action, item, days, scope).SequenceEqual(args); }
            catch (ArgumentException) { return false; }
        }

        static object Required(IDictionary<string, object> obj, string key)
        { object value; if (!obj.TryGetValue(key, out value)) throw new FormatException("missing field"); return value; }
        static bool Bool(IDictionary<string, object> obj, string key)
        { var value = Required(obj, key); if (!(value is bool)) throw new FormatException("boolean field"); return (bool)value; }
        static string Text(IDictionary<string, object> obj, string key, int bound)
        { var value = Required(obj, key) as string; if (value == null || value.Length > bound) throw new FormatException("text field"); return value; }

        public static SystemTuningSnapshot Parse(string json, string scope = "Machine", string action = "List")
        {
            if (scope != "Machine" && scope != "User") throw new FormatException("scope");
            if (string.IsNullOrWhiteSpace(json) || json.Length > MaxJson) throw new FormatException("size");
            var root = new JavaScriptSerializer { MaxJsonLength = MaxJson, RecursionLimit = 16 }.DeserializeObject(json) as IDictionary<string, object>;
            if (root == null || !(Required(root, "schema") is int) || (int)root["schema"] != 1) throw new FormatException("schema");
            if (Text(root, "scope", 16) != scope || Text(root, "action", 32) != action || Bool(root, "dryRun")) throw new FormatException("response identity");
            var result = new SystemTuningSnapshot { Ok = Bool(root, "ok") };
            Text(root, "error", 4096); // Diagnostic details may contain paths. They never become user-facing text.
            var items = Required(root, "items") as object[];
            if (items == null || items.Length > Catalog.Length) throw new FormatException("items");
            var seen = new HashSet<string>(StringComparer.Ordinal);
            foreach (var raw in items)
            {
                var obj = raw as IDictionary<string, object>;
                if (obj == null) throw new FormatException("item");
                var id = Text(obj, "id", 80);
                var entry = Entry(id);
                if (entry == null || entry.Scope != scope || !seen.Add(id) || Text(obj, "kind", 20) != entry.Kind) throw new FormatException("id");
                var item = new SystemTuningItem { Id = id, Present = Bool(obj, "present"), Managed = Bool(obj, "managed"),
                    CanApply = Bool(obj, "canApply"), CanRestore = Bool(obj, "canRestore"), Recommended = Bool(obj, "recommended"), Note = Text(obj, "note", 4096) };
                if (item.Recommended && (entry.Scope == "User" || entry.Kind == "driverPolicy" || entry.Kind == "pausePolicy")) throw new FormatException("explicit opt-in required");
                Required(obj, "current"); // Preserve the backend's typed current state in its report, not in display text.
                item.State = Text(obj, "displayState", 32);
                if (!new[] { "absent", "unavailable", "enabled", "disabled", "mixed", "configured", "notConfigured" }.Contains(item.State))
                    throw new FormatException("display state");
                if (id == "policy.UpdatePause")
                {
                    object until;
                    if (obj.TryGetValue("requestedUntil", out until) && until != null)
                    {
                        DateTime date;
                        if (!(until is string) || !DateTime.TryParseExact((string)until, "yyyy-MM-dd", CultureInfo.InvariantCulture, DateTimeStyles.None, out date))
                            throw new FormatException("pause date");
                        item.RequestedUntil = (string)until;
                    }
                    object observed;
                    if (obj.TryGetValue("observedDisplayState", out observed))
                    {
                        if (!(observed is string) || !new[] { "paused", "resumed", "mixed", "unknown" }.Contains((string)observed)) throw new FormatException("observed state");
                        item.ObservedState = (string)observed;
                    }
                }
                // A failed or partial inventory cannot authorize a write from the UI.
                if (!result.Ok) item.CanApply = item.CanRestore = false;
                else if (item.State == "unavailable") item.CanApply = false;
                result.Items.Add(item);
            }
            if (result.Ok && result.Items.Count != Catalog.Count(x => x.Scope == scope)) throw new FormatException("incomplete catalog");
            return result;
        }

        public static string StateText(SystemTuningItem item)
        {
            if (item == null) return Strings.T("system.state.unknown");
            // Only fixed translation keys. An unknown backend value is not an instruction or a claim of success.
            switch (item.State)
            {
                case "enabled": case "disabled": case "configured": case "mixed": case "absent":
                    return Strings.T("system.state." + item.State);
                case "notConfigured": return Strings.T("system.state.not-configured");
                case "unavailable": return Strings.T("system.state.unavailable");
                default: return Strings.T("system.state.unknown");
            }
        }

        public static SystemTuningSnapshot Fixture()
        {
            var snapshot = new SystemTuningSnapshot { Ok = true };
            foreach (var entry in Catalog)
                snapshot.Items.Add(new SystemTuningItem { Id = entry.Id, Present = true, State = "enabled", CanApply = true });
            return snapshot;
        }
    }
}
