// The case fan card of the Performance page (docs/design/fan.md Part B): who runs the fan, how fast it turns, and the
// four choices - the board's own BIOS setting, the driver's curve (the default), Quiet and Performance - plus a curve
// the person edits on a chart or point by point. The window edits points and asks; the driver checks every rule again,
// stores the choice and runs the fan at full speed from 87 C whatever the curve says. A short test runs one speed under
// the driver's lease and then puts the choice in force back.
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
        // The card's modes in the order of the segmented row.
        static readonly string[] FanModes = { "board", "standard", "quiet", "performance", "custom" };

        sealed class FanChoiceRecord
        {
            public string Choice;
            public uint[] C, Pct;               // the points for "custom", else null
        }

        string _fanChoice;              // null: follow what the driver has
        uint[] _fanEditC, _fanEditPct;  // null: no curve edit
        string _fanState;               // the signature of the last build, so a change by the driver rebuilds the card
        int _fanPoint;                  // the point selected on the chart
        FanChoiceRecord _fanUndo;       // the choice before the last Apply, for Undo; null: nothing to undo
        uint _fanTestPct = 60;          // the speed the test picker shows
        uint? _fanTesting;              // the speed of the test that runs now
        uint _fanTestMax;               // the fastest the fan turned while the test ran
        string _fanTestResult;          // the last test in plain words
        FanChart _fanChart;
        Label _fanNow;

        FanState FanNow { get { return _snap != null ? _snap.Fan : null; } }

        static string FanSignature(FanState f)
        {
            return f == null ? "-" : f.Flags + ":" + f.State + ":" + f.Mode + ":" + f.Profile + ":" + f.StoredMode + ":" + f.StoredProfile +
                ":" + FanCurves.CurveText(FanCurves.ShownC(f), FanCurves.ShownPct(f));
        }

        // The choice the card shows: the person's pick, else what the driver runs (a lease of the command line shows as
        // the stored choice, because that is what comes back when it ends).
        string FanChoiceShown(FanState f)
        {
            if (_fanEditC != null) return "custom";
            if (_fanChoice != null) return _fanChoice;
            return FanCurves.StoredChoice(f);
        }

        // The curve the card shows: the edit, else the preset of the choice, else the driver's curve in force.
        void FanCurveShown(FanState f, string choice, out uint[] c, out uint[] pct)
        {
            if (_fanEditC != null) { c = (uint[])_fanEditC.Clone(); pct = (uint[])_fanEditPct.Clone(); return; }
            if (choice != "custom" && choice != "board" && FanCurves.Preset(FanCurves.ProfileOf(choice), out c, out pct)) return;
            c = FanCurves.ShownC(f); pct = FanCurves.ShownPct(f);
            if (c == null || c.Length < FanCurves.MinPoints) FanCurves.Preset(FanState.ProfileStandard, out c, out pct);
        }

        // The fan speed, from the same source as the "Now" card (Sensors.FanValue).
        string FanSpeedText(FanState f)
        {
            string level;
            return Sensors.FanValue(_fan, f, out level) ?? Strings.T("perf.no-reading");
        }

        // Where the fan runs now, in words, and whether the chart draws its ring: only while the driver runs the fan.
        static bool FanNowShown(FanState f) { return f != null && f.Has(FanState.FlagControlling) && f.GuardMc > 0; }

        static string FanNowText(FanState f)
        {
            return Strings.T("perf.fan.now", TunerView.Temperature(f.GuardMc), f.AppliedPct);
        }

        CardPanel BuildFanCard(int width)
        {
            var card = new CardPanel(Strings.T("perf.fan.title"), width);
            Mark("perf.fan", card);
            _fanChart = null; _fanNow = null;
            var f = FanNow;
            _fanState = FanSignature(f);
            card.Add(Ui.Dim(Strings.T("perf.fan.intro"), card.Inner));
            if (!_snap.DriverInstalled || f == null)
            {
                card.Add(Ui.Dim(Strings.T(!_snap.DriverInstalled ? "graphics.not-installed" : "perf.fan.no-control"), card.Inner));
                return card;
            }
            var speed = card.Pair(Strings.T("perf.fan.speed"), FanSpeedText(f));
            _live["perf.fan.speed"] = speed;
            var who = card.Pair(Strings.T("perf.fan.who"), FanCurves.StateText(f),
                f.State == FanState.StateEmergency || f.State == FanState.StateDoubt || f.State == FanState.StateFault ? Theme.Warn : (Color?)null);
            _live["perf.fan.who"] = who;
            var gate = FanCurves.GateText(f);
            if (gate != null)
            {
                card.Add(Ui.Label(gate, null, Theme.Dim, card.Inner));
                return card;
            }

            // The five modes as one segmented row, and one line that says what the selected one does.
            string choice = FanChoiceShown(f);
            var heading = Ui.Label(Strings.T("perf.fan.choice"), Theme.Bold, null, card.Inner - Theme.S(40));
            heading.Margin = Theme.Pad(0, 7, 0, 0);
            card.Add(Ui.Row(heading, Explain("fan", Strings.T("perf.fan.title"))));
            var modes = Ui.WrapRow(card.Inner);
            foreach (var name in FanModes)
            {
                string id = name;
                var r = Segment(Strings.T("perf.fan.choice." + id));
                r.Checked = choice == id;
                r.CheckedChanged += (s, e) =>
                {
                    if (!r.Checked || FanChoiceShown(FanNow) == id) return;
                    if (id == "custom") StartFanEdit();
                    else { _fanChoice = id; _fanEditC = null; _fanEditPct = null; }
                    ShowPage(_page, null, false);
                };
                modes.Controls.Add(r);
            }
            card.Add(modes);
            card.Add(Ui.Dim(Strings.T("perf.fan.mode." + choice), card.Inner));

            uint[] c, pct;
            FanCurveShown(f, choice, out c, out pct);
            int point;
            var error = FanCurves.Check(c, pct, out point);
            if (choice != "board")
            {
                // The chart: a preset previews its curve here, and a drag or a key turns it into the person's own curve.
                _fanPoint = Math.Max(0, Math.Min(c.Length - 1, _fanPoint));
                var chart = new FanChart(card.Inner, Theme.S(190));
                chart.SetCurve(c, pct);
                chart.SetInForce(f.Mode == FanState.ModeCurve ? FanCurves.ShownC(f) : null, FanCurves.ShownPct(f));
                if (FanNowShown(f)) chart.SetNow(f.GuardMc / 1000.0, f.AppliedPct);
                chart.Selected = _fanPoint;
                chart.Changed += (s, e) =>
                {
                    StartFanEdit();
                    _fanEditC = chart.C; _fanEditPct = chart.Pct; _fanPoint = chart.Selected;
                    ShowPage(_page, null, false);
                };
                chart.Picked += (s, e) => { _fanPoint = chart.Selected; ShowPage(_page, null, false); };
                _fanChart = chart;
                Mark("perf.fan.chart", chart);
                card.Add(chart);
                if (FanNowShown(f)) _fanNow = card.Add(Ui.Label(FanNowText(f), Theme.Bold, null, card.Inner));
                card.Add(Ui.Dim(Strings.T("perf.fan.chart.marks"), card.Inner));
                card.Add(Ui.Dim(Strings.T("perf.fan.chart.help"), card.Inner));

                // The points as compact rows, each value inside the driver's own range, so a step can never leave it.
                card.Add(Ui.Label(Strings.T("perf.fan.curve"), Theme.Bold, null, card.Inner));
                card.Add(Ui.Dim(Strings.T("perf.fan.curve.help"), card.Inner));
                var temps = Range(FanCurves.MinC, FanCurves.MaxC, FanCurves.StepC);
                var speeds = Range(FanCurves.FloorPct, FanCurves.FullPct, FanCurves.StepPct);
                for (int i = 0; i < c.Length; i++)
                {
                    int at = i;
                    // A value off the 5-step grid (82 %, or a point moved on the chart) stays in the list as it is.
                    var t = new ValuePicker(With(temps, c[i]), " °C", Strings.T("perf.fan.point.temp", i + 1));
                    t.Value = c[i];
                    t.Stepped += (s, e) => EditFanPoint(at, t.Value, null);
                    var v = new ValuePicker(With(speeds, pct[i]), " %", Strings.T("perf.fan.point.speed", i + 1));
                    v.Value = pct[i];
                    v.Stepped += (s, e) => EditFanPoint(at, null, v.Value);
                    bool selected = i == _fanPoint;
                    var name = Ui.Label(Strings.T("perf.fan.point", i + 1), selected ? Theme.Bold : null, selected ? Theme.Text : Theme.Dim, Theme.S(90));
                    name.MinimumSize = new Size(Theme.S(90), 0);   // the bold selected row keeps the pickers in line
                    name.Margin = Theme.Pad(0, 12, 0, 0);      // on the line of the pickers' values
                    card.Add(Ui.WrapRow(card.Inner, name, t, v));
                }
                var edit = Ui.WrapRow(card.Inner);
                var add = Ui.Button(Strings.T("perf.fan.add"), (s, e) => AddFanPoint());
                add.Enabled = c.Length < FanCurves.MaxPoints;
                edit.Controls.Add(add);
                var remove = Ui.Button(Strings.T("perf.fan.remove"), (s, e) => RemoveFanPoint());
                remove.Enabled = c.Length > FanCurves.MinPoints;
                edit.Controls.Add(remove);
                var reset = Ui.Button(Strings.T("perf.fan.reset"), (s, e) => { _fanEditC = null; _fanEditPct = null; _fanChoice = "standard"; ShowPage(_page, null, false); });
                reset.Enabled = choice != "standard";
                edit.Controls.Add(reset);
                card.Add(edit);
                if (error != FanCurveError.Ok)
                    card.Add(Ui.Label(FanCurves.ErrorText(error, point), Theme.Bold, Theme.Warn, card.Inner));

                // What the driver does whatever the curve says (fan.md rules 4 to 8), in plain words.
                card.Add(Ui.Label(Strings.T("perf.fan.rules"), Theme.Bold, null, card.Inner));
                foreach (var rule in new[] { "input", "doubt", "slow", "hot", "floor" })
                    card.Add(Ui.Dim("• " + Strings.T("perf.fan.rule." + rule), card.Inner));
            }

            bool changed = FanCurves.Changed(f, choice, c, pct);
            var row = Ui.WrapRow(card.Inner);
            var apply = Ui.Button(Strings.T(choice == "board" ? "perf.fan.apply.board" : "perf.fan.apply"), (s, e) => ApplyFan(), true);
            apply.Enabled = changed && (choice == "board" || error == FanCurveError.Ok);
            row.Controls.Add(apply);
            if (_fanChoice != null || _fanEditC != null)
                row.Controls.Add(Ui.Button(Strings.T("tuner.discard"), (s, e) => ClearFanEdit()));
            if (_fanUndo != null)
                row.Controls.Add(Ui.Button(Strings.T("perf.fan.undo"), (s, e) => UndoFan()));
            card.Add(row);
            if (f.Has(FanState.FlagStored) && !changed)
                card.Add(Ui.Dim(Strings.T("graphics.tuning.saved"), card.Inner));
            var result = ResultLine(card.Inner);
            if (result != null) card.Add(result);

            // The short test: one speed for ten seconds under the driver's lease, then the choice in force again.
            card.Add(Ui.Label(Strings.T("perf.fan.test"), Theme.Bold, null, card.Inner));
            card.Add(Ui.Dim(Strings.T("perf.fan.test.help"), card.Inner));
            var picker = new ValuePicker(FanCurves.TestChoices, " %", Strings.T("perf.fan.test.speed"));
            picker.Value = _fanTestPct;
            picker.Stepped += (s, e) => _fanTestPct = picker.Value;
            bool free = FanCurves.TestAllowed(f) && _fanTesting == null;
            var run = Ui.Button(Strings.T("perf.fan.test.run"), (s, e) => TestFan());
            run.Enabled = free;
            card.Add(Ui.WrapRow(card.Inner, picker, run));
            if (!FanCurves.TestAllowed(f) && _fanTesting == null)
                card.Add(Ui.Dim(Strings.T("perf.fan.refuse.busy"), card.Inner));
            if (_fanTestResult != null)
                card.Add(Ui.Label(_fanTestResult, null, Theme.Text, card.Inner));
            return card;
        }

        // One mode of the segmented row: a radio button drawn as a flat toggle, so the five fit on one or two even rows.
        static RadioButton Segment(string text)
        {
            var r = new RadioButton
            {
                Text = text, Appearance = Appearance.Button, AutoSize = true, FlatStyle = FlatStyle.Flat, ForeColor = Theme.Text,
                BackColor = Theme.CardHi, Font = Theme.Bold, Padding = Theme.Pad(10, 3, 10, 3), Margin = Theme.Pad(0, 4, 6, 4),
                TextAlign = ContentAlignment.MiddleCenter, UseVisualStyleBackColor = false, AccessibleName = text, UseMnemonic = false, Cursor = Cursors.Hand,
            };
            r.FlatAppearance.BorderColor = Theme.Line;
            r.FlatAppearance.CheckedBackColor = Color.FromArgb(28, 78, 86);
            r.CheckedChanged += (s, e) => r.FlatAppearance.BorderColor = r.Checked ? Theme.Teal : Theme.Line;
            r.GotFocus += (s, e) => { r.FlatAppearance.BorderColor = Theme.Focus; r.FlatAppearance.BorderSize = 2; };
            r.LostFocus += (s, e) => { r.FlatAppearance.BorderColor = r.Checked ? Theme.Teal : Theme.Line; r.FlatAppearance.BorderSize = 1; };
            return r;
        }

        static uint[] Range(uint from, uint to, uint step)
        {
            var v = new List<uint>();
            for (uint x = from; x <= to; x += step) v.Add(x);
            return v.ToArray();
        }

        static uint[] With(uint[] grid, uint value)
        {
            return grid.Contains(value) || value < grid[0] || value > grid[grid.Length - 1] ? grid : grid.Concat(new[] { value }).OrderBy(x => x).ToArray();
        }

        // Any edit turns the shown curve into "Custom", starting from what the card showed.
        void StartFanEdit()
        {
            if (_fanEditC != null) return;
            var f = FanNow;
            uint[] c, pct;
            FanCurveShown(f, FanChoiceShown(f), out c, out pct);
            _fanEditC = c; _fanEditPct = pct;
            _fanChoice = "custom";
        }

        void EditFanPoint(int at, uint? temperature, uint? speed)
        {
            StartFanEdit();
            if (at >= _fanEditC.Length) return;
            if (temperature != null) _fanEditC[at] = temperature.Value;
            if (speed != null) _fanEditPct[at] = speed.Value;
            _fanPoint = at;
            ShowPage(_page, null, false);
        }

        // A new point 5 C above the last one at full speed, or between the last two when the last is at the top already.
        void AddFanPoint()
        {
            StartFanEdit();
            int n = _fanEditC.Length;
            if (n >= FanCurves.MaxPoints) return;
            var c = _fanEditC.ToList(); var pct = _fanEditPct.ToList();
            if (c[n - 1] + FanCurves.StepC <= FanCurves.MaxC) { c.Add(c[n - 1] + FanCurves.StepC); pct.Add(FanCurves.FullPct); _fanPoint = n; }
            else { c.Insert(n - 1, (c[n - 2] + c[n - 1]) / 2); pct.Insert(n - 1, (pct[n - 2] + pct[n - 1]) / 2); _fanPoint = n - 1; }
            _fanEditC = c.ToArray(); _fanEditPct = pct.ToArray();
            ShowPage(_page, null, false);
        }

        void RemoveFanPoint()
        {
            StartFanEdit();
            if (_fanEditC.Length <= FanCurves.MinPoints) return;
            _fanEditC = _fanEditC.Take(_fanEditC.Length - 1).ToArray();
            _fanEditPct = _fanEditPct.Take(_fanEditPct.Length - 1).ToArray();
            ShowPage(_page, null, false);
        }

        // The choice in force, as the card would ask for it again: what Undo goes back to.
        static FanChoiceRecord FanInForce(FanState f)
        {
            var r = new FanChoiceRecord { Choice = FanCurves.StoredChoice(f) };
            if (r.Choice == "custom") { r.C = FanCurves.ShownC(f); r.Pct = FanCurves.ShownPct(f); }
            return r;
        }

        void ApplyFan()
        {
            var f = FanNow;
            string choice = FanChoiceShown(f);
            uint[] c = null, pct = null;
            if (choice == "custom") FanCurveShown(f, choice, out c, out pct);
            var before = FanInForce(f);
            RunFanChoice(choice, c, pct, ok => { if (ok) _fanUndo = before; ClearFanEdit(); });
        }

        void UndoFan()
        {
            var back = _fanUndo;
            if (back == null) return;
            RunFanChoice(back.Choice, back.C, back.Pct, ok => { if (ok) _fanUndo = null; ClearFanEdit(); });
        }

        void RunFanChoice(string choice, uint[] c, uint[] pct, Action<bool> done)
        {
            if (choice == "board") { RunAction("fan-auto", null, null, null, false, done); return; }
            var more = new Recovery.PlanArgs { FanProfile = choice };
            if (choice == "custom") more.FanCurve = FanCurves.CurveText(c, pct);
            RunAction("fan-curve", more, null, null, false, done);
        }

        void ClearFanEdit()
        {
            _fanChoice = null; _fanEditC = null; _fanEditPct = null;
            ShowPage(_page, null, false);
        }

        // The test: the helper holds the speed for ten seconds and then puts the choice in force back. The window keeps
        // polling meanwhile, so the fastest reading of the test is the fan's answer to that speed.
        void TestFan()
        {
            uint pct = _fanTestPct;
            _fanTestResult = null;
            _fanTesting = pct; _fanTestMax = 0;
            RunAction("fan-test", new Recovery.PlanArgs { FanTestPct = pct }, null, null, false, ok =>
            {
                uint fastest = _fanTestMax;
                _fanTesting = null;
                _fanTestResult = !ok ? null : fastest != 0 ? Strings.T("perf.fan.test.result", pct, fastest) : Strings.T("perf.fan.test.no-rpm", pct);
                ShowPage(_page, null, false);
            });
            // A refusal or a cancel never reaches done: the window is enabled again at once in that case.
            if (Enabled) _fanTesting = null;
        }

        // The live part: the speed, who runs the fan and the ring on the chart every poll; a change of the choice or the
        // state rebuilds the card.
        void TickFan()
        {
            if (_page != "performance" || _snap == null || _fixture) return;
            var fan = Kmd.Fan();
            _snap.Fan = fan.Value;
            var f = _snap.Fan;
            if (_fanTesting != null && f != null && f.Mode == FanState.ModeFixed) _fanTestMax = Math.Max(_fanTestMax, f.Rpm);
            if (FanSignature(f) != _fanState) { ShowPage(_page, null, false); return; }
            Label l;
            if (f != null && _live.TryGetValue("perf.fan.speed", out l)) l.Text = FanSpeedText(f);
            if (f != null && _live.TryGetValue("perf.fan.who", out l)) l.Text = FanCurves.StateText(f);
            if (f != null && _fanChart != null && !_fanChart.IsDisposed) _fanChart.SetNow(FanNowShown(f) ? f.GuardMc / 1000.0 : -1, f.AppliedPct);
            if (f != null && _fanNow != null && !_fanNow.IsDisposed && FanNowShown(f)) _fanNow.Text = FanNowText(f);
        }
    }
}
