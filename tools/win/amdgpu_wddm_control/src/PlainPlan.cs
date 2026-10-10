// The confirmation dialog of a planned action in plain words (WU-053, WU-061, R1): what changes, when it applies,
// whether the screen can go dark, what to close first, whether it can be undone. Every write of the plan maps to a
// sentence (two switches of one feature share one), so the dialog lists exactly the plan's changes (G-PLAN: plan =
// preview = the helper's writes); the registry names stay in the dry run, the log and the support report.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text.RegularExpressions;

namespace AmdgpuWddmControl
{
    public sealed class PlainDialog
    {
        public string Title;
        public readonly List<string> Changes = new List<string>();
        public readonly List<string> Notes = new List<string>();
        public bool Restart;
    }

    public static class PlainPlan
    {
        // The sentence id of one write; null when the write has no plain sentence (a test keeps that impossible for
        // the app's own plans).
        public static string LineId(RegWrite w)
        {
            if (GraphicsSettings.Owns(w)) return GraphicsSettings.LineId(w);
            switch (w.Name)
            {
                case "EnableGpuPresentBlit":
                case "EnableCddDwmInterop": return w.Delete || w.Number == 1 ? "plan.line.desktop-on" : "plan.line.desktop-off";
                case "InteropClosedReason": return w.Delete ? "plan.line.clear-mark" : null;
                case "DwmForceCpu": return w.Delete || w.Number == 0 ? "plan.line.route-gpu" : "plan.line.route-cpu";
                case "DpmMode": return !w.Delete && w.Number == 1 ? "plan.line.auto-on" : "plan.line.auto-off";
                case "DpmMaxMHz": return w.Delete ? "plan.line.ceiling-default" : "plan.line.ceiling";
                case "CpuTune": return w.Delete || w.Number == 0 ? "plan.line.cpu-tune-off" : "plan.line.cpu-tune-on";
                // Full fan speed under heavy load (fan.md rule 10). On is the driver's default, so the delete form
                // is the "on" sentence here, the other way round from the switches above.
                case "FanLoadBoost": return w.Delete || w.Number == 1 ? "plan.line.fan-boost-on" : "plan.line.fan-boost-off";
                // The waiting time for the graphics (TdrSetting.cs). The app only ever writes a number; the delete
                // form is here for an undo of a backup taken before the value existed.
                case TdrSetting.ValueName: return w.Delete ? "plan.line.tdr-default" : "plan.line.tdr";
                default: return null;
            }
        }

        static string Line(RegWrite w)
        {
            if (GraphicsSettings.Owns(w)) return GraphicsSettings.PlainLine(w);
            var id = LineId(w);
            if (id == null) return Strings.T("plan.line.other");
            if (id == "plan.line.ceiling-default") return Strings.T(id, DpmSettings.DefaultMaxMHz.ToString(CultureInfo.InvariantCulture));
            return id == "plan.line.ceiling" || id == "plan.line.tdr" ? Strings.T(id, w.Number.ToString(CultureInfo.InvariantCulture)) : Strings.T(id);
        }

        public static string GroupList(string value)
        {
            var on = GameGroups.All.Where(g => !g.SupportOnly && GameGroups.State(g, value) != GroupState.Off).Select(g => g.Title).ToList();
            var sm = ShaderModelCeiling.State(value);
            if (sm != ShaderModelCeiling.Default) on.Add(Strings.T("plan.game.sm", sm));
            if (GameGroups.Hidden(value).Count > 0) on.Add(Strings.T("plan.game.other"));
            return on.Count == 0 ? Strings.T("plan.game.none") : string.Join(", ", on);
        }

        public static PlainDialog Describe(ActionPlan p)
        {
            var d = new PlainDialog { Title = Strings.Has("plan.title." + p.Action) ? Strings.T("plan.title." + p.Action) : Strings.T("plan.title.other") };
            foreach (var w in p.Writes) { var l = Line(w); if (!d.Changes.Contains(l)) d.Changes.Add(l); }
            foreach (var g in p.GameWrites)
                d.Changes.Add(g.Value.Length == 0 ? Strings.T("plan.game.remove", g.Key) : Strings.T("plan.game.set", g.Key, GroupList(g.Value)));
            if (p.Cu != null) d.Changes.AddRange(p.Cu.Preview);
            // The tuning actions write no registry value, so Preview is the only description of what they do. The
            // CU steps are already above (Recovery copies them into Preview as well), hence the duplicate check.
            foreach (var line in p.Preview) if (!d.Changes.Contains(line)) d.Changes.Add(line);
            if (p.Action == "cu-mode" && p.Cu != null && p.Cu.Target == CuMode.Full) d.Notes.Add(CuMode.EffectText());
            // WU-042: before a tuning change the person reads what it does to heat, noise and stability. Notes is
            // English for the log and the support report; PlainNotes is the same risk in the window's language.
            foreach (var n in p.PlainNotes) if (!d.Notes.Contains(n)) d.Notes.Add(n);
            if (p.CuConfirm) d.Changes.Add(Strings.T("plan.line.cu-confirm"));
            if (p.ConfirmStart) d.Changes.Add(Strings.T("plan.line.confirm-start"));

            bool atRestart = p.Effect == "at the next restart of Windows";
            bool atGame = p.Action.StartsWith("game-", StringComparison.Ordinal);
            bool atGames = p.Effect == "the next time a game starts";
            var image = p.GameImage ?? p.GameWrites.Keys.FirstOrDefault();
            d.Notes.Insert(0, atGame && image != null ? Strings.T("plan.when.game", image) : atRestart ? Strings.T("plan.when.restart")
                : atGames ? Strings.T("plan.when.games") : Strings.T("plan.when.now"));
            if (atGame && image != null) d.Notes.Add(Strings.T("plan.scope.game", image));
            if (p.Action == "reset-defaults") d.Notes.Add(Strings.T(p.GameWrites.Count > 0 ? "plan.scope.games-reset" : "plan.scope.games-kept"));
            if (p.Writes.Any(w => w.Name == "DwmForceCpu" || w.Name == "EnableGpuPresentBlit" || w.Name == "EnableCddDwmInterop"))
                d.Notes.Add(Strings.T("plan.dark-screen"));
            d.Restart = p.OfferRestart;
            if (p.OfferRestart) d.Notes.Add(Strings.T("plan.close-apps"));
            if (p.Cu != null) d.Notes.Add(Strings.T("plan.cu-no-undo"));
            d.Notes.Add(Strings.T(p.Undoable && p.Cu == null ? "plan.undo.yes" : p.Undoable ? "plan.undo.partly" : "plan.undo.no"));
            return d;
        }

        // A refusal in plain words. The plan's own sentence goes to the log and the support report.
        public static string Refusal(ActionPlan p)
        {
            if (p.PlainRefusal != null) return p.PlainRefusal;      // the tuning page refuses in both languages at once
            var r = p.Refusal ?? "";
            if (r.Contains("not installed")) return Strings.T("plan.refuse.not-installed");
            if (r.Contains("driver is not running")) return Strings.T("plan.refuse.not-running");
            if (r.Contains("stored already") || r.Contains("selected already") || r.Contains("already") || r.Contains("release defaults already")) return Strings.T("plan.refuse.already");
            if (r.StartsWith("There is no action to undo", StringComparison.Ordinal) || r.StartsWith("There is no change", StringComparison.Ordinal) || r.StartsWith("There is no undone", StringComparison.Ordinal))
                return Strings.T("plan.refuse.nothing-to-undo");
            if (r.StartsWith("No 40-core start", StringComparison.Ordinal)) return Strings.T("plan.refuse.no-cu-waiting");
            if (r.StartsWith("This start cannot be confirmed now", StringComparison.Ordinal)) return Strings.T("plan.refuse.confirm-later");
            if (p.Action == "cu-mode" && p.Cu == null && r.Length > 0 && !r.StartsWith("No graphics", StringComparison.Ordinal)) return r;    // CuMode refusals are translated already
            return Strings.T("plan.refuse.other");
        }
    }

    // G-NOINT (R1): what an ordinary user sees names no driver internals; the support view is exempt (D5). Latin terms
    // are checked in every language, so a JA/KO translation that kept them fails too.
    public static class PlainWords
    {
        public static readonly Regex[] Internals =
        {
            new Regex(@"\b(KMD|UMD|ICD|DWM|DDI|WDDM|SMU|DPM|IOCTL|HKLM|HKCU|HKEY_\w+)\b"),
            new Regex(@"(?i)\b(fence|escapes?|exit code|error code|dxgkrnl|registry)\b"),
            new Regex(@"0x[0-9A-Fa-f]+"),
            new Regex(@"\b(BC250_\w+|bc250kmd\w*|bc250control|Dpm[A-Z]\w*|Cu(Mode|Disable)\w*|Enable(Gpu|Cdd)\w*|InteropClosedReason|DwmForceCpu|Cpu(Tune|Lab|MaxMHz|UvSteps|TempC|Trial\w*|Pending|Confirmed|LastReason)|CoreMask\w*|Tdr[A-Z]\w*)\b"),
            new Regex(@"\b(FrameRateLimit|MaxFrameLatency|PerformanceOverlay|RenderOnCpu|ReportAmdDriverVersion|WsiRoute|MemoryOverflow|shader-model-\d+-off|dxgi-composition)\b"),
            new Regex(@"\b[0-9a-f]{12,}\b"),
        };

        public static IEnumerable<string> Findings(IEnumerable<string> texts)
        {
            foreach (var t in texts)
                foreach (var r in Internals)
                {
                    var m = r.Match(t);
                    if (m.Success) { yield return "\"" + m.Value + "\" in \"" + (t.Length > 80 ? t.Substring(0, 80) + "..." : t) + "\""; break; }
                }
        }
    }
}