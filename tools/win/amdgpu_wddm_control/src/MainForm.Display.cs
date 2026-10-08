// The Display page's choices (WU-031): resolution, refresh rate and GPU scaling of each monitor, from the modes
// Windows lists. Edits stay in the window until Apply; Apply changes the monitor at once and asks to keep the new
// settings (KeepDisplayDialog, 15 s), as Windows does. DisplayModes.cs makes the change.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Linq;
using System.Windows.Forms;

namespace AmdgpuWddmControl
{
    public sealed partial class MainForm
    {
        // One monitor's choice: the whole mode, and the scaling (null: as it is).
        sealed class DisplayEdit
        {
            public int Width, Height, RefreshHz;
            public string Scaling;
        }

        readonly Dictionary<string, DisplayEdit> _displayEdits = new Dictionary<string, DisplayEdit>(StringComparer.OrdinalIgnoreCase);

        string ScalingNow(string device)
        {
            var raw = DisplayModes.ReadScaling(device);
            return raw == null ? null : DisplayInfo.ScalingOf(raw.Value);
        }

        static bool ModeChanged(DisplayMode m, DisplayEdit e) { return e != null && (e.Width != m.Width || e.Height != m.Height || e.RefreshHz != m.RefreshHz); }

        static bool ScalingChanged(DisplayEdit e, string now) { return e != null && e.Scaling != null && now != null && e.Scaling != now; }

        bool DisplayDirty()
        {
            if (_displayEdits.Count == 0) return false;
            foreach (var m in DisplayInfo.Current())
            {
                DisplayEdit e;
                if (_displayEdits.TryGetValue(m.Device, out e) && (ModeChanged(m, e) || ScalingChanged(e, ScalingNow(m.Device)))) return true;
            }
            return false;
        }

        DisplayEdit EditOf(DisplayMode m)
        {
            DisplayEdit e;
            if (!_displayEdits.TryGetValue(m.Device, out e)) _displayEdits[m.Device] = e = new DisplayEdit { Width = m.Width, Height = m.Height, RefreshHz = m.RefreshHz };
            return e;
        }

        // An edit that changes nothing goes away.
        void Settle(DisplayMode m, string scalingNow)
        {
            DisplayEdit e;
            if (_displayEdits.TryGetValue(m.Device, out e) && !ModeChanged(m, e) && !ScalingChanged(e, scalingNow)) _displayEdits.Remove(m.Device);
        }

        // The rows of one monitor's card: resolution, refresh rate, scaling, and Apply / Discard for this monitor.
        void AddDisplayChoices(CardPanel c, DisplayMode m, int number, string title)
        {
            var modes = DisplayInfo.Modes(m.Device);
            string scalingNow = ScalingNow(m.Device);
            DisplayEdit edit;
            _displayEdits.TryGetValue(m.Device, out edit);
            int w = edit != null ? edit.Width : m.Width, h = edit != null ? edit.Height : m.Height, hz = edit != null ? edit.RefreshHz : m.RefreshHz;
            int max = ChoiceMax(c.Inner);
            if (modes.Count == 0)
            {
                c.Pair(Strings.T("display.resolution"), Strings.T("display.size", m.Width, m.Height));
                c.Pair(Strings.T("display.refresh"), m.RefreshHz > 1 ? Strings.T("display.rate", m.RefreshHz) : Strings.T("perf.no-reading"));
                c.Add(Ui.Dim(Strings.T("display.modes.none"), c.Inner));
            }
            else
            {
                var sizes = DisplayInfo.Resolutions(modes);
                if (!sizes.Contains(new Size(w, h))) sizes.Insert(0, new Size(w, h));
                var res = Choices(sizes.Select(s => Strings.T("display.size", s.Width, s.Height)).ToList(), Strings.T("display.resolution") + ": " + title, max);
                res.SelectedIndex = sizes.IndexOf(new Size(w, h));
                res.SelectionChangeCommitted += (o, e) =>
                {
                    if (res.SelectedIndex < 0) return;
                    var pick = sizes[res.SelectedIndex];
                    var d = EditOf(m);
                    d.Width = pick.Width; d.Height = pick.Height;
                    d.RefreshHz = DisplayInfo.RateFor(modes, pick.Width, pick.Height, d.RefreshHz);
                    Settle(m, scalingNow);
                    BeginInvoke((Action)(() => ShowPage(_page, null, false)));
                };
                if (number == 1) Mark("display.resolution", res);
                c.Add(Ui.WrapRow(c.Inner, RowLabel(Strings.T("display.resolution"), c.Inner), res, Explain("resolution", Strings.T("search.display.resolution"))));

                var rates = DisplayInfo.Rates(modes, w, h);
                if (!rates.Contains(hz)) rates.Insert(0, hz);
                var rate = Choices(rates.Select(r => r > 1 ? Strings.T("display.rate", r) : Strings.T("perf.no-reading")).ToList(), Strings.T("display.refresh") + ": " + title, max);
                rate.SelectedIndex = rates.IndexOf(hz);
                rate.SelectionChangeCommitted += (o, e) =>
                {
                    if (rate.SelectedIndex < 0) return;
                    var d = EditOf(m);
                    d.Width = w; d.Height = h; d.RefreshHz = rates[rate.SelectedIndex];
                    Settle(m, scalingNow);
                    BeginInvoke((Action)(() => ShowPage(_page, null, false)));
                };
                c.Add(Ui.WrapRow(c.Inner, RowLabel(Strings.T("display.refresh"), c.Inner), rate));
            }

            if (scalingNow == null) c.Add(Ui.Dim(Strings.T("display.scaling.unknown"), c.Inner));
            else
            {
                var values = DisplayInfo.ScalingChoices.ToList();
                var texts = values.Select(v => Strings.T("display.scaling." + v)).ToList();
                if (scalingNow == "windows") { values.Add("windows"); texts.Add(Strings.T("display.scaling.windows")); }
                string shown = edit != null && edit.Scaling != null ? edit.Scaling : scalingNow;
                var scale = Choices(texts, Strings.T("search.display.scaling") + ": " + title, max);
                scale.SelectedIndex = Math.Max(0, values.IndexOf(shown));
                scale.SelectionChangeCommitted += (o, e) =>
                {
                    if (scale.SelectedIndex < 0) return;
                    var pick = values[scale.SelectedIndex];
                    var d = EditOf(m);
                    d.Scaling = pick == "windows" || pick == scalingNow ? null : pick;
                    Settle(m, scalingNow);
                    BeginInvoke((Action)(() => ShowPage(_page, null, false)));
                };
                if (number == 1) Mark("display.scaling", scale);
                c.Add(Ui.WrapRow(c.Inner, RowLabel(Strings.T("search.display.scaling"), c.Inner), scale, Explain("scaling", Strings.T("search.display.scaling"))));
                c.Add(RowDetail(Strings.T("display.scaling.note"), c.Inner));
            }

            bool changed = ModeChanged(m, edit) || ScalingChanged(edit, scalingNow);
            if (changed)
            {
                c.Add(Ui.Label(Strings.T("display.changed", ModeText(m)), null, Theme.Warn, c.Inner));
                var apply = Ui.Button(Strings.T("ui.apply"), (o, e) => ApplyDisplay(m.Device), true);
                apply.AccessibleName = Strings.T("ui.apply") + ": " + title;
                var discard = Ui.Button(Strings.T("ui.discard"), (o, e) => { _displayEdits.Remove(m.Device); ShowPage(_page, null, false); });
                discard.AccessibleName = Strings.T("ui.discard") + ": " + title;
                c.Add(Ui.WrapRow(c.Inner, apply, discard));
            }
        }

        // Applies the choices of one monitor (device) or of every monitor (null), one monitor at a time, each with
        // its own "keep these settings?" question. False when a change was refused or taken back.
        bool ApplyDisplay(string device = null)
        {
            if (_smoke) return false;
            bool all = true;
            foreach (var m in DisplayInfo.Current())
            {
                if (device != null && !m.Device.Equals(device, StringComparison.OrdinalIgnoreCase)) continue;
                DisplayEdit e;
                if (!_displayEdits.TryGetValue(m.Device, out e)) continue;
                string now = ScalingNow(m.Device);
                bool mode = ModeChanged(m, e), scaling = ScalingChanged(e, now);
                _displayEdits.Remove(m.Device);
                if (!mode && !scaling) continue;
                string error;
                var change = DisplayModes.Apply(m.Device, mode ? new DisplayChoice { Width = e.Width, Height = e.Height, RefreshHz = e.RefreshHz } : null,
                    scaling ? (uint?)DisplayInfo.ScalingValue(e.Scaling) : null, out error);
                if (change == null) { Result(Strings.T("display.result.failed"), false); all = false; break; }
                bool keep = KeepDisplayDialog.Ask(this);
                string after = keep ? DisplayModes.Keep(change) : DisplayModes.Revert(change);
                if (keep && after == null) { Result(Strings.T("display.result.kept"), true); continue; }
                Result(Strings.T(keep ? "display.result.keep-failed" : after == null ? "display.result.reverted" : "display.result.revert-failed"), false);
                all = false;
                break;
            }
            ShowPage(_page, null, false);
            return all;
        }
    }
}
