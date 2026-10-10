using System;
using System.Collections.Generic;
using System.Linq;
using System.IO;
using System.Web.Script.Serialization;
using AmdgpuWddmControl;

static partial class UnitTests
{
    static void SystemTuningTests()
    {
        var serializer = new JavaScriptSerializer();
        Func<Dictionary<string, object>> fixture = () => new Dictionary<string, object>
        {
            { "schema", 1 }, { "ok", true }, { "error", "" },
            { "scope", "Machine" }, { "action", "List" }, { "dryRun", false },
            { "items", SystemTuning.Catalog.Where(x => x.Scope == "Machine").Select(entry => new Dictionary<string, object>
            {
                { "id", entry.Id }, { "kind", entry.Kind }, { "present", true }, { "current", new { enabled = true } },
                { "managed", false }, { "canApply", true }, { "canRestore", false }, { "note", "diagnostic-only" },
                { "displayState", "enabled" },
                { "recommended", entry.Recommended },
            }).ToArray() },
        };
        var parsed = SystemTuning.Parse(serializer.Serialize(fixture()));
        Equal(12, parsed.Items.Count, "system tuning: all fixed settings parsed");
        foreach (var entry in SystemTuning.Catalog.Where(x => x.Scope == "Machine"))
        {
            Check(parsed.Find(entry.Id).CanApply, "system tuning: permission read for " + entry.Id);
            if (entry.Id == "policy.UpdatePause") continue;
            foreach (var verb in new[] { "Apply", "Restore" })
            {
                var args = SystemTuning.Arguments(verb, entry.Id);
                string action, item, scope; int days;
                Check(SystemTuning.TryArguments(args, out action, out item, out days, out scope) && scope == "Machine" && action == verb && item == entry.Id && days == 0,
                    "system tuning: fixed item arguments round trip");
            }
        }
        foreach (var days in new[] { 1, 7, 35 })
        {
            string action, item, scope; int parsedDays;
            Check(SystemTuning.TryArguments(SystemTuning.Arguments("PauseUpdates", null, days), out action, out item, out parsedDays, out scope) && parsedDays == days,
                "system tuning: bounded pause argument");
        }
        foreach (var bad in new[] { "service.sshd", "service.SysMain; calc", "../script.ps1", "policy.UpdatePause" })
            Throws<ArgumentException>(() => SystemTuning.Arguments("Apply", bad), "system tuning: no arbitrary item");
        foreach (var days in new[] { -1, 0, 36, int.MaxValue })
            Throws<ArgumentException>(() => SystemTuning.Arguments("PauseUpdates", null, days), "system tuning: invalid pause refused");
        Throws<ArgumentException>(() => SystemTuning.Arguments("ImportLegacy", "path"), "system tuning: maintenance import not exposed");
        foreach (var args in new[] { new[] { "--system-tuning", "Machine", "PauseUpdates", "01" }, new[] { "--system-tuning", "Machine", "RestoreAll", "extra" },
            new[] { "--system-tuning", "Machine", "Apply" }, new[] { "--system-tuning", "Machine", "PauseUpdates", "7;calc" } })
        {
            string action, item, scope; int days;
            Check(!SystemTuning.TryArguments(args, out action, out item, out days, out scope), "system tuning: noncanonical arguments refused");
        }
        var root = fixture(); root["schema"] = 2;
        Throws<FormatException>(() => SystemTuning.Parse(serializer.Serialize(root)), "system tuning: unknown schema refused");
        root = fixture(); root["scope"] = "User";
        Throws<FormatException>(() => SystemTuning.Parse(serializer.Serialize(root)), "system tuning: wrong scope reply refused");
        root = fixture(); root["action"] = "RestoreAll";
        Throws<FormatException>(() => SystemTuning.Parse(serializer.Serialize(root)), "system tuning: wrong action reply refused");
        root = fixture(); root["dryRun"] = true;
        Throws<FormatException>(() => SystemTuning.Parse(serializer.Serialize(root)), "system tuning: preview cannot count as applied");
        root = fixture(); root["items"] = new object[0];
        Throws<FormatException>(() => SystemTuning.Parse(serializer.Serialize(root)), "system tuning: partial success refused");
        root = fixture(); var rows = (Dictionary<string, object>[])root["items"]; rows[1]["id"] = rows[0]["id"];
        Throws<FormatException>(() => SystemTuning.Parse(serializer.Serialize(root)), "system tuning: duplicate item refused");
        root = fixture(); rows = (Dictionary<string, object>[])root["items"]; rows[0]["canApply"] = "true";
        Throws<FormatException>(() => SystemTuning.Parse(serializer.Serialize(root)), "system tuning: boolean types enforced");
        root = fixture(); rows = (Dictionary<string, object>[])root["items"]; rows[0]["displayState"] = "raw path or diagnostic";
        Throws<FormatException>(() => SystemTuning.Parse(serializer.Serialize(root)), "system tuning: unknown state refused");
        root = fixture(); root["ok"] = false;
        Check(SystemTuning.Parse(serializer.Serialize(root)).Items.All(x => !x.CanApply && !x.CanRestore), "system tuning: failed inventory disables changes");
        string userAction, userItem, userScope; int userDays;
        Check(SystemTuning.TryArguments(SystemTuning.Arguments("Apply", "autostart.OneDrive", 0, "User"), out userAction, out userItem, out userDays, out userScope)
            && userScope == "User", "system tuning: user startup scope preserved");
        Throws<ArgumentException>(() => SystemTuning.Arguments("Apply", "autostart.OneDrive"), "system tuning: user startup cannot target elevated machine scope");
        Throws<ArgumentException>(() => SystemTuning.Arguments("Apply", "service.SysMain", 0, "User"), "system tuning: machine setting cannot target user scope");
        Check(!SystemTuning.Catalog.Single(x => x.Id == "policy.DriverUpdates").Recommended &&
            !SystemTuning.Catalog.Single(x => x.Id == "policy.UpdatePause").Recommended, "system tuning: update policies explicit opt-ins");
        foreach (var language in Strings.Languages)
        {
            Strings.Language = language;
            foreach (var entry in SystemTuning.Catalog)
                Check(Strings.Has("system.item." + entry.Key) && Strings.Has("system.item." + entry.Key + ".description"), "system tuning: localized item " + language);
            foreach (var state in new[] { "absent", "unavailable", "enabled", "disabled", "mixed", "configured", "notConfigured" })
                Check(!SystemTuning.StateText(new SystemTuningItem { Present = true, State = state }).StartsWith("system.", StringComparison.Ordinal),
                    "system tuning: localized state " + language + " " + state);
        }
        Strings.Language = "en";
        Equal("Disabled", SystemTuning.StateText(new SystemTuningItem { Present = false, Managed = true, State = "disabled" }),
            "system tuning: intentionally removed startup entry is disabled, not missing");
        Equal(Strings.T("system.state.unavailable"), SystemTuning.StateText(new SystemTuningItem { Present = false, State = "unavailable" }),
            "system tuning: failed read is unavailable, not missing");
        var path = Environment.GetEnvironmentVariable("AMDGPU_WDDM_SYSTEM_TUNING_FIXTURES");
        Check(!string.IsNullOrEmpty(path), "system tuning: Core-emitted fake fixtures supplied by the build");
        if (!string.IsNullOrEmpty(path))
        {
            var machine = SystemTuning.Parse(File.ReadAllText(Path.Combine(path, "list-machine.json")));
            var user = SystemTuning.Parse(File.ReadAllText(Path.Combine(path, "list-user.json")), "User");
            Check(machine.Ok && machine.Items.Count == 12 && machine.Items.Count(x => x.Recommended) == 5,
                "system tuning: actual Core machine JSON parsed, five recommendations");
            Check(user.Ok && user.Items.Count == 2 && user.Items.All(x => !x.Recommended), "system tuning: actual Core user JSON parsed");
            Equal("Not configured", SystemTuning.StateText(machine.Find("policy.UpdatePause")), "system tuning: real unconfigured pause status");
        }
    }
}
