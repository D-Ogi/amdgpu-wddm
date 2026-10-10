// The case fan card of the Performance page (docs/design/fan.md Part B): the four choices, the curve rules and the
// plans of the card's two actions.
//
// The rules live twice on purpose, as on the tuning page: the driver decides (driver/shim/bc250_fan.c), and this file
// says why before the window asks. test/FanTests.cs keeps the two copies the same, and reads the presets out of the
// driver's own source, so a changed preset there fails the build here until both agree.
//
// The fan is a choice, not a trial: a curve cannot hang the machine, because the driver runs the fan at full speed
// from 87 C whatever the curve says, and at a floor of 20 % below that. So the card stores what the person picks at
// once, for every start, and needs no Keep.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;

namespace AmdgpuWddmControl
{
    public enum FanCurveError { Ok, Points, Temperature, Duty }

    public static class FanCurves
    {
        // driver/shim/include/bc250_fan.h
        public const uint MinPoints = 2, MaxPoints = 8, FloorPct = 20, FullPct = 100, MinC = 20, MaxC = 95, EmergencyC = 87;
        public const uint StepC = 5, StepPct = 5;

        // The card's choices, in the order it shows them. "board" is the BIOS fan setting; the other three are the
        // driver's presets, and "custom" is a curve the person edited.
        public static readonly string[] Choices = { "board", "standard", "quiet", "performance" };

        // driver/shim/bc250_fan.c g_fan_profiles, as (temperature C, duty %) pairs.
        static readonly uint[][] PresetPoints =
        {
            null,
            new uint[] { 40, 50, 60, 70, 70, 85, 76, 95, 80, 100 },
            new uint[] { 40, 30, 60, 45, 70, 60, 80, 80, 85, 100 },
            new uint[] { 40, 60, 55, 75, 65, 90, 75, 100 },
        };

        public static uint ProfileOf(string name)
        {
            switch (name)
            {
                case "standard": return FanState.ProfileStandard;
                case "quiet": return FanState.ProfileQuiet;
                case "performance": return FanState.ProfilePerformance;
                default: return FanState.ProfileCustom;
            }
        }

        public static string NameOf(uint profile)
        {
            switch (profile)
            {
                case FanState.ProfileStandard: return "standard";
                case FanState.ProfileQuiet: return "quiet";
                case FanState.ProfilePerformance: return "performance";
                default: return "custom";
            }
        }

        // The curve of a preset: temperatures in c, duties in pct. False for "custom" and for a name that is no preset.
        public static bool Preset(uint profile, out uint[] c, out uint[] pct)
        {
            c = null; pct = null;
            if (profile == FanState.ProfileCustom || profile >= PresetPoints.Length) return false;
            var v = PresetPoints[profile];
            c = Enumerable.Range(0, v.Length / 2).Select(i => v[2 * i]).ToArray();
            pct = Enumerable.Range(0, v.Length / 2).Select(i => v[2 * i + 1]).ToArray();
            return true;
        }

        // The driver's rule, in its order (bc250_fan_curve_check): the count, then point by point the temperature range,
        // the duty range, a temperature that rises and a duty that does not fall. point is the 0-based index, -1 if none.
        public static FanCurveError Check(uint[] c, uint[] pct, out int point)
        {
            point = -1;
            if (c == null || pct == null || c.Length != pct.Length || c.Length < MinPoints || c.Length > MaxPoints) return FanCurveError.Points;
            for (int i = 0; i < c.Length; i++)
            {
                point = i;
                if (c[i] < MinC || c[i] > MaxC) return FanCurveError.Temperature;
                if (pct[i] < FloorPct || pct[i] > FullPct) return FanCurveError.Duty;
                if (i > 0 && c[i] <= c[i - 1]) return FanCurveError.Temperature;
                if (i > 0 && pct[i] < pct[i - 1]) return FanCurveError.Duty;
            }
            point = -1;
            return FanCurveError.Ok;
        }

        public static string ErrorText(FanCurveError e, int point)
        {
            switch (e)
            {
                case FanCurveError.Ok: return "";
                case FanCurveError.Points: return Strings.T("perf.fan.error.points", MinPoints, MaxPoints);
                case FanCurveError.Temperature: return Strings.T("perf.fan.error.temperature", point + 1, MinC, MaxC);
                default: return Strings.T("perf.fan.error.duty", point + 1, FloorPct, FullPct);
            }
        }

        // "40:50,60:70,...": the command line form, temperatures and duties in pairs. Null for a malformed text, so a
        // bad argument is a refusal and never a partial curve.
        public static string CurveText(uint[] c, uint[] pct)
        {
            if (c == null || pct == null || c.Length != pct.Length) return null;
            return string.Join(",", c.Select((t, i) => t.ToString(CultureInfo.InvariantCulture) + ":" + pct[i].ToString(CultureInfo.InvariantCulture)));
        }

        public static bool ParseCurve(string text, out uint[] c, out uint[] pct)
        {
            c = null; pct = null;
            if (string.IsNullOrEmpty(text)) return false;
            var parts = text.Split(',');
            if (parts.Length < MinPoints || parts.Length > MaxPoints) return false;
            var tc = new uint[parts.Length];
            var tp = new uint[parts.Length];
            for (int i = 0; i < parts.Length; i++)
            {
                var pair = parts[i].Split(':');
                if (pair.Length != 2 ||
                    !uint.TryParse(pair[0], NumberStyles.None, CultureInfo.InvariantCulture, out tc[i]) ||
                    !uint.TryParse(pair[1], NumberStyles.None, CultureInfo.InvariantCulture, out tp[i]) ||
                    tc[i] > 999 || tp[i] > 999)
                    return false;
            }
            c = tc; pct = tp;
            return true;
        }

        // The curve in plain words for a dialog and the card: "40 °C 50 %, 60 °C 70 %, ...".
        public static string Describe(uint[] c, uint[] pct)
        {
            if (c == null || pct == null || c.Length != pct.Length || c.Length == 0) return Strings.T("perf.no-reading");
            return string.Join(", ", c.Select((t, i) => Strings.T("perf.fan.point.value", t, pct[i])));
        }

        // Moves point i of a curve to (temperature, duty) and keeps the curve legal while it moves: the temperature
        // stays between its neighbours, one degree apart, the duty between theirs, both inside the driver's ranges. The
        // chart's drag and keys go through here, so a curve edited on the chart never needs a refusal.
        public static void Move(uint[] c, uint[] pct, int i, int temperature, int duty)
        {
            if (c == null || pct == null || i < 0 || i >= c.Length || c.Length != pct.Length) return;
            int lowC = i > 0 ? (int)c[i - 1] + 1 : (int)MinC, highC = i < c.Length - 1 ? (int)c[i + 1] - 1 : (int)MaxC;
            int lowP = i > 0 ? (int)pct[i - 1] : (int)FloorPct, highP = i < c.Length - 1 ? (int)pct[i + 1] : (int)FullPct;
            lowC = Math.Max(lowC, (int)MinC); highC = Math.Min(highC, (int)MaxC);
            lowP = Math.Max(lowP, (int)FloorPct); highP = Math.Min(highP, (int)FullPct);
            if (lowC <= highC) c[i] = (uint)Math.Max(lowC, Math.Min(highC, temperature));
            if (lowP <= highP) pct[i] = (uint)Math.Max(lowP, Math.Min(highP, duty));
        }

        // The duty a curve asks for at a temperature, as the driver computes it: the first duty below the first point,
        // the last above the last, the straight line between two points rounded up to a whole percent.
        public static uint DutyAt(uint[] c, uint[] pct, double temperature)
        {
            if (c == null || pct == null || c.Length == 0 || c.Length != pct.Length) return 0;
            if (temperature <= c[0]) return pct[0];
            for (int i = 1; i < c.Length; i++)
                if (temperature <= c[i])
                    return (uint)Math.Ceiling(pct[i - 1] + (pct[i] - (double)pct[i - 1]) * (temperature - c[i - 1]) / (c[i] - c[i - 1]) - 1e-9);
            return pct[c.Length - 1];
        }

        // Whether the card's choice and curve differ from what the driver runs and stores, which is when Apply has
        // something to do.
        public static bool Changed(FanState f, string choice, uint[] c, uint[] pct)
        {
            if (f == null) return false;
            bool sameCurve = CurveText(c, pct) == CurveText(ShownC(f), ShownPct(f));
            return choice != StoredChoice(f) || (choice == "custom" && !sameCurve) ||
                (choice != "board" && f.Mode != FanState.ModeCurve) || (choice == "board" && f.Mode != FanState.ModeBoard);
        }

        // The test of the card: one duty for TestMs under a lease of TestLeaseMs, then the choice in force again. The
        // lease outlives the test by 5 s, so a helper that dies in the middle leaves the fan with the board, never stuck.
        public const uint TestMs = 10000, TestLeaseMs = 15000;
        public static readonly uint[] TestChoices = { 30, 40, 50, 60, 70, 80, 90, 100 };

        // Whether a short test may run now: the driver runs the fan, and nothing more urgent has it.
        public static bool TestAllowed(FanState f)
        {
            return f != null && f.Has(FanState.FlagEnabled) && !f.Has(FanState.FlagLeased) && !f.Has(FanState.FlagPaused) &&
                !f.Has(FanState.FlagFault) && !f.Has(FanState.FlagEmergency) && !f.Has(FanState.FlagHeldBack) &&
                f.State != FanState.StateEmergency && f.State != FanState.StateDoubt && f.State != FanState.StateFault;
        }

        // The curve in force, as the driver reports it.
        public static uint[] ShownC(FanState f) { return f == null ? null : f.CurveC.Take((int)Math.Min(f.Points, MaxPoints)).ToArray(); }
        public static uint[] ShownPct(FanState f) { return f == null ? null : f.CurvePct.Take((int)Math.Min(f.Points, MaxPoints)).ToArray(); }

        // Full speed under a sustained heavy load (docs/design/fan.md rule 10, the driver's FanLoadBoost). The
        // driver reads the value at its start and treats anything but 1 as off; nothing stored means on, so a
        // machine that never touched the switch has nothing in the registry. The argument is the registry value.
        public static bool BoostOn(uint? stored) { return stored == null || stored.Value == 1; }

        // Which of the four choices (or "custom") the driver's stored choice is. A start without a stored choice runs
        // the standard curve, so that is what "nothing stored" means here as well.
        public static string StoredChoice(FanState f)
        {
            if (f == null || !f.Has(FanState.FlagStored)) return "standard";
            if (f.StoredMode == FanState.ModeBoard) return "board";
            return NameOf(f.StoredProfile);
        }

        // Who runs the fan now, in plain words.
        public static string StateText(FanState f)
        {
            if (f == null) return Strings.T("perf.no-reading");
            if (!f.Has(FanState.FlagEnabled)) return Strings.T("perf.fan.state.board");
            if (f.Has(FanState.FlagPaused)) return Strings.T("perf.fan.state.paused");
            switch (f.State)
            {
                case FanState.StateCurve: return Strings.T("perf.fan.state.curve");
                case FanState.StateFixed: return Strings.T("perf.fan.state.fixed");
                case FanState.StateEmergency: return Strings.T("perf.fan.state.emergency");
                case FanState.StateDoubt: return Strings.T("perf.fan.state.doubt");
                case FanState.StateFault: return Strings.T("perf.fan.state.fault");
                default:
                    // The board runs the fan. Say why when the driver gave it back by itself and will take it again.
                    return Strings.T(f.Has(FanState.FlagHeldBack) ? "perf.fan.state.held-back" : "perf.fan.state.board");
            }
        }

        // Why the driver does not run the fan in this start, or null when it may.
        public static string GateText(FanState f)
        {
            if (f == null || f.Has(FanState.FlagEnabled)) return null;
            switch (f.Gate)
            {
                case FanState.GateSetting: return Strings.T("perf.fan.gate.setting");
                case FanState.GateChip: return Strings.T("perf.fan.gate.chip");
                default: return Strings.T("perf.fan.gate.reader");
            }
        }

        // One support-report line, English like the rest of the report.
        public static string ReportLine(FanState f)
        {
            if (f == null) return "fan control: no reading";
            return "fan control: state " + f.State + ", mode " + f.Mode + ", profile " + f.Profile + ", flags " + f.Flags +
                ", gate " + f.Gate + ", target " + f.TargetPct + " % applied " + f.AppliedPct + " % raw " + f.WrittenRaw +
                " readback " + f.ReadbackRaw + ", " + f.Rpm + " rpm, guard " + (f.GuardMc / 1000.0).ToString("0.0", CultureInfo.InvariantCulture) +
                " C, lease " + f.LeaseMs + " ms, reason " + f.Reason + " doubt " + f.DoubtReason +
                ", stored " + (f.Has(FanState.FlagStored) ? "mode " + f.StoredMode + " profile " + f.StoredProfile : "none") +
                ", curve " + (CurveText(ShownC(f), ShownPct(f)) ?? "none") +
                ", saved mode " + f.SavedMode + " target " + f.SavedTarget +
                ", takeovers " + f.Takeovers + " handbacks " + f.Handbacks + " writes " + f.Writes + " failures " + f.Failures +
                " emergencies " + f.Emergencies + " doubts " + f.Doubts + " lease expiries " + f.LeaseExpiries + " watchdog " + f.WatchdogFires;
        }
    }

    public static class FanPlan
    {
        public static readonly string[] Actions = { "fan-auto", "fan-curve", "fan-test", "fan-boost-on", "fan-boost-off" };

        public static bool Owns(string action) { return Actions.Contains(action); }

        static string No(ActionPlan p, string english, string plainId, params object[] a)
        {
            p.PlainRefusal = Strings.T(plainId, a);
            return english;
        }

        // Fills the plan for one of the two actions. The return value is the refusal, or null when the plan stands.
        public static string Fill(string action, RecoverySnapshot s, Recovery.PlanArgs more, ActionPlan p)
        {
            more = more ?? new Recovery.PlanArgs();
            var f = s.Fan;
            p.Undoable = false;                 // the card itself is the way back: pick the other choice
            p.Effect = "at once";
            // The one switch of this card that is a registry value and not an escape: it needs no running driver
            // and no fan reading, because the driver reads it when it starts.
            if (action == "fan-boost-on" || action == "fan-boost-off") return Boost(action == "fan-boost-on", s, p);
            p.Title = action == "fan-auto" ? "Let the board run the fan" : "Let the driver run the fan by a curve";
            if (!s.DriverRunning) return No(p, "The graphics driver is not running.", "tuner.refuse.not-running");
            if (f == null) return No(p, "The driver did not answer the fan read.", "perf.fan.refuse.no-read");
            if (!f.Has(FanState.FlagEnabled)) return No(p, "This start does not run the fan control (gate " + f.Gate + ").", "perf.fan.refuse.off");
            if (action == "fan-test") return Test(f, more, p);
            bool stored = f.Has(FanState.FlagStored);
            var request = new TuneRequest { Kind = action == "fan-auto" ? "fan-board" : "fan-curve" };
            if (action == "fan-auto")
            {
                if (stored && f.StoredMode == FanState.ModeBoard && f.Mode == FanState.ModeBoard)
                    return No(p, "The board runs the fan already.", "perf.fan.refuse.same");
                p.Change = "the board's own fan setting runs the fan, now and at every start";
                p.Preview.Add(Strings.T("plan.line.fan-board"));
                p.Tune = request;
                return null;
            }
            string name = more.FanProfile ?? "standard";
            uint profile = FanCurves.ProfileOf(name);
            if (profile == FanState.ProfileCustom && name != "custom")
                return No(p, "\"" + name + "\" is not a fan curve.", "perf.fan.refuse.bad-curve");
            uint[] c, pct;
            if (profile == FanState.ProfileCustom)
            {
                if (!FanCurves.ParseCurve(more.FanCurve, out c, out pct))
                    return No(p, "The fan curve is not 2 to 8 temperature and speed pairs.", "perf.fan.refuse.bad-curve");
                int point;
                var error = FanCurves.Check(c, pct, out point);
                if (error != FanCurveError.Ok)
                {
                    p.PlainRefusal = FanCurves.ErrorText(error, point);
                    return "The fan curve breaks rule " + error + " at point " + (point + 1) + ".";
                }
            }
            else
            {
                if (more.FanCurve != null) return No(p, "A preset carries no curve of its own.", "perf.fan.refuse.bad-curve");
                FanCurves.Preset(profile, out c, out pct);
            }
            // The same choice in force and stored: nothing to do. A custom curve is the same only point for point.
            bool sameCurve = FanCurves.CurveText(FanCurves.ShownC(f), FanCurves.ShownPct(f)) == FanCurves.CurveText(c, pct);
            if (stored && f.StoredMode == FanState.ModeCurve && f.StoredProfile == profile && f.Mode == FanState.ModeCurve &&
                f.Profile == profile && sameCurve && !f.Has(FanState.FlagLeased))
                return No(p, "That fan curve is in use already.", "perf.fan.refuse.same");
            request.FanProfile = profile;
            if (profile == FanState.ProfileCustom) { request.FanC = c; request.FanPct = pct; }
            p.Change = "the driver runs the fan by the " + name + " curve (" + FanCurves.CurveText(c, pct) + "), now and at every start";
            p.Preview.Add(Strings.T("plan.line.fan-curve", Strings.T("perf.fan.choice." + name)));
            p.Preview.Add(Strings.T("plan.line.fan-points", FanCurves.Describe(c, pct)));
            p.Notes.Add("From 87 C the driver runs the fan at full speed whatever the curve says.");
            p.PlainNotes.Add(Strings.T("perf.fan.note"));
            p.Tune = request;
            return null;
        }

        // Full speed under a sustained heavy load: the one registry switch of this card. The driver reads
        // FanLoadBoost at its start, so the change applies at the next restart of Windows; on is the default, so
        // "on" removes the value and "off" writes 0.
        static string Boost(bool on, RecoverySnapshot s, ActionPlan p)
        {
            p.Title = on ? "Turn full fan speed under heavy load on" : "Turn full fan speed under heavy load off";
            p.Change = on ? "the driver runs the fan at full speed under a sustained heavy load, from its next start"
                : "the driver leaves the fan to the curve under load, from its next start";
            p.Effect = "at the next restart of Windows";
            p.Undoable = true;
            if (on == FanCurves.BoostOn(s.P("FanLoadBoost")))
                return No(p, on ? "The fan runs at full speed under a heavy load already."
                    : "The fan is left to the curve under a heavy load already.",
                    on ? "perf.fan.refuse.boost-on-already" : "perf.fan.refuse.boost-off-already");
            if (on) p.Writes.Add(RegWrite.Remove(Recovery.ParametersPath, "FanLoadBoost"));
            else p.Writes.Add(RegWrite.Dword(Recovery.ParametersPath, "FanLoadBoost", 0));
            p.Preview.Add(Strings.T(on ? "plan.line.fan-boost-on" : "plan.line.fan-boost-off"));
            p.OfferRestart = true;
            return null;
        }

        // A short test: one duty under a lease, then the choice in force again (docs/design/fan.md Part B, rule 9: a
        // lease that runs out gives the fan to the board, so the helper sends the choice back itself at the end).
        static string Test(FanState f, Recovery.PlanArgs more, ActionPlan p)
        {
            p.Title = "Test the fan at one speed";
            uint pct = more.FanTestPct ?? 0;
            if (!FanCurves.TestChoices.Contains(pct))
                return No(p, "The test speed must be one of " + string.Join(", ", FanCurves.TestChoices) + " %.", "perf.fan.refuse.bad-test");
            if (!FanCurves.TestAllowed(f))
                return No(p, "The fan is not free for a test now (state " + f.State + ", flags " + f.Flags + ").", "perf.fan.refuse.busy");
            var then = f.Mode == FanState.ModeCurve
                ? new TuneRequest { Kind = "fan-curve", FanProfile = f.Profile }
                : new TuneRequest { Kind = "fan-board" };
            if (then.Kind == "fan-curve" && f.Profile == FanState.ProfileCustom) { then.FanC = FanCurves.ShownC(f); then.FanPct = FanCurves.ShownPct(f); }
            p.Tune = new TuneRequest { Kind = "fan-fixed", FixedPct = pct, LeaseMs = FanCurves.TestLeaseMs, TestMs = FanCurves.TestMs, Then = then };
            p.Change = "the fan runs at " + pct + " % for " + FanCurves.TestMs / 1000 + " s, then " +
                (then.Kind == "fan-board" ? "the board runs it again" : "the curve in force runs it again") + "; nothing is stored";
            p.Preview.Add(Strings.T("plan.line.fan-test", pct, FanCurves.TestMs / 1000));
            p.Notes.Add("The lease is " + FanCurves.TestLeaseMs + " ms: if the test stops halfway, the board runs the fan when it ends.");
            return null;
        }

        // The step that puts the fan back to its standard choice for reset-defaults: the driver's standard curve, which
        // is also what a start with nothing stored runs. Null when nothing needs to change or the step was added; the
        // English note when this start cannot take the stored choice back.
        public static string StandardSteps(RecoverySnapshot s, ActionPlan p)
        {
            var f = s.Fan;
            if (f == null || !f.Has(FanState.FlagStored)) return null;
            if (f.StoredMode == FanState.ModeCurve && f.StoredProfile == FanState.ProfileStandard) return null;
            if (!f.Has(FanState.FlagEnabled)) return "This start cannot take the stored fan choice back: restart Windows and reset again.";
            p.TuneSteps.Add(new TuneRequest { Kind = "fan-curve", FanProfile = FanState.ProfileStandard });
            p.Preview.Add(Strings.T("plan.line.fan-curve", Strings.T("perf.fan.choice.standard")));
            return null;
        }

        public static List<string> Arguments(Recovery.PlanArgs more)
        {
            var v = new List<string>();
            if (more == null) return v;
            if (more.FanProfile != null) { v.Add("--fan-profile"); v.Add(more.FanProfile); }
            if (more.FanCurve != null) { v.Add("--fan-curve"); v.Add(more.FanCurve); }
            if (more.FanTestPct != null) { v.Add("--fan-test-pct"); v.Add(more.FanTestPct.Value.ToString(CultureInfo.InvariantCulture)); }
            return v;
        }

        public static bool ValidProfileName(string name)
        {
            return name == "custom" || FanCurves.Choices.Skip(1).Contains(name);
        }
    }
}
