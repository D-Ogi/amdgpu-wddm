// What the tuning cards show and which of their buttons work, as pure functions of the driver's two readings and the
// person's edits. No WinForms and no P/Invoke here: MainForm.Tuner.cs draws what these views say, and
// test/TunerViewTests.cs drives the same views through a fake driver that keeps the trial rules of
// docs/design/tuner.md, so "the button is enabled" and "the plan for that button stands" are checked as one rule.
//
// Three things the views add to the readings:
//   - How a trial ended. The driver says only that no trial runs; TrialWatch compares two readings of the same start
//     and names the end (kept, stopped, gone back by itself, or reset), so the card can say it in plain words.
//   - What is in force, what is saved for every start, and what this start recorded as standard, side by side.
//   - The processor card's states around the one setting that needs a restart (processor tuning on or off).
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;

namespace AmdgpuWddmControl
{
    // How the last trial of one surface ended, as the window saw it between two readings of the same start.
    public enum TrialEnd { None = 0, Kept = 1, Stopped = 2, Expired = 3, Reset = 4 }

    // Remembers the last reading of each surface. Observe is cheap and idempotent: a page that is built ten times
    // between two polls sees the same end ten times, and a trial that ended while another page was shown is named
    // when the tuning page is shown again.
    public sealed class TrialWatch
    {
        CurveState _curve;
        CpuState _cpu;

        public TrialEnd Curve { get; private set; }
        public TrialEnd Cpu { get; private set; }

        public void Observe(CurveState c, CpuState u)
        {
            if (c != null)
            {
                // A running trial, or another start of the driver, makes an earlier end old news.
                if (c.Has(CurveState.FlagOnTrial) || (_curve != null && _curve.Generation != c.Generation)) Curve = TrialEnd.None;
                var e = CurveEnd(_curve, c);
                if (e != TrialEnd.None) Curve = e;
                _curve = c;
            }
            if (u != null)
            {
                if (u.Has(CpuState.FlagOnTrial) || (_cpu != null && _cpu.Generation != u.Generation)) Cpu = TrialEnd.None;
                var e = CpuEnd(_cpu, u);
                if (e != TrialEnd.None) Cpu = e;
                _cpu = u;
            }
        }

        // An end the window caused itself and the driver's counters cannot name: a stop of a processor trial (the
        // driver counts a stop and a window that ran out as the same revert), and a reset outside any trial. The
        // window calls it after the helper has finished and the page has read the driver again, so it overrides what
        // Observe concluded from the counters alone.
        public void Did(bool processor, TrialEnd end)
        {
            if (processor) Cpu = end; else Curve = end;
        }

        // A new edit makes the old end irrelevant: the card then talks about the edit.
        public void Forget(bool curve, bool cpu)
        {
            if (curve) Curve = TrialEnd.None;
            if (cpu) Cpu = TrialEnd.None;
        }

        // The curve's own counters say exactly how a trial ended (driver/shim/bc250_dpm.c: keep, cancel and the
        // deadline each count once; a reset during a trial counts as a cancel and removes the stored curve).
        public static TrialEnd CurveEnd(CurveState before, CurveState now)
        {
            if (before == null || now == null || before.Generation != now.Generation) return TrialEnd.None;
            if (!before.Has(CurveState.FlagOnTrial) || now.Has(CurveState.FlagOnTrial)) return TrialEnd.None;
            if (now.Keeps > before.Keeps) return TrialEnd.Kept;
            if (now.Reverts > before.Reverts) return TrialEnd.Expired;
            if (now.Cancels > before.Cancels)
                return before.Has(CurveState.FlagStored) && !now.Has(CurveState.FlagStored) ? TrialEnd.Reset : TrialEnd.Stopped;
            return TrialEnd.None;
        }

        // The processor has one revert counter for a stop and for a window that ran out (driver/kmd/cpu.c CpuRevert),
        // no keep counter, and a reset that clears the trial without a revert. A keep stores what was applied.
        public static TrialEnd CpuEnd(CpuState before, CpuState now)
        {
            if (before == null || now == null || before.Generation != now.Generation) return TrialEnd.None;
            if (!before.Has(CpuState.FlagOnTrial) || now.Has(CpuState.FlagOnTrial)) return TrialEnd.None;
            if (now.Reverts > before.Reverts) return TrialEnd.Expired;
            if (now.Has(CpuState.FlagStored)) return TrialEnd.Kept;
            return TrialEnd.Reset;
        }
    }

    public enum TuneCard { NotInstalled = 0, NoReading = 1, NeedAuto = 2, Ready = 3, Off = 4, OnAfterRestart = 5 }

    // One row of a card: the name on the left, the value on the right, and whether the value asks for attention.
    public sealed class TuneRow
    {
        public string Key, Value;
        public bool Warn, Strong;
        public TuneRow(string key, string value, bool warn = false, bool strong = false) { Key = key; Value = value; Warn = warn; Strong = strong; }
    }

    public sealed class CurveView
    {
        public TuneCard Card;
        public string Status, Live, Saved, Waiting, GuardNote, Outcome, ErrorText;
        public bool StatusWarn, OutcomeGood, OnTrial, Changed;
        public bool ApplyEnabled, KeepEnabled, StopEnabled, ResetEnabled;
        public uint[] Shown, Line, Floor, Active;
        public CurveError Error;
        public int ErrorLevel = -1, NowIndex = -1, CeilingIndex = -1;
        public string Preset;
        public readonly List<TuneRow> Rows = new List<TuneRow>();
    }

    public sealed class CpuView
    {
        public TuneCard Card;
        public string Status, GuardNote, Outcome, RevertOwed, AfterRestart;
        public bool StatusWarn, OutcomeGood, OnTrial, Proven, Busy, Edited;
        public bool ReadEnabled, ApplyEnabled, KeepEnabled, StopEnabled, ResetEnabled, EnableOffered, DisableOffered;
        public uint Clock, Steps, Temp;                         // what the three pickers show
        public uint[] ClockChoices, StepChoices, TempChoices;   // the driver's own lists, nothing outside them
        public readonly List<TuneRow> Readings = new List<TuneRow>();
        public readonly List<TuneRow> InForce = new List<TuneRow>();
    }

    public static class TunerView
    {
        static uint[] Reference(uint[] v, uint[] fallback)
        {
            return v != null && v.Length == Tuner.Points && v[0] != 0 ? v : fallback;
        }

        // The index of a clock on the curve's grid, or -1 when the clock is under the grid or not on it.
        public static int IndexOf(uint mhz)
        {
            if (mhz < Tuner.FirstMHz || (mhz - Tuner.FirstMHz) % Tuner.StepMHz != 0) return -1;
            int i = (int)((mhz - Tuner.FirstMHz) / Tuner.StepMHz);
            return i < Tuner.Points ? i : -1;
        }

        // What the chart shows: the person's edit, else the curve on trial, else the one in force.
        public static uint[] Shown(CurveState c, uint[] edit)
        {
            if (edit != null && edit.Length == Tuner.Points) return (uint[])edit.Clone();
            if (c == null) return Tuner.Table();
            if (c.Has(CurveState.FlagOnTrial) && c.Candidate != null && c.Candidate.Length == Tuner.Points && c.Candidate[0] != 0) return (uint[])c.Candidate.Clone();
            return (uint[])Reference(c.Active, Tuner.Table()).Clone();
        }

        public static string EndText(TrialEnd end, bool processor)
        {
            switch (end)
            {
                case TrialEnd.Kept: return Strings.T("tuner.end.kept");
                case TrialEnd.Stopped: return Strings.T("tuner.end.stopped");
                case TrialEnd.Expired: return Strings.T("tuner.end.expired");
                case TrialEnd.Reset: return Strings.T(processor ? "tuner.end.reset.cpu" : "tuner.end.reset.curve");
                default: return null;
            }
        }

        public static string Temperature(int milliC)
        {
            return milliC <= 0 ? null : Strings.T("tuner.temp", (milliC + 500) / 1000);
        }

        // ---- the voltage curve -------------------------------------------------------------------------------------

        // lastReason: DpmCurveLastReason of the Parameters key (null: absent).
        public static CurveView Curve(CurveState c, uint[] edit, bool installed, uint? lastReason, TrialEnd end)
        {
            var v = new CurveView();
            if (!installed) { v.Card = TuneCard.NotInstalled; return v; }
            if (c == null || !c.Has(CurveState.FlagValid)) { v.Card = TuneCard.NoReading; return v; }
            if (!c.Has(CurveState.FlagGoverning)) { v.Card = TuneCard.NeedAuto; return v; }
            v.Card = TuneCard.Ready;
            v.Line = Reference(c.Default, Tuner.Table());
            v.Floor = Reference(c.Floor, Tuner.Floors());
            v.Active = Reference(c.Active, v.Line);
            v.Shown = Shown(c, edit);
            int level;
            v.Error = Tuner.Check(v.Shown, v.Floor, out level);
            v.ErrorLevel = level;
            if (v.Error != CurveError.Ok) v.ErrorText = Tuner.ErrorText(v.Error, Tuner.MHzAt(Math.Max(0, level)));
            v.OnTrial = c.Has(CurveState.FlagOnTrial);
            var active = v.Active;
            v.Changed = v.Shown.Where((x, i) => x != active[i]).Any();
            v.Preset = Tuner.PresetOf(v.Shown, v.Line, v.Floor);

            if (v.OnTrial)
            {
                v.Status = Strings.T("graphics.tuning.running", Tuner.Countdown(c.TrialRemainingMs));
                v.StatusWarn = true;
                if (!c.Has(CurveState.FlagApplied)) v.Waiting = Strings.T("graphics.tuning.not-applied");
            }
            else v.Status = Strings.T("tuner.curve.in-use", TunerPlan.Summary(active, v.Line));
            v.Saved = Strings.T("tuner.curve.saved", c.Has(CurveState.FlagStored) ? TunerPlan.Summary(Reference(c.Stored, v.Line), v.Line) : Strings.T("tuner.curve.summary.line"));

            // Where the chip is now: the governor's level, the curve's voltage there and the temperature it judged.
            string temp = Temperature(c.TemperatureMc);
            if (c.LevelMHz != 0)
                v.Live = temp == null ? Strings.T("tuner.curve.live", c.LevelMHz, c.LevelMv) : Strings.T("tuner.curve.live.temp", c.LevelMHz, c.LevelMv, temp);
            v.NowIndex = IndexOf(c.LevelMHz);
            v.CeilingIndex = IndexOf(c.CeilingMHz);

            // DpmCurveLastReason 2 and 3: a saved curve exists and this start runs the standard line instead.
            if (lastReason == 3) v.GuardNote = Strings.T("tuner.curve.guard.unfinished");
            else if (lastReason == 2) v.GuardNote = Strings.T("tuner.curve.guard.refused");

            if (!v.OnTrial && !v.Changed && edit == null) { v.Outcome = EndText(end, false); v.OutcomeGood = end == TrialEnd.Kept; }

            for (int i = 0; i < Tuner.Points; i++)
            {
                string value = v.Shown[i] != active[i]
                    ? Strings.T("tuner.curve.row.now", v.Shown[i], Tuner.DeltaText(v.Shown[i], v.Line[i]), active[i])
                    : Strings.T("tuner.curve.row", v.Shown[i], Tuner.DeltaText(v.Shown[i], v.Line[i]));
                v.Rows.Add(new TuneRow(Strings.T("graphics.tuning.point", Tuner.MHzAt(i)), value, v.Error != CurveError.Ok && i == v.ErrorLevel));
            }

            // The four buttons and the four plans of TunerPlan.Curve agree: a button is enabled only where its plan
            // stands (test/TunerViewTests.cs checks this over every state of the fake driver).
            v.ApplyEnabled = v.Error == CurveError.Ok && v.Changed;
            v.KeepEnabled = v.OnTrial && c.Has(CurveState.FlagApplied);
            v.StopEnabled = v.OnTrial;
            v.ResetEnabled = c.Has(CurveState.FlagStored) || v.OnTrial || !c.Has(CurveState.FlagDefault);
            return v;
        }

        // What the Apply button of the curve card asks the helper for.
        public static Recovery.PlanArgs CurveRequest(CurveView v)
        {
            return new Recovery.PlanArgs { Curve = TunerPlan.CurveText(v.Shown), Window = TunerPlan.DefaultWindowMs };
        }

        // ---- the processor -----------------------------------------------------------------------------------------

        static string Mhz(uint[] list)
        {
            if (list == null) return null;
            var on = list.Where(x => x != 0).Select(x => x.ToString(CultureInfo.CurrentCulture)).ToArray();
            return on.Length == 0 ? null : Strings.T("tuner.cpu.mhz-list", string.Join(", ", on));
        }

        static string Own(uint value, string unit)
        {
            return value == 0 ? Strings.T("tuner.cpu.own") : value.ToString(CultureInfo.CurrentCulture) + unit;
        }

        // tuneStored: CpuTune of the Parameters key (null: absent), which the driver reads at its start. lastReason:
        // CpuLastReason. The three edits are the pickers' values the person stepped to, null when untouched.
        public static CpuView Cpu(CpuState u, bool installed, uint? tuneStored, uint? lastReason, uint? clockEdit, uint? uvEdit, uint? tempEdit, TrialEnd end)
        {
            var v = new CpuView
            {
                ClockChoices = CpuTuning.ClockChoices(),
                StepChoices = Enumerable.Range(0, (int)CpuTuning.MaxSteps + 1).Select(i => (uint)i).ToArray(),
                TempChoices = Enumerable.Range((int)CpuTuning.MinTempC, (int)(CpuTuning.MaxTempC - CpuTuning.MinTempC) + 1).Select(i => (uint)i).ToArray(),
            };
            if (!installed) { v.Card = TuneCard.NotInstalled; return v; }
            if (u == null || !u.Has(CpuState.FlagValid)) { v.Card = TuneCard.NoReading; return v; }
            bool wantOn = tuneStored == 1;
            if (!u.Has(CpuState.FlagTuneOn))
            {
                // Off in this start. The setting is read at the driver's start, so "on" that is written already waits
                // for a restart: offering "turn on" again would only be refused.
                v.Card = wantOn ? TuneCard.OnAfterRestart : TuneCard.Off;
                v.Status = Strings.T(wantOn ? "tuner.cpu.on-after-restart" : "graphics.cpu.off");
                v.EnableOffered = !wantOn;
                v.DisableOffered = wantOn;
                return v;
            }
            v.Card = TuneCard.Ready;
            v.OnTrial = u.Has(CpuState.FlagOnTrial);
            v.Proven = u.Has(CpuState.FlagQueue3Proven);
            v.Busy = u.Has(CpuState.FlagBusy);
            if (!wantOn) v.AfterRestart = Strings.T("tuner.cpu.off-after-restart");
            v.EnableOffered = !wantOn;
            v.DisableOffered = wantOn;
            if (u.Has(CpuState.FlagRevertOwed)) v.RevertOwed = Strings.T("graphics.cpu.revert-owed");
            if (v.OnTrial) { v.Status = Strings.T("graphics.tuning.running", Tuner.Countdown(u.TrialRemainingMs)); v.StatusWarn = true; }
            else if (u.Has(CpuState.FlagStored)) v.Status = Strings.T("tuner.cpu.saved");
            else v.Status = Strings.T("tuner.cpu.standard");
            if (lastReason == 3) v.GuardNote = Strings.T("tuner.cpu.guard.unfinished");
            else if (lastReason == 2 || lastReason == 6 || lastReason == 7) v.GuardNote = Strings.T("tuner.cpu.guard.refused");

            // The readings: the last readback of this start. Nothing here is guessed; a value nobody read says so.
            string none = Strings.T("perf.no-reading");
            v.Readings.Add(new TuneRow(Strings.T("graphics.cpu.voltage.label"), CpuTuning.VoltageText(u.VoltageMv), u.VoltageMv >= CpuTuning.RefuseMv));
            v.Readings.Add(new TuneRow(Strings.T("tuner.cpu.temp.label"), u.Has(CpuState.FlagTempValid) ? Temperature(u.TemperatureMc) ?? none : none));
            v.Readings.Add(new TuneRow(Strings.T("graphics.cpu.cap.label"), u.CapC == 0 ? none : Strings.T("tuner.temp", u.CapC)));
            v.Readings.Add(new TuneRow(Strings.T("tuner.cpu.cores-now.label"), Mhz(u.CoreMHz) ?? none));
            v.Readings.Add(new TuneRow(Strings.T("tuner.cpu.steps-now.label"), Mhz(u.PstateMHz) ?? none));
            v.Readings.Add(new TuneRow(Strings.T("graphics.cpu.count.label"), u.Cores == 0 ? none : u.Cores + " / " + u.Threads));

            // In force, saved, and the standard this start recorded before its first change (what Reset puts back).
            v.InForce.Add(new TuneRow(Strings.T("tuner.cpu.in-use.label"), Strings.T("tuner.cpu.settings",
                Own(u.AppliedMaxMHz, " MHz"), CpuTuning.StepsText(u.AppliedUvSteps), Own(u.AppliedTempC, " °C")), false, true));
            if (u.Has(CpuState.FlagStored))
                v.InForce.Add(new TuneRow(Strings.T("tuner.cpu.saved.label"), Strings.T("tuner.cpu.settings",
                    Own(u.StoredMaxMHz, " MHz"), CpuTuning.StepsText(u.StoredUvSteps), Own(u.StoredTempC, " °C"))));
            v.InForce.Add(new TuneRow(Strings.T("tuner.cpu.baseline.label"), u.BaselineMaxMHz == 0 && u.BaselineTempC == 0
                ? Strings.T("tuner.cpu.baseline.none")
                : Strings.T("tuner.cpu.settings", Own(u.BaselineMaxMHz, " MHz"), CpuTuning.StepsText(u.BaselineUvSteps), Own(u.BaselineTempC, " °C"))));

            // The pickers start at what is in force; a value outside the driver's list (a baseline above the
            // release's highest limit) is never shown as a choice, the picker shows the nearest allowed one.
            v.Clock = clockEdit ?? Nearest(v.ClockChoices, u.AppliedMaxMHz != 0 ? u.AppliedMaxMHz : CpuTuning.MaxMHz);
            v.Steps = uvEdit ?? Nearest(v.StepChoices, u.AppliedUvSteps);
            v.Temp = tempEdit ?? Nearest(v.TempChoices, u.AppliedTempC != 0 ? u.AppliedTempC : u.CapC != 0 ? u.CapC : CpuTuning.MaxTempC);
            v.Edited = clockEdit != null || uvEdit != null || tempEdit != null;

            if (!v.OnTrial && !v.Edited) { v.Outcome = EndText(end, true); v.OutcomeGood = end == TrialEnd.Kept; }

            // Read first (docs/design/tuner.md): no change before one readback of this start has answered.
            v.ReadEnabled = !v.Busy;
            v.ApplyEnabled = v.Proven && !v.Busy && v.Edited;
            v.KeepEnabled = v.OnTrial && v.Proven && !v.Busy;
            v.StopEnabled = v.OnTrial && v.Proven && !v.Busy;
            v.ResetEnabled = v.Proven && !v.Busy && (u.Has(CpuState.FlagStored) || v.OnTrial || u.AppliedMaxMHz != 0 || u.AppliedUvSteps != 0 || u.AppliedTempC != 0);
            return v;
        }

        static uint Nearest(uint[] choices, uint value)
        {
            return choices.OrderBy(x => Math.Abs((long)x - value)).ThenBy(x => x).First();
        }

        // What the Apply button of the processor card asks the helper for: only the values the person changed.
        public static Recovery.PlanArgs CpuRequest(uint? clockEdit, uint? uvEdit, uint? tempEdit)
        {
            return new Recovery.PlanArgs { CpuClock = clockEdit, CpuUv = uvEdit, CpuTemp = tempEdit, Window = TunerPlan.DefaultWindowMs };
        }

        // The summary of the closed "Advanced tuning" card: one line per surface, so a person sees that something is
        // not standard without opening it.
        public static List<string> Summary(CurveState c, CpuState u)
        {
            var lines = new List<string>();
            if (c != null && c.Has(CurveState.FlagValid))
            {
                if (c.Has(CurveState.FlagOnTrial)) lines.Add(Strings.T("tuner.summary.curve-trial"));
                else if (c.Has(CurveState.FlagStored) && !c.Has(CurveState.FlagDefault)) lines.Add(Strings.T("tuner.summary.curve-own"));
            }
            if (u != null && u.Has(CpuState.FlagValid) && u.Has(CpuState.FlagTuneOn))
            {
                if (u.Has(CpuState.FlagOnTrial)) lines.Add(Strings.T("tuner.summary.cpu-trial"));
                else if (u.Has(CpuState.FlagStored)) lines.Add(Strings.T("tuner.summary.cpu-own"));
            }
            if (lines.Count == 0) lines.Add(Strings.T("tuner.summary.standard"));
            return lines;
        }

        // The card opens by itself while something needs the person: a trial runs, or a way back is owed.
        public static bool MustShow(CurveState c, CpuState u)
        {
            return (c != null && c.Has(CurveState.FlagOnTrial)) || (u != null && (u.Has(CpuState.FlagOnTrial) || u.Has(CpuState.FlagRevertOwed)));
        }
    }
}
