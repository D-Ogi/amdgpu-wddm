// Home, Display, Performance, Driver and Settings. Plain words only (R1): versions of releases, dates and settings,
// never driver internals; the support view on Help has the technical states.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Globalization;
using System.Linq;
using System.Windows.Forms;

namespace AmdgpuWddmControl
{
    public sealed partial class MainForm
    {
        static string When(DateTime utc) { return utc.ToLocalTime().ToString("yyyy-MM-dd HH:mm", CultureInfo.InvariantCulture); }

        Color SeverityColor(string severity)
        {
            return severity == "problem" ? Theme.Accent : severity == "attention" ? Theme.Warn : severity == "ok" ? Theme.Good : Theme.Text;
        }

        // "Coming later" (WU-029): the state, the reason in plain words, the way to get it.
        Control LaterRow(string id, int width)
        {
            var name = Ui.Label(Strings.T("search." + id), Theme.Bold, null, width);
            var state = Ui.Label(Strings.T("ui.later.reason"), null, Theme.Dim, width);
            state.Margin = Theme.Pad(0, 0, 0, 8);
            var block = Ui.Stack(width);
            block.Controls.Add(name); block.Controls.Add(state);
            block.AccessibleName = Strings.T("search." + id) + ": " + Strings.T("ui.later");
            Mark(id, name);
            return block;
        }

        CardPanel LaterCard(int width, params string[] ids)
        {
            var c = new CardPanel(Strings.T("ui.later.title"), width);
            foreach (var id in ids) c.Add(LaterRow(id, c.Inner));
            return c;
        }

        static string ClockText(uint? mode, uint maxMHz)
        {
            return mode == 1 ? Strings.T("graphics.clock.auto-up-to", maxMHz) : Strings.T("graphics.clock.fixed");
        }

        string ClockNow()
        {
            var d = _snap.Dpm;
            if (d == null) return Strings.T("perf.no-reading");
            return d.Mode == 1 ? Strings.T("graphics.clock.auto-up-to", d.MaxMHz) : d.Mode == 0 ? Strings.T("graphics.clock.fixed") : Strings.T("perf.no-reading");
        }

        // ---- Home (WU-007) -------------------------------------------------------------------------------------------

        Control BuildHome(int width)
        {
            var p = Frame("home", width);
            if (_status == null) ComputeStatus();
            var status = new CardPanel(_status.Title, width);
            status.Body.Controls[0].ForeColor = SeverityColor(_status.Severity);
            Mark("home.status", status);
            if (_status.Items.Count == 0) status.Add(Ui.Label(Strings.T("status.ok-detail"), null, null, status.Inner));
            foreach (var item in _status.Items)
            {
                status.Add(Ui.Label(item.Text, Theme.Bold, SeverityColor(item.Severity), status.Inner));
                if (item.Next != null) status.Add(Ui.Dim(item.Next, status.Inner));
                if (item.Action != null)
                {
                    var it = item;
                    status.Add(Ui.Button(it.ActionLabel, (s, e) => StatusAction(it.Action), it.Severity == "problem" || it.Severity == "attention"));
                }
            }
            var result = ResultLine(status.Inner);
            if (result != null) status.Add(result);
            p.Controls.Add(status);

            if (!_prefs.GettingStartedDismissed)
            {
                var start = new CardPanel(Strings.T("home.start.title"), width);
                for (int i = 1; i <= 3; i++) start.Add(Ui.Label((i) + ".  " + Strings.T("home.start." + i), null, null, start.Inner));
                start.Add(Ui.Button(Strings.T("home.start.dismiss"), (s, e) => { _prefs.GettingStartedDismissed = true; ShowPage(_page, null, false); }));
                p.Controls.Add(start);
            }

            var recent = new CardPanel(Strings.T("home.recent.title"), width);
            var hidden = _prefs.HiddenGames;
            var shown = _recent.Where(r => !hidden.Contains(r.Image)).Take(4).ToList();
            if (shown.Count == 0) recent.Add(Ui.Dim(Strings.T(_prefs.RecordRecentLaunches ? "home.recent.none" : "home.recent.off"), recent.Inner));
            foreach (var r in shown)
            {
                var game = r;
                var name = Ui.Label(game.Image, Theme.Bold, null, recent.Inner / 2);
                name.MinimumSize = new Size(recent.Inner / 2, 0);
                var when = Ui.Dim(Strings.T("games.launched", When(game.LastLaunchUtc), Strings.T(game.D3D12 ? "games.api.d3d12" : "games.api.d3d11")), recent.Inner / 2 - Theme.S(110));
                var open = Ui.Button(Strings.T("home.recent.settings"), (s, e) => { _game = game.Image; Navigate("games"); });
                open.AccessibleName = Strings.T("home.recent.settings") + ": " + game.Image;
                recent.Add(Ui.WrapRow(recent.Inner, name, when, open));
            }
            recent.Add(Ui.Button(Strings.T("home.recent.all"), (s, e) => Navigate("games")));
            p.Controls.Add(recent);

            var screen = new CardPanel(Strings.T("home.monitor.title"), width);
            var modes = DisplayInfo.Current();
            var main = modes.FirstOrDefault();
            screen.Add(Ui.Label(main == null ? Strings.T("perf.no-reading") : ModeText(main), Theme.Big, null, screen.Inner));
            screen.Add(Ui.Button(Strings.T("home.monitor.open"), (s, e) => Navigate("display")));
            p.Controls.Add(screen);

            var drv = new CardPanel(Strings.T("home.driver.title"), width);
            drv.Add(Ui.Label(_drv != null ? _drv.RunningText : Strings.T("drv.running.unknown"), null, null, drv.Inner));
            drv.Add(Ui.Dim(UpdateCheck.CardText(_upd, InstalledVersion, UpdateCheck.Running), drv.Inner));
            drv.Add(Ui.Button(Strings.T("status.action.driver"), (s, e) => Navigate("driver")));
            p.Controls.Add(drv);

            var gfx = new CardPanel(Strings.T("home.graphics.title"), width);
            gfx.Pair(Strings.T("graphics.clock.now"), ClockNow());
            var cu = CuMode.View(_snap.Cu, _snap.Health != null ? (ulong?)_snap.Health.Generation : null, _snap.StoredCu(), _cuConfirmFailed);
            gfx.Pair(Strings.T("graphics.cores.title"), _snap.DriverInstalled ? cu.Running : Strings.T("perf.no-reading"));
            gfx.Add(Ui.Button(Strings.T("status.action.graphics"), (s, e) => Navigate("graphics")));
            p.Controls.Add(gfx);
            return p;
        }

        void StatusAction(string action)
        {
            if (action == "restart") { OfferRestartNow(); return; }
            if (action == "repair") { Navigate("help", "help.repair"); return; }
            if (action.StartsWith("page:", StringComparison.Ordinal)) { Navigate(action.Substring(5)); return; }
            RunAction(action);
        }

        void OfferRestartNow()
        {
            if (RestartDialog.Ask(this, Hints.Decide(Hints.Read(_recent)))) RestartWindows();
            else { _laterRestart = true; ComputeStatus(); ShowPage(_page, null, false); }
        }

        static string ModeText(DisplayMode m)
        {
            return m.RefreshHz > 1 ? Strings.T("display.mode", m.Width, m.Height, m.RefreshHz) : Strings.T("display.mode.no-rate", m.Width, m.Height);
        }

        // ---- Display (WU-030, WU-031, WU-034 links) -----------------------------------------------------------------

        Control BuildDisplay(int width)
        {
            var p = Frame("display", width);
            var modes = DisplayInfo.Current();
            int n = 0;
            foreach (var m in modes)
            {
                n++;
                string title = Strings.T(m.Primary ? "display.name.main" : "display.name", n);
                var c = new CardPanel(title, width);
                if (n == 1) Mark("display.resolution", c);
                AddDisplayChoices(c, m, n, title);
                c.Add(Ui.Dim(Strings.T("display.mode-note"), c.Inner));
                var screen = m.Bounds;
                int number = n;
                var identify = Ui.Button(Strings.T("display.identify"), (s, e) => Identify(screen, number));
                identify.AccessibleName = Strings.T("display.identify") + " " + number;
                if (n == 1) Mark("display.identify", identify);
                c.Add(Ui.Row(identify, Explain("identify", Strings.T("display.identify"))));
                p.Controls.Add(c);
            }
            if (modes.Count == 0) p.Controls.Add(Ui.Dim(Strings.T("perf.no-reading"), width));
            var win = new CardPanel(Strings.T("display.windows.title"), width);
            win.Add(Ui.Dim(Strings.T("display.windows.text"), win.Inner));
            var open = Ui.Button(Strings.T("display.windows.open"), (s, e) => OpenWindowsSettings(DisplayInfo.WindowsDisplaySettings));
            Mark("display.scale", open);
            win.Add(Ui.Row(open, Explain("scale", Strings.T("display.windows.open"))));
            p.Controls.Add(win);
            p.Controls.Add(LaterCard(width, "later.hdr", "later.vrr"));
            return p;
        }

        void Identify(Rectangle screen, int number)
        {
            if (_smoke) return;
            var f = new IdentifyForm(screen, number.ToString(CultureInfo.InvariantCulture));
            f.Show();
        }

        static void OpenWindowsSettings(string uri)
        {
            if (!DisplayInfo.AllowedLinks.Contains(uri)) return;
            try { using (Process.Start(new ProcessStartInfo(uri) { UseShellExecute = true })) { } } catch (Exception) { }
        }

        // ---- Performance (WU-038, WU-041) -------------------------------------------------------------------------

        Control BuildPerformance(int width)
        {
            var p = Frame("performance", width);
            var now = new CardPanel(Strings.T("perf.now.title"), width);
            Mark("perf.sensors", now);
            foreach (var row in Sensors.Rows(_snap.Dpm, _vram, _fan, _snap.Fan))
            {
                var r = row;
                int keyWidth = Math.Min(Theme.S(210), now.Inner / 3);
                var k = Ui.Label(r.Label, null, Theme.Dim, keyWidth);
                k.MinimumSize = new Size(keyWidth, 0);
                var v = Ui.Label(r.Value, Theme.Bold, r.Level == "hot" ? Theme.Accent : r.Level == "warn" ? Theme.Warn : r.NoReading ? Theme.Dim : Theme.Text, now.Inner - keyWidth - Theme.S(48));
                v.MinimumSize = new Size(Theme.S(120), 0);
                _live["perf." + r.Id] = v;
                now.Add(Ui.Row(k, v, Ui.Explain(r.Label, () => ShowExplainText(r.Label, r.Explain))));
            }
            now.Add(Ui.Dim(Strings.T("perf.now.note"), now.Inner));
            p.Controls.Add(now);
            p.Controls.Add(BuildFanCard(width));

            var caches = new CardPanel(Strings.T("perf.cache.title"), width);
            Mark("perf.cache", caches);
            foreach (var c in CacheInventory.Read(CacheInventory.D3D12Directory())) caches.Pair(c.Name, c.Text);
            caches.Add(Ui.Dim(Strings.T("perf.cache.note"), caches.Inner));
            p.Controls.Add(caches);
            p.Controls.Add(LaterCard(width, "later.power", "later.cache-clear"));
            return p;
        }

        void ShowExplainText(string title, string text)
        {
            if (_explainTitle == null) return;
            _explainTitle.Text = title; _explainText.Text = text;
            if (!SideShown && !_smoke) MessageBox.Show(this, text, title, MessageBoxButtons.OK, MessageBoxIcon.None);
        }

        void RefreshLiveLabels()
        {
            if (_page == "performance")
                foreach (var row in Sensors.Rows(_snap.Dpm, _vram, _fan, _snap.Fan))
                {
                    Label l;
                    if (!_live.TryGetValue("perf." + row.Id, out l)) continue;
                    l.Text = row.Value;
                    l.ForeColor = row.Level == "hot" ? Theme.Accent : row.Level == "warn" ? Theme.Warn : row.NoReading ? Theme.Dim : Theme.Text;
                }
        }

        // ---- Driver (WU-043, WU-046) ---------------------------------------------------------------------------------

        Control BuildDriver(int width)
        {
            var p = Frame("driver", width);
            var card = new CardPanel(Strings.T("driver.card.title"), width);
            Mark("driver.version", card);
            var d = _drv;
            card.Add(Ui.Label(d != null ? d.RunningText : Strings.T("drv.running.unknown"), Theme.Bold, null, card.Inner));
            card.Add(Ui.Label(d != null ? d.InstalledText : Strings.T("drv.installed.none"), null, d != null && d.InstalledPending ? Theme.Warn : (Color?)null, card.Inner));
            card.Add(Ui.Label(d != null ? d.VerificationText : Strings.T("drv.verify.unknown"), null, d != null && d.Verification == VerifyKind.Failed ? Theme.Warn : (Color?)null, card.Inner));
            card.Add(Ui.Dim(d != null ? d.DriverDateText : Strings.T("drv.date.none"), card.Inner));
            var released = UpdateCheck.InstalledReleased(_upd, InstalledVersion);
            card.Add(Ui.Dim(released != null ? Strings.T("drv.released", released) : Strings.T("drv.released.none"), card.Inner));
            if (d != null && d.InstalledPending) card.Add(Ui.Button(Strings.T("status.action.restart"), (s, e) => OfferRestartNow(), true));
            p.Controls.Add(card);

            var upd = new CardPanel(Strings.T("driver.updates.title"), width);
            bool running = UpdateCheck.Running;
            upd.Add(Ui.Label(UpdateCheck.CardText(_upd, InstalledVersion, running), null, null, upd.Inner));
            var check = Ui.Button(Strings.T("driver.updates.check"), (s, e) => StartUpdateCheck(true));
            check.Enabled = !running;
            Mark("driver.update", check);
            var buttons = Ui.WrapRow(upd.Inner, check);
            if (UpdateCheck.IsCandidateNewer(_upd, InstalledVersion) || (_upd != null && _upd.CandidateTag != null && InstalledVersion == null))
            {
                string tag = _upd.CandidateTag;
                buttons.Controls.Add(Ui.Button(Strings.T("driver.updates.open"), (s, e) => UpdateCheck.OpenReleasePage(tag), true));
            }
            else buttons.Controls.Add(Ui.Button(Strings.T("driver.updates.releases"), (s, e) => UpdateCheck.OpenReleasePage(null)));
            upd.Add(buttons);
            upd.Add(Ui.Dim(Strings.T("driver.updates.privacy"), upd.Inner));
            p.Controls.Add(upd);

            var repair = new CardPanel(Strings.T("driver.repair.title"), width);
            repair.Add(Ui.Dim(Strings.T("driver.repair.text"), repair.Inner));
            repair.Add(Ui.Button(Strings.T("driver.repair.open"), (s, e) => Navigate("help", "help.repair")));
            p.Controls.Add(repair);
            return p;
        }

        // ---- Settings (WU-003, WU-046, WU-075, WU-076, WU-077) --------------------------------------------------------

        Control BuildSettings(int width)
        {
            var p = Frame("settings", width);
            var lang = new CardPanel(Strings.T("search.settings.language"), width);
            Mark("settings.language", lang);
            var radios = Ui.WrapRow(lang.Inner);
            foreach (var l in Strings.Languages)
            {
                string code = l;
                var r = Ui.Radio(LanguageName(l));
                r.Checked = l == Strings.Language;
                r.CheckedChanged += (s, e) => { if (r.Checked) BeginInvoke((Action)(() => SetLanguage(code))); };
                radios.Controls.Add(r);
            }
            radios.Controls.Add(Explain("language", Strings.T("search.settings.language")));
            lang.Add(radios);
            lang.Add(Ui.Dim(Strings.T("settings.language.note"), lang.Inner));
            p.Controls.Add(lang);

            var upd = new CardPanel(Strings.T("settings.updates.title"), width);
            upd.Add(PrefCheck("settings.update-start", "update-start", _prefs.UpdateCheckAtStart, v => _prefs.UpdateCheckAtStart = v, upd.Inner));
            upd.Add(Ui.Dim(Strings.T("settings.update-start.note"), upd.Inner));
            p.Controls.Add(upd);

            var rec = new CardPanel(Strings.T("settings.recent.title"), width);
            var invalid = _prefs.RecentLaunchesSwitch == RecentSwitch.Invalid;
            var switchNote = Ui.Dim(invalid ? Strings.T("settings.recent.invalid") : "", rec.Inner);
            rec.Add(PrefCheck("settings.recent", "recent", _prefs.RecordRecentLaunches, v => { _prefs.RecordRecentLaunches = v; switchNote.Text = ""; }, rec.Inner));
            if (invalid) rec.Add(switchNote);
            rec.Add(Ui.Dim(Strings.T("settings.recent.note"), rec.Inner));
            if (_recentState == RecentListState.Invalid) rec.Add(Ui.Dim(Strings.T("settings.recent.unreadable"), rec.Inner));
            var clearResult = Ui.Dim("", rec.Inner);
            Button clear = null;
            clear = Ui.Button(Strings.T("settings.recent.clear"), (s, e) =>
            {
                // The lock may take up to 2 s (docs/design/recent-launches.md): off the UI thread.
                clear.Enabled = false;
                clearResult.Text = "";
                System.Threading.ThreadPool.QueueUserWorkItem(_ =>
                {
                    var r = RecentLaunches.Clear(new LocalRecentFiles());
                    var list = RecentLaunches.Read(new LocalRecentFiles());
                    try
                    {
                        BeginInvoke((Action)(() =>
                        {
                            _recent = list.Entries; _recentState = list.State;
                            clearResult.Text = Strings.T(r == ClearResult.Cleared ? "settings.recent.cleared" : r == ClearResult.Busy ? "settings.recent.busy" : "settings.recent.failed");
                            clear.Enabled = true;
                        }));
                    }
                    catch (InvalidOperationException) { }
                });
            });
            rec.Add(clear);
            rec.Add(clearResult);
            p.Controls.Add(rec);

            var guide = new CardPanel(Strings.T("settings.guide.title"), width);
            guide.Add(PrefCheck("settings.nagi", "nagi", _prefs.ShowNagi, v => { _prefs.ShowNagi = v; UpdateGuide(); }, guide.Inner));
            guide.Add(PrefCheck("settings.tips", "tips", _prefs.ShowTipsAutomatically, v => { _prefs.ShowTipsAutomatically = v; UpdateGuide(); }, guide.Inner));
            guide.Add(PrefCheck("settings.animations", "animations", _prefs.ReduceAnimations, v => _prefs.ReduceAnimations = v, guide.Inner));
            if (AppPrefs.WindowsAnimations() == false) guide.Add(Ui.Dim(Strings.T("settings.animations.windows-off"), guide.Inner));
            guide.Add(Ui.Dim(Strings.T("settings.guide.note"), guide.Inner));
            p.Controls.Add(guide);

            var support = new CardPanel(Strings.T("settings.support.title"), width);
            support.Add(PrefCheck("settings.support", "support", _prefs.ShowSupportOptions, v => _prefs.ShowSupportOptions = v, support.Inner));
            support.Add(Ui.Dim(Strings.T("settings.support.note"), support.Inner));
            p.Controls.Add(support);

            var data = new CardPanel(Strings.T("search.settings.data"), width);
            Mark("settings.data", data);
            for (int i = 1; i <= 5; i++) data.Add(Ui.Label("•  " + Strings.T("settings.data." + i), null, null, data.Inner));
            p.Controls.Add(data);
            return p;
        }

        // An app preference: a check box written at once (HKCU app state, R4), with its "?".
        Control PrefCheck(string anchor, string setting, bool value, Action<bool> set, int width)
        {
            var c = Ui.Check(Strings.T("search." + anchor), width - Theme.S(40));
            c.Checked = value;
            c.CheckedChanged += (s, e) => set(c.Checked);
            Mark(anchor, c);
            return Ui.Row(c, Explain(setting, c.Text));
        }
    }
}
