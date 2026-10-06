// The tuning cards of the Graphics page: the voltage curve, the processor, and the number of processor cores, behind
// one "Advanced tuning" card that is closed until a person opens it. The window edits numbers and asks; the driver
// owns the trial window, the revert and every bound (docs/design/tuner.md).
//
// Three rules shape this page:
//   - Nothing is saved by trying it. Every change runs as a trial with a countdown the driver keeps, so a setting
//     that hangs the machine is gone at the next start even if the application never runs again.
//   - The reference lines come from the driver (Default and Floor of its reading), not from the table in Tuner.cs:
//     a driver with other limits moves the chart, and the window does not argue with it.
//   - The processor numbers are community reports. The card says so, and nothing is sent to the processor's own
//     mailbox queue until one readback has answered on this start.
// What each card says and which button works is decided in TunerView.cs, where the host tests reach it.
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
        bool _tuningOpen;               // the person opened "Advanced tuning" (this window only, never stored)
        readonly TrialWatch _trials = new TrialWatch();

        CurveState CurveNow { get { return _snap != null ? _snap.Curve : null; } }
        CpuState CpuNow { get { return _snap != null ? _snap.Cpu : null; } }

        // The cards poll the driver while they are open: the clock, voltage and temperature rows change by themselves.
        bool TuningShown { get { return _page == "graphics" && (_tuningOpen || TunerView.MustShow(CurveNow, CpuNow)); } }

        static string TuneSignature(CurveState c, CpuState u)
        {
            return (c == null ? "-" : c.Flags + ":" + c.Serial + ":" + c.Generation) + "/" + (u == null ? "-" : u.Flags + ":" + u.Serial + ":" + u.Generation);
        }

        // A search result or a link inside the tuning cards opens them first, so the anchor exists.
        static bool TuningAnchor(string anchor)
        {
            return anchor != null && (anchor.StartsWith("graphics.tuning", StringComparison.Ordinal) || anchor == "graphics.cpu-tuning" || anchor == "graphics.cpu-cores");
        }

        void AddTuning(Control page, int width, bool installed)
        {
            var c = CurveNow; var u = CpuNow;
            _trials.Observe(c, u);
            bool must = TunerView.MustShow(c, u);
            bool open = _tuningOpen || must;

            var head = new CardPanel(Strings.T("graphics.tuning.title"), width);
            Mark("graphics.tuning", head);
            head.Add(Ui.Dim(Strings.T("graphics.tuning.intro"), head.Inner));
            if (!open && installed)
                foreach (var line in TunerView.Summary(c, u)) head.Add(Ui.Label(line, null, null, head.Inner));
            var toggle = Ui.Button(Strings.T(open ? "graphics.tuning.hide" : "graphics.tuning.show"), (s, e) => { _tuningOpen = !open; ShowPage(_page, null, false); });
            // A running trial keeps the cards open: its countdown and its Keep button must stay in sight.
            toggle.Enabled = !(open && must);
            head.Add(Ui.WrapRow(head.Inner, toggle, Explain("tuning", Strings.T("graphics.tuning.title"))));
            page.Controls.Add(head);
            if (open)
            {
                page.Controls.Add(BuildCurveCard(width, installed));
                page.Controls.Add(BuildCpuCard(width, installed));
                page.Controls.Add(BuildCoresCard(width, installed));
            }
            _tuneState = TuneSignature(c, u);
        }

        CardPanel BuildCurveCard(int width, bool installed)
        {
            var card = new CardPanel(Strings.T("graphics.tuning.curve.title"), width);
            Mark("graphics.tuning.curve", card);
            var c = CurveNow;
            var v = TunerView.Curve(c, _curveEdit, installed, _snap.P("DpmCurveLastReason"), _trials.Curve);
            switch (v.Card)
            {
                case TuneCard.NotInstalled: card.Add(Ui.Dim(Strings.T("graphics.not-installed"), card.Inner)); return card;
                case TuneCard.NoReading: card.Add(Ui.Dim(Strings.T("perf.no-reading"), card.Inner)); return card;
                case TuneCard.NeedAuto: card.Add(Ui.Label(Strings.T("graphics.tuning.need-auto"), null, Theme.Warn, card.Inner)); return card;
            }

            // What is in force, where the chip is now, and what every start uses.
            var status = Ui.Label(v.Status, Theme.Bold, v.StatusWarn ? Theme.Warn : (Color?)null, card.Inner);
            _live["tuner.curve.status"] = status;
            card.Add(status);
            if (v.OnTrial) card.Add(Ui.Label(Strings.T("graphics.tuning.unsaved"), null, Theme.Warn, card.Inner));
            if (v.Waiting != null) card.Add(Ui.Label(v.Waiting, null, Theme.Dim, card.Inner));
            if (v.Outcome != null) card.Add(Ui.Label(v.Outcome, Theme.Bold, v.OutcomeGood ? Theme.Good : Theme.Warn, card.Inner));
            if (v.GuardNote != null) card.Add(Ui.Label(v.GuardNote, null, Theme.Warn, card.Inner));
            if (v.Live != null)
            {
                var live = Ui.Label(v.Live, null, Theme.Teal, card.Inner);
                _live["tuner.curve.live"] = live;
                card.Add(live);
            }
            card.Add(Ui.Dim(v.Saved, card.Inner));

            // The presets: four legal curves by construction, so a person never has to read a chart to use this card.
            var presets = Ui.WrapRow(card.Inner);
            foreach (var name in Tuner.PresetNames)
            {
                string id = name;
                var r = Ui.Radio(Strings.T("graphics.tuning.preset." + id));
                r.Checked = v.Preset == id;
                r.CheckedChanged += (s, e) =>
                {
                    if (!r.Checked || Tuner.PresetOf(TunerView.Shown(CurveNow, _curveEdit), v.Line, v.Floor) == id) return;
                    _curveEdit = Tuner.Preset(id, v.Line, v.Floor);
                    _trials.Forget(true, false);
                    ShowPage(_page, null, false);
                };
                presets.Controls.Add(r);
            }
            card.Add(Ui.Label(Strings.T("graphics.tuning.preset"), Theme.Bold, null, card.Inner));
            card.Add(presets);

            // The chart, the two buttons that move the selected knot, and the table under it (G-A11Y: the table alone
            // is enough to read every value).
            var chart = new CurveChart(card.Inner, Theme.S(170));
            chart.SetReference(v.Line, v.Floor);
            chart.SetMarks(v.NowIndex, c.LevelMv, v.CeilingIndex);
            chart.Values = v.Shown;
            chart.Selected = _curveLevel;
            chart.Changed += (s, e) => { _curveEdit = chart.Values; _curveLevel = chart.Selected; _trials.Forget(true, false); ShowPage(_page, null, false); };
            chart.Picked += (s, e) => { _curveLevel = chart.Selected; ShowPage(_page, null, false); };
            Mark("graphics.tuning.chart", chart);
            card.Add(Ui.Label(Strings.T("graphics.tuning.chart"), Theme.Bold, null, card.Inner));
            card.Add(chart);
            card.Add(Ui.Dim(Strings.T("graphics.tuning.chart.help"), card.Inner));
            card.Add(Ui.Dim(Strings.T("graphics.tuning.chart.marks"), card.Inner));
            card.Add(Ui.WrapRow(card.Inner,
                Ui.Button(Strings.T("graphics.tuning.lower"), (s, e) => Nudge(-5)),
                Ui.Button(Strings.T("graphics.tuning.raise"), (s, e) => Nudge(5))));

            card.Add(Ui.Label(Strings.T("graphics.tuning.table"), Theme.Bold, null, card.Inner));
            for (int i = 0; i < v.Rows.Count; i++)
                card.Pair(v.Rows[i].Key, v.Rows[i].Value, v.Rows[i].Warn ? Theme.Warn : i == _curveLevel ? Theme.Text : Theme.Dim);
            if (v.ErrorText != null)
                card.Add(Ui.Label(v.ErrorText, Theme.Bold, Theme.Warn, card.Inner));

            var row = Ui.WrapRow(card.Inner);
            var apply = Ui.Button(Strings.T("graphics.tuning.apply"), (s, e) => RunAction("tune-trial", TunerView.CurveRequest(v), null, null, false, ok => ClearCurveEdit()), true);
            apply.Enabled = v.ApplyEnabled;
            row.Controls.Add(apply);
            // Keeping a curve nobody has run is how a bad curve reaches every later start: the driver stores what
            // the governor applied, so the button waits for the governor to apply it (it does so on its next tick).
            var keep = Ui.Button(Strings.T("graphics.tuning.keep"), (s, e) => RunAction("tune-keep", null, null, null, false, ok => ClearCurveEdit()));
            keep.Enabled = v.KeepEnabled;
            row.Controls.Add(keep);
            var stop = Ui.Button(Strings.T("graphics.tuning.stop"), (s, e) => RunAction("tune-stop", null, null, null, false, ok => { if (ok) _trials.Did(false, TrialEnd.Stopped); ClearCurveEdit(); }));
            stop.Enabled = v.StopEnabled;
            row.Controls.Add(stop);
            var reset = Ui.Button(Strings.T("graphics.tuning.reset"), (s, e) => RunAction("tune-reset", null, null, null, false, ok => { if (ok) _trials.Did(false, TrialEnd.Reset); ClearCurveEdit(); }));
            reset.Enabled = v.ResetEnabled;
            row.Controls.Add(reset);
            card.Add(row);
            if (_curveEdit != null)
                card.Add(Ui.WrapRow(card.Inner, Ui.Button(Strings.T("tuner.discard"), (s, e) => ClearCurveEdit())));
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
            var v = TunerView.Curve(c, _curveEdit, true, null, TrialEnd.None);
            if (v.Card != TuneCard.Ready) return;
            var mv = v.Shown;
            mv[_curveLevel] = Tuner.Nudge(mv[_curveLevel], delta, _curveLevel, v.Line, v.Floor);
            _curveEdit = mv;
            _trials.Forget(true, false);
            ShowPage(_page, null, false);
        }

        CardPanel BuildCpuCard(int width, bool installed)
        {
            var card = new CardPanel(Strings.T("graphics.cpu.title"), width);
            Mark("graphics.cpu-tuning", card);
            var u = CpuNow;
            var v = TunerView.Cpu(u, installed, _snap.P("CpuTune"), _snap.P("CpuLastReason"), _cpuClockEdit, _cpuUvEdit, _cpuTempEdit, _trials.Cpu);
            card.Add(Ui.Dim(Strings.T("graphics.cpu.intro"), card.Inner));
            switch (v.Card)
            {
                case TuneCard.NotInstalled: card.Add(Ui.Dim(Strings.T("graphics.not-installed"), card.Inner)); return card;
                case TuneCard.NoReading: card.Add(Ui.Dim(Strings.T("perf.no-reading"), card.Inner)); return card;
                case TuneCard.Off:
                case TuneCard.OnAfterRestart:
                    // The one setting of this page that needs a restart: the driver reads it at its start.
                    card.Add(Ui.Label(v.Status, Theme.Bold, v.Card == TuneCard.OnAfterRestart ? Theme.Warn : Theme.Dim, card.Inner));
                    var offer = Ui.WrapRow(card.Inner);
                    if (v.EnableOffered) offer.Controls.Add(Ui.Button(Strings.T("graphics.cpu.enable"), (s, e) => RunAction("cpu-enable"), true));
                    if (v.Card == TuneCard.OnAfterRestart)
                        offer.Controls.Add(Ui.Button(Strings.T("status.action.restart"), (s, e) => { if (RestartDialog.Ask(this, Hints.Decide(Hints.Read(_recent)))) RestartWindows(); }, true));
                    if (v.DisableOffered) offer.Controls.Add(Ui.Button(Strings.T("graphics.cpu.disable"), (s, e) => RunAction("cpu-disable")));
                    offer.Controls.Add(Explain("cpu-tuning", Strings.T("graphics.cpu.title")));
                    card.Add(offer);
                    return card;
            }

            // The driver could not put the settings back and keeps trying. The person has to know, because the
            // processor is running a setting nobody chose to keep (0.7.211).
            if (v.RevertOwed != null) card.Add(Ui.Label(v.RevertOwed, Theme.Bold, Theme.Warn, card.Inner));
            var status = Ui.Label(v.Status, Theme.Bold, v.StatusWarn ? Theme.Warn : (Color?)null, card.Inner);
            _live["tuner.cpu.status"] = status;
            card.Add(status);
            if (v.OnTrial) card.Add(Ui.Label(Strings.T("graphics.tuning.unsaved"), null, Theme.Warn, card.Inner));
            if (v.Outcome != null) card.Add(Ui.Label(v.Outcome, Theme.Bold, v.OutcomeGood ? Theme.Good : Theme.Warn, card.Inner));
            if (v.GuardNote != null) card.Add(Ui.Label(v.GuardNote, null, Theme.Warn, card.Inner));
            if (v.AfterRestart != null) card.Add(Ui.Label(v.AfterRestart, null, Theme.Warn, card.Inner));

            // Step 1 of the read-first order: what the processor says now.
            card.Add(Ui.Label(Strings.T("tuner.cpu.readings"), Theme.Bold, null, card.Inner));
            foreach (var r in v.Readings) card.Pair(r.Key, r.Value, r.Warn ? Theme.Warn : (Color?)null);
            var read = Ui.Button(Strings.T("graphics.cpu.readback"), (s, e) => RunAction("cpu-readback"), !v.Proven);
            read.Enabled = v.ReadEnabled;
            card.Add(Ui.WrapRow(card.Inner, read, Explain("cpu-tuning", Strings.T("graphics.cpu.title"))));
            if (!v.Proven) card.Add(Ui.Label(Strings.T("graphics.cpu.need-readback"), null, Theme.Warn, card.Inner));

            // Step 2: what is in force, saved, and what this start recorded as standard.
            card.Add(Ui.Label(Strings.T("tuner.cpu.settings.title"), Theme.Bold, null, card.Inner));
            foreach (var r in v.InForce) card.Pair(r.Key, r.Value, r.Strong ? Theme.Text : (Color?)null);

            // Step 3: the three numbers, each inside the driver's own list, so a step can never leave the admitted band.
            var row = Ui.WrapRow(card.Inner);
            var apply = Ui.Button(Strings.T("graphics.cpu.apply"), (s, e) => RunAction("cpu-trial", TunerView.CpuRequest(_cpuClockEdit, _cpuUvEdit, _cpuTempEdit), null, null, false, ok => ClearCpuEdit()), true);
            apply.Enabled = v.ApplyEnabled;
            Action edited = () =>
            {
                _trials.Forget(false, true);
                apply.Enabled = TunerView.Cpu(CpuNow, installed, _snap.P("CpuTune"), _snap.P("CpuLastReason"), _cpuClockEdit, _cpuUvEdit, _cpuTempEdit, TrialEnd.None).ApplyEnabled;
            };
            card.Add(Ui.Label(Strings.T("tuner.cpu.choose"), Theme.Bold, null, card.Inner));
            var clock = new ValuePicker(v.ClockChoices, " MHz", Strings.T("graphics.cpu.clock.label"));
            clock.Value = v.Clock;
            clock.Stepped += (s, e) => { _cpuClockEdit = clock.Value; edited(); };
            card.Add(Ui.WrapRow(card.Inner, PickerName(Strings.T("graphics.cpu.clock.label"), card.Inner), clock));
            var steps = new ValuePicker(v.StepChoices, "", Strings.T("graphics.cpu.uv.label"));
            steps.Value = v.Steps;
            steps.Stepped += (s, e) => { _cpuUvEdit = steps.Value; edited(); };
            card.Add(Ui.WrapRow(card.Inner, PickerName(Strings.T("graphics.cpu.uv.label"), card.Inner), steps));
            var temp = new ValuePicker(v.TempChoices, " °C", Strings.T("graphics.cpu.temp.label"));
            temp.Value = v.Temp;
            temp.Stepped += (s, e) => { _cpuTempEdit = temp.Value; edited(); };
            card.Add(Ui.WrapRow(card.Inner, PickerName(Strings.T("graphics.cpu.temp.label"), card.Inner), temp));
            card.Add(Ui.Dim(Strings.T("tuner.cpu.choose.help"), card.Inner));

            row.Controls.Add(apply);
            var keep = Ui.Button(Strings.T("graphics.tuning.keep"), (s, e) => RunAction("cpu-keep", null, null, null, false, ok => ClearCpuEdit()));
            keep.Enabled = v.KeepEnabled;
            row.Controls.Add(keep);
            var stop = Ui.Button(Strings.T("graphics.tuning.stop"), (s, e) => RunAction("cpu-stop", null, null, null, false, ok => { if (ok) _trials.Did(true, TrialEnd.Stopped); ClearCpuEdit(); }));
            stop.Enabled = v.StopEnabled;
            row.Controls.Add(stop);
            var reset = Ui.Button(Strings.T("graphics.cpu.reset"), (s, e) => RunAction("cpu-reset", null, null, null, false, ok => { if (ok) _trials.Did(true, TrialEnd.Reset); ClearCpuEdit(); }));
            reset.Enabled = v.ResetEnabled;
            row.Controls.Add(reset);
            card.Add(row);
            var more = Ui.WrapRow(card.Inner);
            if (v.Edited) more.Controls.Add(Ui.Button(Strings.T("tuner.discard"), (s, e) => ClearCpuEdit()));
            if (v.DisableOffered) more.Controls.Add(Ui.Button(Strings.T("graphics.cpu.disable"), (s, e) => RunAction("cpu-disable")));
            if (v.EnableOffered) more.Controls.Add(Ui.Button(Strings.T("graphics.cpu.enable"), (s, e) => RunAction("cpu-enable")));
            if (more.Controls.Count > 0) card.Add(more);
            return card;
        }

        // The name in front of a picker, one width for all three so that the pickers line up in one column.
        static Label PickerName(string text, int inner)
        {
            int w = Math.Min(Theme.S(210), inner / 3);
            var l = Ui.Label(text, null, Theme.Dim, w);
            l.MinimumSize = new Size(w, 0);
            l.Margin = Theme.Pad(0, 12, 0, 0);
            return l;
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
            if (_page != "graphics" || _fixture) return;
            var curve = Kmd.Curve(); if (curve.Value != null) _snap.Curve = curve.Value;
            var cpu = Kmd.Cpu(); if (cpu.Value != null) _snap.Cpu = cpu.Value;
            var c = CurveNow; var u = CpuNow;
            if (TuneSignature(c, u) != _tuneState) { ShowPage(_page, null, false); return; }
            _trials.Observe(c, u);
            Label l;
            var cv = TunerView.Curve(c, _curveEdit, _snap.DriverInstalled, _snap.P("DpmCurveLastReason"), _trials.Curve);
            if (cv.Status != null && _live.TryGetValue("tuner.curve.status", out l)) l.Text = cv.Status;
            if (cv.Live != null && _live.TryGetValue("tuner.curve.live", out l)) l.Text = cv.Live;
            var uv = TunerView.Cpu(u, _snap.DriverInstalled, _snap.P("CpuTune"), _snap.P("CpuLastReason"), _cpuClockEdit, _cpuUvEdit, _cpuTempEdit, _trials.Cpu);
            if (uv.Status != null && _live.TryGetValue("tuner.cpu.status", out l)) l.Text = uv.Status;
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
