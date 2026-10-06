// The tuning cards of the Graphics page: the voltage curve, the processor, and the number of processor cores. The
// window edits numbers and asks; the driver owns the trial window, the revert and every bound (docs/design/tuner.md).
//
// Three rules shape this page:
//   - Nothing is saved by trying it. Every change runs as a trial with a countdown the driver keeps, so a setting
//     that hangs the machine is gone at the next start even if the application never runs again.
//   - The reference lines come from the driver (Default and Floor of its reading), not from the table in Tuner.cs:
//     a driver with other limits moves the chart, and the window does not argue with it.
//   - The processor numbers are community reports. The card says so, and nothing is sent to the processor's own
//     mailbox queue until one readback has answered on this start.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;
using System.Linq;
using System.Windows.Forms;

namespace AmdgpuWddmControl
{
    public sealed partial class MainForm
    {
        uint[] _curveEdit;              // null: follow the curve the driver has
        int _curveLevel;
        uint? _cpuClockEdit, _cpuUvEdit, _cpuTempEdit, _coresEdit;
        string _tuneState;              // the trial signature of the last build, so a trial that ended rebuilds the page

        CurveState CurveNow { get { return _snap != null ? _snap.Curve : null; } }
        CpuState CpuNow { get { return _snap != null ? _snap.Cpu : null; } }

        // What the chart shows: the person's edit, else the curve on trial, else the one in force.
        uint[] CurveShown()
        {
            var c = CurveNow;
            if (_curveEdit != null) return _curveEdit;
            if (c == null) return Tuner.Table();
            if (c.Has(CurveState.FlagOnTrial) && c.Candidate != null && c.Candidate.Length == Tuner.Points && c.Candidate[0] != 0) return (uint[])c.Candidate.Clone();
            return c.Active != null && c.Active.Length == Tuner.Points && c.Active[0] != 0 ? (uint[])c.Active.Clone() : Tuner.Table();
        }

        static uint[] Reference(uint[] v, uint[] fallback)
        {
            return v != null && v.Length == Tuner.Points && v[0] != 0 ? v : fallback;
        }

        void AddTuning(Control page, int width, bool installed)
        {
            page.Controls.Add(BuildCurveCard(width, installed));
            page.Controls.Add(BuildCpuCard(width, installed));
            page.Controls.Add(BuildCoresCard(width, installed));
            var c = CurveNow; var u = CpuNow;
            _tuneState = (c == null ? "-" : c.Flags + ":" + c.Serial) + "/" + (u == null ? "-" : u.Flags + ":" + u.Serial);
        }

        CardPanel BuildCurveCard(int width, bool installed)
        {
            var card = new CardPanel(Strings.T("graphics.tuning.title"), width);
            Mark("graphics.tuning", card);
            var c = CurveNow;
            card.Add(Ui.Dim(Strings.T("graphics.tuning.intro"), card.Inner));
            if (!installed || c == null || !c.Has(CurveState.FlagValid))
            {
                card.Add(Ui.Dim(Strings.T(installed ? "perf.no-reading" : "graphics.not-installed"), card.Inner));
                return card;
            }
            if (!c.Has(CurveState.FlagGoverning))
            {
                card.Add(Ui.Label(Strings.T("graphics.tuning.need-auto"), null, Theme.Warn, card.Inner));
                return card;
            }
            var line = Reference(c.Default, Tuner.Table());
            var floor = Reference(c.Floor, Tuner.Floors());
            var shown = CurveShown();
            int level;
            var error = Tuner.Check(shown, floor, out level);
            bool onTrial = c.Has(CurveState.FlagOnTrial);
            var active = Reference(c.Active, Tuner.Table());
            bool changed = shown.Where((v, i) => v != active[i]).Any();

            // What is in force, and whether it outlives this start.
            card.Add(Ui.Label(onTrial ? Strings.T("graphics.tuning.running", Tuner.Countdown(c.TrialRemainingMs))
                : c.Has(CurveState.FlagStored) ? Strings.T("graphics.tuning.saved") : TunerPlan.Summary(Reference(c.Active, line), line),
                Theme.Bold, onTrial ? Theme.Warn : (Color?)null, card.Inner));
            if (onTrial)
            {
                var left = Ui.Label(Strings.T("graphics.tuning.unsaved"), null, Theme.Warn, card.Inner);
                _live["tuner.curve.countdown"] = left;
                card.Add(left);
                if (!c.Has(CurveState.FlagApplied))
                    card.Add(Ui.Label(Strings.T("graphics.tuning.not-applied"), null, Theme.Dim, card.Inner));
            }

            // The presets: four legal curves by construction, so a person never has to read a chart to use this card.
            var presets = Ui.WrapRow(card.Inner);
            string chosen = Tuner.PresetOf(shown, line, floor);
            foreach (var name in Tuner.PresetNames)
            {
                string id = name;
                var r = Ui.Radio(Strings.T("graphics.tuning.preset." + id));
                r.Checked = chosen == id;
                r.CheckedChanged += (s, e) => { if (r.Checked && Tuner.PresetOf(CurveShown(), line, floor) != id) { _curveEdit = Tuner.Preset(id, line, floor); ShowPage(_page, null, false); } };
                presets.Controls.Add(r);
            }
            presets.Controls.Add(Explain("tuning", Strings.T("graphics.tuning.title")));
            card.Add(Ui.Label(Strings.T("graphics.tuning.preset"), Theme.Bold, null, card.Inner));
            card.Add(presets);

            // The chart, the two buttons that move the selected knot, and the table under it (G-A11Y: the table alone
            // is enough to read every value).
            var chart = new CurveChart(card.Inner, Theme.S(170));
            chart.SetReference(line, floor);
            chart.Values = shown;
            chart.Selected = _curveLevel;
            chart.Changed += (s, e) => { _curveEdit = chart.Values; _curveLevel = chart.Selected; ShowPage(_page, null, false); };
            chart.Picked += (s, e) => { _curveLevel = chart.Selected; ShowPage(_page, null, false); };
            Mark("graphics.tuning.chart", chart);
            card.Add(Ui.Label(Strings.T("graphics.tuning.chart"), Theme.Bold, null, card.Inner));
            card.Add(chart);
            card.Add(Ui.Dim(Strings.T("graphics.tuning.chart.help"), card.Inner));
            var nudge = Ui.WrapRow(card.Inner,
                Ui.Button(Strings.T("graphics.tuning.lower"), (s, e) => Nudge(-5)),
                Ui.Button(Strings.T("graphics.tuning.raise"), (s, e) => Nudge(5)));
            card.Add(nudge);

            card.Add(Ui.Label(Strings.T("graphics.tuning.table"), Theme.Bold, null, card.Inner));
            for (int i = 0; i < Tuner.Points; i++)
                card.Pair(Strings.T("graphics.tuning.point", Tuner.MHzAt(i)), shown[i] + " mV, " + Tuner.DeltaText(shown[i], line[i]),
                    i == _curveLevel ? Theme.Text : Theme.Dim);
            if (error != CurveError.Ok)
                card.Add(Ui.Label(Tuner.ErrorText(error, Tuner.MHzAt(Math.Max(0, level))), Theme.Bold, Theme.Warn, card.Inner));

            var row = Ui.WrapRow(card.Inner);
            var apply = Ui.Button(Strings.T("graphics.tuning.apply"), (s, e) => RunAction("tune-trial",
                new Recovery.PlanArgs { Curve = TunerPlan.CurveText(CurveShown()), Window = TunerPlan.DefaultWindowMs }), true);
            apply.Enabled = error == CurveError.Ok && changed;
            row.Controls.Add(apply);
            var keep = Ui.Button(Strings.T("graphics.tuning.keep"), (s, e) => RunAction("tune-keep", null, null, null, false, ok => ClearCurveEdit()));
            // Keeping a curve nobody has run is how a bad curve reaches every later start: the driver stores what
            // the governor applied, so the button waits for the governor to apply it (it does so on its next tick).
            keep.Enabled = onTrial && c.Has(CurveState.FlagApplied);
            row.Controls.Add(keep);
            var stop = Ui.Button(Strings.T("graphics.tuning.stop"), (s, e) => RunAction("tune-stop", null, null, null, false, ok => ClearCurveEdit()));
            stop.Enabled = onTrial;
            row.Controls.Add(stop);
            var reset = Ui.Button(Strings.T("graphics.tuning.reset"), (s, e) => RunAction("tune-reset", null, null, null, false, ok => ClearCurveEdit()));
            reset.Enabled = c.Has(CurveState.FlagStored) || onTrial;
            row.Controls.Add(reset);
            card.Add(row);
            var result = ResultLine(card.Inner);
            if (result != null) card.Add(result);
            return card;
        }

        void ClearCurveEdit()
        {
            _curveEdit = null;
            ShowPage(_page, null, false);
        }

        void Nudge(int delta)
        {
            var c = CurveNow;
            if (c == null) return;
            var line = Reference(c.Default, Tuner.Table());
            var floor = Reference(c.Floor, Tuner.Floors());
            var mv = CurveShown();
            mv[_curveLevel] = Tuner.Nudge(mv[_curveLevel], delta, _curveLevel, line, floor);
            _curveEdit = mv;
            ShowPage(_page, null, false);
        }

        CardPanel BuildCpuCard(int width, bool installed)
        {
            var card = new CardPanel(Strings.T("graphics.cpu.title"), width);
            Mark("graphics.cpu-tuning", card);
            var u = CpuNow;
            card.Add(Ui.Dim(Strings.T("graphics.cpu.intro"), card.Inner));
            if (!installed || u == null || !u.Has(CpuState.FlagValid))
            {
                card.Add(Ui.Dim(Strings.T(installed ? "perf.no-reading" : "graphics.not-installed"), card.Inner));
                return card;
            }
            if (!u.Has(CpuState.FlagTuneOn))
            {
                card.Add(Ui.Label(Strings.T("graphics.cpu.off"), null, Theme.Dim, card.Inner));
                card.Add(Ui.WrapRow(card.Inner,
                    Ui.Button(Strings.T("graphics.cpu.enable"), (s, e) => RunAction("cpu-enable")),
                    Explain("cpu-tuning", Strings.T("graphics.cpu.title"))));
                return card;
            }
            bool onTrial = u.Has(CpuState.FlagOnTrial);
            // The driver could not put the settings back and keeps trying. The person has to know, because the
            // processor is running a setting nobody chose to keep (0.7.211).
            if (u.Has(CpuState.FlagRevertOwed))
                card.Add(Ui.Label(Strings.T("graphics.cpu.revert-owed"), Theme.Bold, Theme.Warn, card.Inner));
            card.Pair(Strings.T("graphics.cpu.voltage.label"), CpuTuning.VoltageText(u.VoltageMv), u.VoltageMv >= CpuTuning.RefuseMv ? Theme.Warn : (Color?)null);
            card.Pair(Strings.T("graphics.cpu.count.label"), u.Cores == 0 ? Strings.T("perf.no-reading") : u.Cores + " / " + u.Threads);
            card.Pair(Strings.T("graphics.cpu.cap.label"), u.CapC == 0 ? Strings.T("perf.no-reading") : u.CapC + " C");
            if (onTrial)
            {
                var left = Ui.Label(Strings.T("graphics.tuning.running", Tuner.Countdown(u.TrialRemainingMs)), Theme.Bold, Theme.Warn, card.Inner);
                _live["tuner.cpu.countdown"] = left;
                card.Add(left);
                card.Add(Ui.Label(Strings.T("graphics.tuning.unsaved"), null, Theme.Warn, card.Inner));
            }
            else if (u.Has(CpuState.FlagStored)) card.Add(Ui.Label(Strings.T("graphics.tuning.saved"), Theme.Bold, null, card.Inner));

            var read = Ui.WrapRow(card.Inner,
                Ui.Button(Strings.T("graphics.cpu.readback"), (s, e) => RunAction("cpu-readback")),
                Explain("cpu-tuning", Strings.T("graphics.cpu.title")));
            card.Add(read);
            bool proven = u.Has(CpuState.FlagQueue3Proven);
            if (!proven) card.Add(Ui.Label(Strings.T("graphics.cpu.need-readback"), null, Theme.Warn, card.Inner));

            // The three numbers, each inside the driver's own list, so a step can never leave the admitted band.
            var clock = new ValuePicker(CpuTuning.ClockChoices(), " MHz", Strings.T("graphics.cpu.clock.label"));
            clock.Value = _cpuClockEdit ?? (u.AppliedMaxMHz != 0 ? u.AppliedMaxMHz : CpuTuning.MaxMHz);
            clock.Stepped += (s, e) => _cpuClockEdit = clock.Value;
            card.Add(Ui.WrapRow(card.Inner, Ui.Label(Strings.T("graphics.cpu.clock.label"), null, Theme.Dim, card.Inner / 2), clock));

            var steps = new ValuePicker(Enumerable.Range(0, (int)CpuTuning.MaxSteps + 1).Select(i => (uint)i).ToArray(), "", Strings.T("graphics.cpu.uv.label"));
            steps.Value = _cpuUvEdit ?? u.AppliedUvSteps;
            steps.Stepped += (s, e) => _cpuUvEdit = steps.Value;
            card.Add(Ui.WrapRow(card.Inner, Ui.Label(Strings.T("graphics.cpu.uv.label"), null, Theme.Dim, card.Inner / 2), steps));

            var temp = new ValuePicker(Enumerable.Range((int)CpuTuning.MinTempC, (int)(CpuTuning.MaxTempC - CpuTuning.MinTempC) + 1).Select(i => (uint)i).ToArray(), " C", Strings.T("graphics.cpu.temp.label"));
            temp.Value = _cpuTempEdit ?? (u.AppliedTempC != 0 ? u.AppliedTempC : u.CapC != 0 ? u.CapC : CpuTuning.MaxTempC);
            temp.Stepped += (s, e) => _cpuTempEdit = temp.Value;
            card.Add(Ui.WrapRow(card.Inner, Ui.Label(Strings.T("graphics.cpu.temp.label"), null, Theme.Dim, card.Inner / 2), temp));

            var row = Ui.WrapRow(card.Inner);
            var apply = Ui.Button(Strings.T("graphics.cpu.apply"), (s, e) => RunAction("cpu-trial", new Recovery.PlanArgs
            {
                CpuClock = _cpuClockEdit, CpuUv = _cpuUvEdit, CpuTemp = _cpuTempEdit, Window = TunerPlan.DefaultWindowMs,
            }), true);
            apply.Enabled = proven && (_cpuClockEdit != null || _cpuUvEdit != null || _cpuTempEdit != null);
            row.Controls.Add(apply);
            var keep = Ui.Button(Strings.T("graphics.tuning.keep"), (s, e) => RunAction("cpu-keep", null, null, null, false, ok => ClearCpuEdit()));
            keep.Enabled = onTrial;
            row.Controls.Add(keep);
            var stop = Ui.Button(Strings.T("graphics.tuning.stop"), (s, e) => RunAction("cpu-stop", null, null, null, false, ok => ClearCpuEdit()));
            stop.Enabled = onTrial;
            row.Controls.Add(stop);
            var reset = Ui.Button(Strings.T("graphics.cpu.reset"), (s, e) => RunAction("cpu-reset", null, null, null, false, ok => ClearCpuEdit()));
            reset.Enabled = proven && (u.Has(CpuState.FlagStored) || onTrial || u.AppliedMaxMHz != 0 || u.AppliedUvSteps != 0 || u.AppliedTempC != 0);
            row.Controls.Add(reset);
            row.Controls.Add(Ui.Button(Strings.T("graphics.cpu.disable"), (s, e) => RunAction("cpu-disable")));
            card.Add(row);
            return card;
        }

        void ClearCpuEdit()
        {
            _cpuClockEdit = null; _cpuUvEdit = null; _cpuTempEdit = null;
            ShowPage(_page, null, false);
        }

        CardPanel BuildCoresCard(int width, bool installed)
        {
            var card = new CardPanel(Strings.T("graphics.cpu.cores.title"), width);
            Mark("graphics.cpu-cores", card);
            var u = CpuNow;
            if (!installed || u == null || !u.Has(CpuState.FlagValid) || !u.Has(CpuState.FlagTuneOn))
            {
                card.Add(Ui.Dim(Strings.T("graphics.cpu.cores.note"), card.Inner));
                return card;
            }
            uint stored = u.CoreMaskStored != 0 ? u.CoreMaskStored : u.CoreMask;
            uint chosen = _coresEdit ?? CpuTuning.CoresFor(stored);
            var r6 = Ui.Radio(Strings.T("graphics.cpu.cores.6"));
            var r8 = Ui.Radio(Strings.T("graphics.cpu.cores.8"));
            r6.Checked = chosen == CpuTuning.StockCores; r8.Checked = chosen == CpuTuning.FullCores;
            r6.CheckedChanged += (s, e) => { if (r6.Checked) { _coresEdit = CpuTuning.StockCores; ShowPage(_page, null, false); } };
            r8.CheckedChanged += (s, e) => { if (r8.Checked) { _coresEdit = CpuTuning.FullCores; ShowPage(_page, null, false); } };
            card.Add(Ui.WrapRow(card.Inner, r6, r8, Explain("cpu-cores", Strings.T("graphics.cpu.cores.title"))));
            card.Add(Ui.Dim(Strings.T("graphics.cpu.cores.note"), card.Inner));
            if (u.Has(CpuState.FlagCorePending)) card.Add(Ui.Label(Strings.T("cu.choice.after-restart"), null, Theme.Warn, card.Inner));
            var use = Ui.Button(Strings.T("graphics.cpu.cores.apply"), (s, e) => RunAction("core-mask", new Recovery.PlanArgs { Cores = chosen }));
            use.Enabled = chosen != CpuTuning.CoresFor(stored);
            card.Add(Ui.WrapRow(card.Inner, use));
            return card;
        }

        // The live part of this page: the driver keeps the countdown, so the window reads it and shows what it read. A
        // trial that ended (or a Keep from another window) changes the signature, and then the page is rebuilt.
        void TickTuning()
        {
            if (_page != "graphics") return;
            var curve = Kmd.Curve(); if (curve.Value != null) _snap.Curve = curve.Value;
            var cpu = Kmd.Cpu(); if (cpu.Value != null) _snap.Cpu = cpu.Value;
            var c = CurveNow; var u = CpuNow;
            var now = (c == null ? "-" : c.Flags + ":" + c.Serial) + "/" + (u == null ? "-" : u.Flags + ":" + u.Serial);
            if (now != _tuneState) { ShowPage(_page, null, false); return; }
            Label l;
            if (c != null && c.Has(CurveState.FlagOnTrial) && _live.TryGetValue("tuner.curve.countdown", out l))
                l.Text = Strings.T("graphics.tuning.running", Tuner.Countdown(c.TrialRemainingMs));
            if (u != null && u.Has(CpuState.FlagOnTrial) && _live.TryGetValue("tuner.cpu.countdown", out l))
                l.Text = Strings.T("graphics.tuning.running", Tuner.Countdown(u.TrialRemainingMs));
        }

        // The tuning lines of the support report and the status card: what is in force, what is on trial, what is
        // stored. English, like the rest of the report.
        public static List<string> TuningReport(CurveState c, CpuState u)
        {
            var v = new List<string>();
            if (c == null) v.Add("voltage curve: no reading");
            else
            {
                v.Add("voltage curve: " + (c.Has(CurveState.FlagValid) ? "valid" : "not valid") + ", " +
                    (c.Has(CurveState.FlagGoverning) ? "governing" : "not governing") + ", mode " + c.Mode +
                    ", level " + c.Level + " (" + c.LevelMHz + " MHz, " + c.LevelMv + " mV), ceiling " + c.CeilingMHz + " MHz");
                v.Add("  active " + TunerPlan.CurveText(c.Active));
                v.Add("  default " + TunerPlan.CurveText(c.Default));
                v.Add("  floor " + TunerPlan.CurveText(c.Floor));
                v.Add("  stored " + (c.Has(CurveState.FlagStored) ? TunerPlan.CurveText(c.Stored) : "none") +
                    (c.Has(CurveState.FlagPending) ? ", pending" : "") + (c.Has(CurveState.FlagConfirmed) ? ", confirmed" : ""));
                v.Add("  trial " + (c.Has(CurveState.FlagOnTrial) ? TunerPlan.CurveText(c.Candidate) + ", " + c.TrialRemainingMs + " ms left of " + c.TrialMs : "none") +
                    ", last reason " + c.Error + " at level " + c.ErrorLevel +
                    ", sets " + c.Sets + " keeps " + c.Keeps + " cancels " + c.Cancels + " reverts " + c.Reverts);
            }
            if (u == null) { v.Add("processor tuning: no reading"); return v; }
            v.Add("processor tuning: " + (u.Has(CpuState.FlagTuneOn) ? "on" : "off") + ", " +
                (u.Has(CpuState.FlagQueue3Proven) ? "readback proven" : "no readback") +
                (u.Has(CpuState.FlagBusy) ? ", busy" : "") + ", voltage " + u.VoltageMv + " mV, cap " + u.CapC + " C");
            v.Add("  applied " + u.AppliedMaxMHz + " MHz / " + u.AppliedUvSteps + " steps / " + u.AppliedTempC + " C" +
                ", stored " + u.StoredMaxMHz + " / " + u.StoredUvSteps + " / " + u.StoredTempC +
                ", baseline " + u.BaselineMaxMHz + " / " + u.BaselineUvSteps + " / " + u.BaselineTempC);
            v.Add("  cores " + u.Cores + " / " + u.Threads + ", mask " + u.CoreMask + ", stored mask " + u.CoreMaskStored +
                (u.Has(CpuState.FlagCorePending) ? ", pending" : "") + (u.Has(CpuState.FlagCoreConfirmed) ? ", confirmed" : "") +
                ", trial " + (u.Has(CpuState.FlagOnTrial) ? u.TrialRemainingMs + " ms left of " + u.TrialMs : "none"));
            v.Add("  cores at " + string.Join(", ", u.CoreMHz.Select(x => x.ToString(CultureInfo.InvariantCulture))) + " MHz" +
                ", p-states " + string.Join(", ", u.PstateMHz.Select(x => x.ToString(CultureInfo.InvariantCulture))) + " MHz");
            v.Add("  last message queue " + u.LastQueue + " message " + u.LastMessage + " status " + u.LastStatus + " parameter " + u.LastParameter +
                ", reads " + u.Reads + " writes " + u.Writes + " refusals " + u.Refusals + " reverts " + u.Reverts +
                ", search step " + u.SearchStep + " best " + u.SearchBest + " fail " + u.SearchFail + " tested " + u.SearchTested);
            return v;
        }
    }
}
