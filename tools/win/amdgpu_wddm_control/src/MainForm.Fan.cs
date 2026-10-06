// The case fan card of the Performance page (docs/design/fan.md Part B): who runs the fan, how fast it turns, and the
// four choices - the board's own BIOS setting, the driver's curve (the default), Quiet and Performance - plus a curve
// the person edits. The window edits points and asks; the driver checks every rule again, stores the choice and runs
// the fan at full speed from 87 C whatever the curve says.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Linq;
using System.Windows.Forms;

namespace AmdgpuWddmControl
{
    public sealed partial class MainForm
    {
        string _fanChoice;              // null: follow what the driver has
        uint[] _fanEditC, _fanEditPct;  // null: no curve edit
        string _fanState;               // the signature of the last build, so a change by the driver rebuilds the card

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

        CardPanel BuildFanCard(int width)
        {
            var card = new CardPanel(Strings.T("perf.fan.title"), width);
            Mark("perf.fan", card);
            var f = FanNow;
            _fanState = FanSignature(f);
            card.Add(Ui.Dim(Strings.T("perf.fan.intro"), card.Inner));
            if (!_snap.DriverInstalled || f == null)
            {
                card.Add(Ui.Dim(Strings.T(!_snap.DriverInstalled ? "graphics.not-installed" : "perf.fan.no-control"), card.Inner));
                return card;
            }
            var speed = card.Pair(Strings.T("perf.fan.speed"), f.Rpm != 0 ? Strings.T("perf.fan.rpm-only", f.Rpm) : Strings.T("perf.no-reading"));
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

            // The four choices, and "My curve" once the person has edited one.
            string choice = FanChoiceShown(f);
            var choices = Ui.WrapRow(card.Inner);
            foreach (var name in FanCurves.Choices.Concat(choice == "custom" ? new[] { "custom" } : new string[0]))
            {
                string id = name;
                var r = Ui.Radio(Strings.T("perf.fan.choice." + id));
                r.Checked = choice == id;
                r.CheckedChanged += (s, e) =>
                {
                    if (!r.Checked || FanChoiceShown(FanNow) == id) return;
                    _fanChoice = id;
                    if (id != "custom") { _fanEditC = null; _fanEditPct = null; }
                    ShowPage(_page, null, false);
                };
                choices.Controls.Add(r);
            }
            choices.Controls.Add(Explain("fan", Strings.T("perf.fan.title")));
            card.Add(Ui.Label(Strings.T("perf.fan.choice"), Theme.Bold, null, card.Inner));
            card.Add(choices);

            uint[] c, pct;
            FanCurveShown(f, choice, out c, out pct);
            int point;
            var error = FanCurves.Check(c, pct, out point);
            if (choice != "board")
            {
                // The curve, one row per point: the temperature and the speed from that temperature, each one inside the
                // driver's own range, so a step can never leave it. Editing any value makes it "My curve".
                card.Add(Ui.Label(Strings.T("perf.fan.curve"), Theme.Bold, null, card.Inner));
                card.Add(Ui.Dim(Strings.T("perf.fan.curve.help"), card.Inner));
                var temps = Range(FanCurves.MinC, FanCurves.MaxC, FanCurves.StepC);
                var speeds = Range(FanCurves.FloorPct, FanCurves.FullPct, FanCurves.StepPct);
                for (int i = 0; i < c.Length; i++)
                {
                    int at = i;
                    // A preset value off the 5-step grid (82 %) stays in the list, so the picker shows it as it is.
                    var t = new ValuePicker(With(temps, c[i]), " °C", Strings.T("perf.fan.point.temp", i + 1));
                    t.Value = c[i];
                    t.Stepped += (s, e) => EditFanPoint(at, t.Value, null);
                    var v = new ValuePicker(With(speeds, pct[i]), " %", Strings.T("perf.fan.point.speed", i + 1));
                    v.Value = pct[i];
                    v.Stepped += (s, e) => EditFanPoint(at, null, v.Value);
                    var name = Ui.Label(Strings.T("perf.fan.point", i + 1), null, Theme.Dim, Theme.S(90));
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
                card.Add(Ui.Dim(Strings.T("perf.fan.note"), card.Inner));
            }

            string stored = FanCurves.StoredChoice(f);
            bool sameCurve = FanCurves.CurveText(c, pct) == FanCurves.CurveText(FanCurves.ShownC(f), FanCurves.ShownPct(f));
            bool changed = choice != stored || (choice == "custom" && !sameCurve) ||
                (choice != "board" && f.Mode != FanState.ModeCurve) || (choice == "board" && f.Mode != FanState.ModeBoard);
            var row = Ui.WrapRow(card.Inner);
            var apply = Ui.Button(Strings.T("perf.fan.apply"), (s, e) => ApplyFan(), true);
            apply.Enabled = changed && (choice == "board" || error == FanCurveError.Ok);
            row.Controls.Add(apply);
            card.Add(row);
            if (f.Has(FanState.FlagStored))
                card.Add(Ui.Dim(Strings.T("graphics.tuning.saved"), card.Inner));
            var result = ResultLine(card.Inner);
            if (result != null) card.Add(result);
            return card;
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

        // Any edit turns the shown curve into "My curve", starting from what the card showed.
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
            ShowPage(_page, null, false);
        }

        // A new point 5 C above the last one at full speed, or between the last two when the last is at the top already.
        void AddFanPoint()
        {
            StartFanEdit();
            int n = _fanEditC.Length;
            if (n >= FanCurves.MaxPoints) return;
            var c = _fanEditC.ToList(); var pct = _fanEditPct.ToList();
            if (c[n - 1] + FanCurves.StepC <= FanCurves.MaxC) { c.Add(c[n - 1] + FanCurves.StepC); pct.Add(FanCurves.FullPct); }
            else { c.Insert(n - 1, (c[n - 2] + c[n - 1]) / 2); pct.Insert(n - 1, (pct[n - 2] + pct[n - 1]) / 2); }
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

        void ApplyFan()
        {
            var f = FanNow;
            string choice = FanChoiceShown(f);
            if (choice == "board") { RunAction("fan-auto", null, null, null, false, ok => ClearFanEdit()); return; }
            var more = new Recovery.PlanArgs { FanProfile = choice };
            if (choice == "custom")
            {
                uint[] c, pct;
                FanCurveShown(f, choice, out c, out pct);
                more.FanCurve = FanCurves.CurveText(c, pct);
            }
            RunAction("fan-curve", more, null, null, false, ok => ClearFanEdit());
        }

        void ClearFanEdit()
        {
            _fanChoice = null; _fanEditC = null; _fanEditPct = null;
            ShowPage(_page, null, false);
        }

        // The live part: the speed and who runs the fan every poll; a change of the choice or the state rebuilds the card.
        void TickFan()
        {
            if (_page != "performance" || _snap == null || _fixture) return;
            var fan = Kmd.Fan();
            _snap.Fan = fan.Value;
            if (FanSignature(_snap.Fan) != _fanState) { ShowPage(_page, null, false); return; }
            var f = _snap.Fan;
            Label l;
            if (f != null && _live.TryGetValue("perf.fan.speed", out l))
                l.Text = f.Rpm != 0 ? Strings.T("perf.fan.rpm-only", f.Rpm) : Strings.T("perf.no-reading");
            if (f != null && _live.TryGetValue("perf.fan.who", out l)) l.Text = FanCurves.StateText(f);
        }
    }
}
