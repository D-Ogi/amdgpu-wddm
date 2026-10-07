// The planned actions of the tuning page: the GPU voltage curve and the processor settings. One place for every
// refusal, so the window and the elevated helper refuse for the same reason, and the dialog can show what is about
// to happen before anything is sent (Recovery.Plan calls Fill).
//
// Two things make these actions different from the rest of this application:
//   - Only "turn processor tuning on" writes the registry. The others send one escape, and the driver owns the trial
//     window and the revert. Nothing survives a restart until a Keep, which is also the only step that stores.
//   - Their bounds are the driver's bounds. This file refuses early for the message; the driver refuses again and is
//     the authority (docs/design/tuner.md, driver/shim/bc250_clock.c, driver/shim/include/bc250_cpu.h).
//
// Title, Change, Effect, Notes and Refusal are English: they go to the log, the dry run and the support report. What
// a person reads is Preview and PlainRefusal, which are translated (G-NOINT: no internals in either).
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;

namespace AmdgpuWddmControl
{
    // What one tuning action asks the driver for. Kind is the escape operation; the helper sends exactly this.
    public sealed class TuneRequest
    {
        public string Kind;                     // curve-trial, curve-keep, curve-stop, curve-reset,
                                                // cpu-readback, cpu-trial, cpu-keep, cpu-stop, cpu-reset, core-mask
        public uint[] Mv;                       // curve-trial: the 11 values
        public uint WindowMs;                   // curve-trial, cpu-trial: 0 = the driver's own window
        public uint? MaxMHz, UvSteps, TempC;    // cpu-trial: what it carries, each one optional
        public uint CoreMask;                   // core-mask
        public uint FanProfile;                 // fan-curve: enum bc250_fan_profile (FanState.Profile*)
        public uint[] FanC, FanPct;             // fan-curve with the custom profile: the points
        public uint FixedPct, LeaseMs, TestMs;  // fan-fixed: the duty, the lease, and how long the helper holds it
        public TuneRequest Then;                // fan-fixed: what the helper sends when the test ends
    }

    public static class TunerPlan
    {
        // The window the application asks for. Long enough that a person can read the result and press Keep with a
        // second administrator prompt in between, and well inside the driver's own 10 s to 180 s.
        public const uint DefaultWindowMs = 120000;
        public const uint MinWindowMs = 10000, MaxWindowMs = 180000;

        public static readonly string[] Actions = { "tune-trial", "tune-keep", "tune-stop", "tune-reset",
            "cpu-enable", "cpu-disable", "cpu-readback", "cpu-trial", "cpu-keep", "cpu-stop", "cpu-reset", "core-mask" };

        public static bool Owns(string action) { return Actions.Contains(action); }

        // The 11 values as the command line carries them: "820,835,...". Null when the text is not 11 numbers in the
        // admitted range, so a malformed argument is a refusal and never a partial curve.
        public static uint[] ParseCurve(string text)
        {
            if (text == null) return null;
            var parts = text.Split(',');
            if (parts.Length != Tuner.Points) return null;
            var mv = new uint[Tuner.Points];
            for (int i = 0; i < parts.Length; i++)
            {
                uint v;
                if (!uint.TryParse(parts[i], NumberStyles.None, CultureInfo.InvariantCulture, out v)) return null;
                if (v < Tuner.FloorMv || v > Tuner.CeilingMv) return null;
                mv[i] = v;
            }
            return mv;
        }

        public static string CurveText(uint[] mv)
        {
            return mv == null ? null : string.Join(",", mv.Select(v => v.ToString(CultureInfo.InvariantCulture)));
        }

        // One refusal in both languages at once: the English sentence is the return value (log, support report), the
        // translated one goes into the plan for the dialog.
        static string No(ActionPlan p, string english, string plainId, params object[] a)
        {
            p.PlainRefusal = Strings.T(plainId, a);
            return english;
        }

        static uint Seconds(uint ms) { return (ms + 999) / 1000; }

        // Fills the plan for one of these actions. The return value is the refusal, or null when the plan stands.
        public static string Fill(string action, RecoverySnapshot s, Recovery.PlanArgs more, ActionPlan p)
        {
            more = more ?? new Recovery.PlanArgs();
            p.Undoable = false;                 // a trial is undone by its own window, not by a backup
            p.Effect = "at once";
            switch (action)
            {
                case "cpu-enable":
                case "cpu-disable":
                    return Enable(action == "cpu-enable", s, p);
                case "tune-trial":
                case "tune-keep":
                case "tune-stop":
                case "tune-reset":
                    return Curve(action, s, more, p);
                default:
                    return Cpu(action, s, more, p);
            }
        }

        // The one registry switch of this page: the processor surface is off per machine until somebody turns it on,
        // and the driver reads it at the next start. Everything else on the processor card needs it on.
        static string Enable(bool on, RecoverySnapshot s, ActionPlan p)
        {
            p.Title = on ? "Turn processor tuning on" : "Turn processor tuning off";
            p.Change = on ? "the driver takes the processor surface at its next start"
                : "the driver leaves the processor alone from its next start";
            p.Effect = "at the next restart of Windows";
            p.Undoable = true;
            long now;
            bool stored = s.Parameters != null && s.Parameters.TryGetValue("CpuTune", out now) && now == 1;
            if (on == stored)
                return No(p, on ? "Processor tuning is on already." : "Processor tuning is off already.",
                    on ? "tuner.refuse.cpu-enable-already" : "tuner.refuse.cpu-disable-already");
            if (on) p.Writes.Add(RegWrite.Dword(Recovery.ParametersPath, "CpuTune", 1));
            else p.Writes.Add(RegWrite.Remove(Recovery.ParametersPath, "CpuTune"));
            p.Preview.Add(Strings.T(on ? "plan.line.cpu-tune-on" : "plan.line.cpu-tune-off"));
            if (on) p.Notes.Add("The processor numbers come from community reports and are measured by nobody on this part. "
                + "Every change is then a trial that the driver reverts by itself.");
            p.OfferRestart = true;
            return null;
        }

        static string Curve(string action, RecoverySnapshot s, Recovery.PlanArgs more, ActionPlan p)
        {
            var c = s.Curve;
            p.Title = action == "tune-trial" ? "Try a voltage curve" : action == "tune-keep" ? "Keep the voltage curve"
                : action == "tune-stop" ? "End the voltage trial" : "Back to the default voltage curve";
            if (!s.DriverRunning) return No(p, "The graphics driver is not running.", "tuner.refuse.not-running");
            if (c == null) return No(p, "The driver did not answer the curve read.", "tuner.refuse.no-read");
            if (!c.Has(CurveState.FlagValid)) return No(p, "This driver start does not carry the curve surface.", "tuner.refuse.not-valid");
            if (!c.Has(CurveState.FlagGoverning)) return No(p, "The driver does not govern the clock in this start.", "tuner.refuse.not-governing");
            var request = new TuneRequest { Kind = action == "tune-trial" ? "curve-trial" : action == "tune-keep" ? "curve-keep"
                : action == "tune-stop" ? "curve-stop" : "curve-reset" };
            switch (action)
            {
                case "tune-trial":
                    var mv = ParseCurve(more.Curve);
                    if (mv == null) return No(p, "The curve is not 11 values in the admitted range.", "tuner.refuse.bad-values");
                    int level;
                    var error = Tuner.Check(mv, c.Floor, out level);
                    if (error != CurveError.Ok)
                    {
                        uint mhz = c.FirstMHz + (uint)Math.Max(0, level) * c.StepMHz;
                        p.PlainRefusal = Tuner.ErrorText(error, mhz);
                        return "The curve breaks rule " + error + " at " + mhz + " MHz.";
                    }
                    if (!mv.Where((v, i) => v != c.Active[i]).Any())
                        return No(p, "That curve is the one in use.", "tuner.refuse.same");
                    request.Mv = mv;
                    request.WindowMs = more.Window ?? DefaultWindowMs;
                    if (request.WindowMs < MinWindowMs || request.WindowMs > MaxWindowMs)
                        return No(p, "The trial window must be " + MinWindowMs + " to " + MaxWindowMs + " ms.", "tuner.refuse.bad-window");
                    p.Change = "the driver applies the curve for " + Seconds(request.WindowMs)
                        + " s and reverts it by itself unless it is kept";
                    p.Preview.Add(Summary(mv, c.Default));
                    p.Preview.Add(Strings.T("tuner.curve.preview.revert", Seconds(request.WindowMs)));
                    p.Notes.Add("A voltage that is too low shows as a hang or a reset under load, not as a refusal. "
                        + "The driver reverts the curve when the window passes, and after a reset the stored curve comes back.");
                    p.PlainNotes.Add(Strings.T("tuner.note.curve-risk"));
                    break;
                case "tune-keep":
                    if (!c.Has(CurveState.FlagOnTrial)) return No(p, "No curve trial is running.", "tuner.refuse.no-trial");
                    // A curve the governor has not put into the chip has proved nothing, and storing it would run
                    // it at every later start. The driver refuses the same case with CurveError.Untried.
                    if (!c.Has(CurveState.FlagApplied))
                        return No(p, "The governor has not applied the candidate yet.", "tuner.refuse.curve-not-applied");
                    p.Change = "the curve on trial is stored and used at every start";
                    p.Preview.Add(Summary(c.Candidate, c.Default));
                    p.Preview.Add(Strings.T("tuner.curve.preview.keep"));
                    p.Notes.Add("The next start marks the curve pending, and the start after it confirms. "
                        + "A start that does not reach the mark falls back to the default curve.");
                    break;
                case "tune-stop":
                    if (!c.Has(CurveState.FlagOnTrial)) return No(p, "No curve trial is running.", "tuner.refuse.no-trial");
                    p.Change = "the trial ends now and the stored curve comes back";
                    p.Preview.Add(Summary(c.Stored, c.Default));
                    break;
                default:
                    if (c.Has(CurveState.FlagDefault) && !c.Has(CurveState.FlagStored) && !c.Has(CurveState.FlagOnTrial))
                        return No(p, "The default curve is in use already.", "tuner.refuse.already-default");
                    p.Change = "the stored curve is removed and the table's own line comes back";
                    p.Preview.Add(Summary(c.Default, c.Default));
                    break;
            }
            p.Tune = request;
            return null;
        }

        static string Cpu(string action, RecoverySnapshot s, Recovery.PlanArgs more, ActionPlan p)
        {
            var u = s.Cpu;
            p.Title = action == "cpu-readback" ? "Read the processor" : action == "cpu-trial" ? "Try processor settings"
                : action == "cpu-keep" ? "Keep the processor settings" : action == "cpu-stop" ? "End the processor trial"
                : action == "core-mask" ? "Change the core count" : "Back to the default processor settings";
            if (!s.DriverRunning) return No(p, "The graphics driver is not running.", "tuner.refuse.not-running");
            if (u == null) return No(p, "The driver did not answer the processor read.", "tuner.refuse.no-read");
            if (!u.Has(CpuState.FlagValid)) return No(p, "This driver start does not carry the processor surface.", "tuner.refuse.not-valid");
            if (!u.Has(CpuState.FlagTuneOn)) return No(p, "Processor tuning is off in this start.", "tuner.refuse.cpu-off");
            if (u.Has(CpuState.FlagBusy)) return No(p, "The processor surface is busy with another request.", "tuner.refuse.cpu-busy");
            var request = new TuneRequest { Kind = action == "cpu-readback" ? "cpu-readback" : action == "cpu-trial" ? "cpu-trial"
                : action == "cpu-keep" ? "cpu-keep" : action == "cpu-stop" ? "cpu-stop" : action == "core-mask" ? "core-mask" : "cpu-reset" };
            // Nothing is sent to the processor's own mailbox queue until one readback has answered on this start: the
            // queue is a community report, so the window proves it before it writes (docs/design/tuner.md).
            if (action != "cpu-readback" && !u.Has(CpuState.FlagQueue3Proven))
                return No(p, "The processor surface has not answered a readback on this start.", "tuner.refuse.cpu-not-proven");
            switch (action)
            {
                case "cpu-readback":
                    p.Change = "the driver reads the processor's voltage, clock and temperature cap";
                    p.Preview.Add(Strings.T("tuner.cpu.preview.readback"));
                    break;
                case "cpu-trial":
                    if (more.CpuClock == null && more.CpuUv == null && more.CpuTemp == null)
                        return No(p, "The request carries no processor value.", "tuner.refuse.cpu-nothing");
                    if (more.CpuClock != null && !CpuTuning.ValidClock(more.CpuClock.Value))
                        return No(p, "The processor clock must be " + CpuTuning.MinMHz + " to " + CpuTuning.MaxMHz
                            + " MHz on the 100 MHz grid.", "tuner.refuse.cpu-bad-clock", CpuTuning.MinMHz, CpuTuning.MaxMHz);
                    if (more.CpuUv != null && !CpuTuning.ValidSteps(more.CpuUv.Value))
                        return No(p, "The undervolt must be " + CpuTuning.MaxSteps + " steps or fewer.",
                            "tuner.refuse.cpu-bad-uv", CpuTuning.MaxSteps);
                    if (more.CpuTemp != null && !CpuTuning.ValidTemp(more.CpuTemp.Value))
                        return No(p, "The processor temperature cap must be " + CpuTuning.MinTempC + " to "
                            + CpuTuning.MaxTempC + " C.", "tuner.refuse.cpu-bad-temp", CpuTuning.MinTempC, CpuTuning.MaxTempC);
                    request.MaxMHz = more.CpuClock; request.UvSteps = more.CpuUv; request.TempC = more.CpuTemp;
                    request.WindowMs = more.Window ?? DefaultWindowMs;
                    if (request.WindowMs < MinWindowMs || request.WindowMs > MaxWindowMs)
                        return No(p, "The trial window must be " + MinWindowMs + " to " + MaxWindowMs + " ms.", "tuner.refuse.bad-window");
                    p.Change = "the driver applies " + Given(request) + " for " + Seconds(request.WindowMs)
                        + " s and reverts by itself unless it is kept";
                    if (more.CpuClock != null) p.Preview.Add(Strings.T("tuner.cpu.preview.clock", more.CpuClock.Value));
                    if (more.CpuUv != null) p.Preview.Add(Strings.T("tuner.cpu.preview.uv", more.CpuUv.Value));
                    if (more.CpuTemp != null) p.Preview.Add(Strings.T("tuner.cpu.preview.temp", more.CpuTemp.Value));
                    p.Preview.Add(Strings.T("tuner.curve.preview.revert", Seconds(request.WindowMs)));
                    p.Notes.Add("Every bound here is a community report, measured by nobody on this part. The driver "
                        + "reads the voltage back after each write and undoes a change that raises it over the refusal line.");
                    p.PlainNotes.Add(Strings.T("tuner.note.cpu-risk"));
                    break;
                case "cpu-keep":
                    if (!u.Has(CpuState.FlagOnTrial)) return No(p, "No processor trial is running.", "tuner.refuse.no-trial");
                    p.Change = "the processor settings on trial are stored and applied at every start";
                    p.Preview.Add(Strings.T("tuner.cpu.preview.keep"));
                    p.Notes.Add("The next start marks the settings pending, and the start after it confirms. "
                        + "A start that does not reach the mark drops them.");
                    break;
                case "cpu-stop":
                    if (!u.Has(CpuState.FlagOnTrial)) return No(p, "No processor trial is running.", "tuner.refuse.no-trial");
                    p.Change = "the trial ends now and the stored processor settings come back";
                    p.Preview.Add(Strings.T("tuner.cpu.preview.stop"));
                    break;
                case "core-mask":
                    if (more.Cores == null || !CpuTuning.ValidCores(more.Cores.Value))
                        return No(p, "The core count must be " + CpuTuning.StockCores + " or " + CpuTuning.FullCores + ".",
                            "tuner.refuse.cpu-bad-cores");
                    request.CoreMask = CpuTuning.MaskFor(more.Cores.Value);
                    if (request.CoreMask == (u.CoreMaskStored != 0 ? u.CoreMaskStored : u.CoreMask))
                        return No(p, "That core count is chosen already.", "tuner.refuse.cpu-cores-already");
                    p.Change = "the processor is asked for " + CpuTuning.CoresFor(request.CoreMask) + " cores";
                    p.Preview.Add(Strings.T(request.CoreMask == CpuTuning.MaskFull ? "graphics.cpu.cores.8" : "graphics.cpu.cores.6"));
                    p.Notes.Add("The two extra cores are untested on this board. If Windows does not reach the desktop, "
                        + "the driver puts the stock count back at the next start.");
                    if (request.CoreMask == CpuTuning.MaskFull) p.PlainNotes.Add(Strings.T("tuner.note.cores-risk"));
                    p.Effect = "at the next restart of Windows";
                    p.OfferRestart = true;
                    break;
                default:
                    if (!u.Has(CpuState.FlagStored) && !u.Has(CpuState.FlagOnTrial) && u.AppliedMaxMHz == 0 &&
                        u.AppliedUvSteps == 0 && u.AppliedTempC == 0)
                        return No(p, "The processor has its default settings already.", "tuner.refuse.cpu-already-standard");
                    p.Change = "the stored processor settings are removed and the firmware's own values come back";
                    p.Preview.Add(Strings.T("tuner.cpu.preview.reset"));
                    break;
            }
            p.Tune = request;
            return null;
        }

        // The steps that put the tuning settings of this machine back to standard, for an action that is not itself
        // a tuning action (reset-defaults). The driver stores these settings itself, so they are not in the
        // release's manifest.json and the installer's defaults say nothing about them: the only way back is the
        // same escapes the tuning page sends. The return value is the English note about what this start cannot
        // take back, or null when everything stored could be named.
        public static string StandardSteps(RecoverySnapshot s, ActionPlan p)
        {
            var left = new List<string>();
            var c = s.Curve;
            if (c != null && (c.Has(CurveState.FlagStored) || c.Has(CurveState.FlagOnTrial)))
            {
                if (c.Has(CurveState.FlagValid) && c.Has(CurveState.FlagGoverning))
                {
                    p.TuneSteps.Add(new TuneRequest { Kind = "curve-reset" });
                    p.Preview.Add(Strings.T("tuner.curve.preview.standard"));
                }
                else left.Add("the voltage curve");
            }
            var u = s.Cpu;
            bool stored = u != null && (u.Has(CpuState.FlagStored) || u.Has(CpuState.FlagOnTrial));
            bool mask = u != null && u.CoreMaskStored != 0 && u.CoreMaskStored != CpuTuning.MaskStock;
            if (stored || mask)
            {
                if (u.Has(CpuState.FlagValid) && u.Has(CpuState.FlagTuneOn) && !u.Has(CpuState.FlagBusy))
                {
                    // The driver admits no setter before a readback has answered on this start, so the reset asks
                    // for one first instead of failing on the step after it.
                    if (!u.Has(CpuState.FlagQueue3Proven)) p.TuneSteps.Add(new TuneRequest { Kind = "cpu-readback" });
                    if (stored)
                    {
                        p.TuneSteps.Add(new TuneRequest { Kind = "cpu-reset" });
                        p.Preview.Add(Strings.T("tuner.cpu.preview.reset"));
                    }
                    if (mask)
                    {
                        p.TuneSteps.Add(new TuneRequest { Kind = "core-mask", CoreMask = CpuTuning.MaskStock });
                        p.Preview.Add(Strings.T("graphics.cpu.cores.6"));
                        p.OfferRestart = true;
                    }
                }
                else left.Add("the processor settings");
            }
            // Processor tuning goes back off, which is what the release ships: the driver then leaves the processor
            // alone at its next start, whatever is still stored.
            long now;
            if (s.Parameters != null && s.Parameters.TryGetValue("CpuTune", out now) && now != 0)
            {
                p.Writes.Add(RegWrite.Remove(Recovery.ParametersPath, "CpuTune"));
                p.Preview.Add(Strings.T("plan.line.cpu-tune-off"));
            }
            if (left.Count == 0) return null;
            return "This start cannot take " + string.Join(" and ", left) + " back, so they stay stored: "
                + "restart Windows and reset again.";
        }

        // What a processor trial carries, for the log line.
        static string Given(TuneRequest r)
        {
            var v = new List<string>();
            if (r.MaxMHz != null) v.Add(r.MaxMHz.Value + " MHz");
            if (r.UvSteps != null) v.Add(r.UvSteps.Value + " undervolt steps");
            if (r.TempC != null) v.Add(r.TempC.Value + " C cap");
            return v.Count == 0 ? "nothing" : string.Join(", ", v);
        }

        // One line for the dialog: how deep the curve goes and where, in plain words.
        public static string Summary(uint[] mv, uint[] line)
        {
            if (mv == null || line == null || mv.Length != Tuner.Points || line.Length != Tuner.Points)
                return Strings.T("perf.no-reading");
            uint deepest = 0, atMHz = 0;
            int changed = 0;
            for (int i = 0; i < Tuner.Points; i++)
            {
                if (mv[i] == line[i]) continue;
                changed++;
                if (line[i] > mv[i] && line[i] - mv[i] > deepest) { deepest = line[i] - mv[i]; atMHz = Tuner.MHzAt(i); }
            }
            if (changed == 0) return Strings.T("tuner.curve.summary.line");
            return Strings.T("tuner.curve.summary", changed, deepest, atMHz);
        }

        // The command-line arguments the window gives the elevated helper, so that the helper plans the same action
        // from its own reading. Numbers only, and the curve as 11 values: nothing here is a path or a name.
        public static List<string> Arguments(Recovery.PlanArgs more)
        {
            var v = new List<string>();
            if (more == null) return v;
            if (more.Curve != null) { v.Add("--curve"); v.Add(more.Curve); }
            if (more.Window != null) { v.Add("--window"); v.Add(more.Window.Value.ToString(CultureInfo.InvariantCulture)); }
            if (more.CpuClock != null) { v.Add("--cpu-clock"); v.Add(more.CpuClock.Value.ToString(CultureInfo.InvariantCulture)); }
            if (more.CpuUv != null) { v.Add("--cpu-uv"); v.Add(more.CpuUv.Value.ToString(CultureInfo.InvariantCulture)); }
            if (more.CpuTemp != null) { v.Add("--cpu-temp"); v.Add(more.CpuTemp.Value.ToString(CultureInfo.InvariantCulture)); }
            if (more.Cores != null) { v.Add("--cores"); v.Add(more.Cores.Value.ToString(CultureInfo.InvariantCulture)); }
            return v;
        }
    }
}
