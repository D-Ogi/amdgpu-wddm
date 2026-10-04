// Help (WU-058, WU-059, WU-063, WU-067): four symptom guides, Getting started, Repair, Restart Windows, the support
// report and About. With "Show support options" the support view follows: the technical states, components and
// actions in English, for the people who fix problems (Tag "support": the plain-words gate G-NOINT skips it).
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Linq;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace AmdgpuWddmControl
{
    public sealed partial class MainForm
    {
        string _openGuide;
        bool _reportDxdiag = true, _reportCaps = true;
        string _reportStatus;
        bool _reportOk;

        Control BuildHelp(int width)
        {
            var p = Frame("help", width);

            var guides = new CardPanel(Strings.T("help.guides.title"), width);
            Mark("help.guides", guides);
            foreach (var id in HelpGuides.Ids)
            {
                string g = id;
                bool open = _openGuide == g;
                var b = Ui.Button((open ? "▾  " : "▸  ") + Strings.T("help.guide." + g), (s, e) => { _openGuide = open ? null : g; ShowPage(_page, "help.guide." + g, true); });
                b.AccessibleName = Strings.T("help.guide." + g);
                b.AccessibleDescription = Strings.T(open ? "ui.expanded" : "ui.collapsed");
                b.MaximumSize = new Size(guides.Inner, 0);
                b.TextAlign = ContentAlignment.MiddleLeft;
                Mark("help.guide." + g, b);
                guides.Add(b);
                if (!open) continue;
                int n = 0;
                foreach (var step in HelpGuides.Steps(g))
                {
                    var l = Ui.Label(++n + ".  " + step, null, null, guides.Inner - Theme.S(20));
                    l.Margin = Theme.Pad(20, 2, 0, 4);
                    guides.Add(l);
                }
            }
            p.Controls.Add(guides);

            var start = new CardPanel(Strings.T("home.start.title"), width);
            start.Add(Ui.Dim(Strings.T("help.start.text"), start.Inner));
            start.Add(Ui.Button(Strings.T("help.start.show"), (s, e) => { _prefs.GettingStartedDismissed = false; Navigate("home"); }));
            p.Controls.Add(start);

            var repair = new CardPanel(Strings.T("search.help.repair"), width);
            Mark("help.repair", repair);
            repair.Add(Ui.Dim(Strings.T("help.repair.text"), repair.Inner));
            bool canRepair = HelpGuides.RepairSetup() != null;
            if (canRepair)
                repair.Add(Ui.Button(Strings.T("status.action.repair"), (s, e) =>
                {
                    if (!_smoke && MessageBox.Show(this, Strings.T("help.repair.confirm"), Strings.T("search.help.repair"), MessageBoxButtons.OKCancel, MessageBoxIcon.None) == DialogResult.OK)
                        Result(Strings.T(HelpGuides.StartRepair() ? "help.repair.started" : "help.repair.failed"), true);
                }, true));
            else repair.Add(Ui.Label(Strings.T("help.repair.unavailable"), null, Theme.Warn, repair.Inner));
            p.Controls.Add(repair);

            var restart = new CardPanel(Strings.T("search.help.restart"), width);
            Mark("help.restart", restart);
            restart.Add(Ui.Dim(Strings.T("help.restart.text"), restart.Inner));
            restart.Add(Ui.Button(Strings.T("status.action.restart"), (s, e) => OfferRestartNow()));
            p.Controls.Add(restart);

            var report = new CardPanel(Strings.T("search.help.report"), width);
            Mark("help.report", report);
            report.Add(Ui.Dim(Strings.T("help.report.text"), report.Inner));
            for (int i = 1; i <= 5; i++) report.Add(Ui.Label("•  " + Strings.T("help.report.item." + i), null, null, report.Inner));
            report.Add(Ui.Dim(Strings.T("help.report.privacy"), report.Inner));
            var dx = Ui.Check(Strings.T("help.report.dxdiag"), report.Inner);
            dx.Checked = _reportDxdiag; dx.CheckedChanged += (s, e) => _reportDxdiag = dx.Checked;
            var caps = Ui.Check(Strings.T("help.report.caps"), report.Inner);
            caps.Checked = _reportCaps; caps.CheckedChanged += (s, e) => _reportCaps = caps.Checked;
            report.Add(dx); report.Add(caps);
            var create = Ui.Button(Strings.T("help.report.create"), (s, e) => CreateReport(), true);
            create.Enabled = !_work;
            report.Add(create);
            if (_reportStatus != null) report.Add(Ui.Label(_reportStatus, null, _reportOk ? Theme.Good : Theme.Dim, report.Inner));
            p.Controls.Add(report);

            var result = ResultLine(width);
            if (result != null) p.Controls.Add(result);

            var about = new CardPanel(Strings.T("help.about.title"), width);
            about.Add(Ui.Label(Program.ProductName, Theme.Bold, null, about.Inner));
            about.Add(Ui.Dim(InstalledVersion != null ? Strings.T("help.about.release", InstalledVersion) : Strings.T("drv.installed.none"), about.Inner));
            about.Add(Ui.Dim(Strings.T("help.about.text"), about.Inner));
            about.Add(Ui.Button(Strings.T("driver.updates.releases"), (s, e) => UpdateCheck.OpenReleasePage(null)));
            p.Controls.Add(about);

            if (_prefs.ShowSupportOptions) p.Controls.Add(BuildSupport(width));
            return p;
        }

        void CreateReport()
        {
            if (_smoke || _work) return;
            _work = true; ComputeStatus();
            _reportStatus = Strings.T("help.report.collecting"); _reportOk = false;
            ShowPage(_page, null, false);
            var report = new BugReport(Redactor.ForThisPc());
            bool dxdiag = _reportDxdiag, caps = _reportCaps;
            Task.Run(() => report.Collect(dxdiag, caps, true, _ => { })).ContinueWith(t =>
            {
                _work = false; ComputeStatus();
                if (t.IsFaulted) { _reportStatus = Strings.T("help.report.failed"); ShowPage(_page, null, false); return; }
                var path = BugReport.DefaultPath(DateTime.Now);
                using (var preview = new ReportPreview(report, path))
                {
                    if (preview.ShowDialog(this) != DialogResult.OK) { _reportStatus = Strings.T("help.report.cancelled"); ShowPage(_page, null, false); return; }
                }
                try { report.Write(path); _reportStatus = Strings.T("help.report.saved", path); _reportOk = true; }
                catch (Exception) { _reportStatus = Strings.T("help.report.not-saved"); }
                ShowPage(_page, null, false);
            }, TaskScheduler.FromCurrentSynchronizationContext());
        }

        // ---- the support view (D5): technical, English, off by default ------------------------------------------------

        Control BuildSupport(int width)
        {
            var p = Ui.Stack(width);
            p.Tag = "support";
            var states = new CardPanel("Support: driver states", width) { Tag = "support" };
            states.Add(Ui.Dim("Technical detail for the people who fix problems. It is in English, like the support report.", states.Inner));
            List<StateLine> lines;
            try { lines = Recovery.Describe(_snap); } catch (Exception e) { lines = new List<StateLine> { new StateLine { Topic = "states", Text = "cannot describe: " + e.Message, Severity = "warn" } }; }
            foreach (var l in lines)
            {
                states.Add(Ui.Label(l.Topic, Theme.Bold, null, states.Inner));
                states.Add(Ui.Label(l.Text, Theme.Small, l.Severity == "warn" ? Theme.Warn : l.Severity == "ok" ? Theme.Good : Theme.Text, states.Inner));
            }
            if (_drv != null && _drv.ReportText != null) states.Add(Ui.Label(_drv.ReportText, Theme.MonoSmall, Theme.Dim, states.Inner));
            p.Controls.Add(states);

            var actions = new CardPanel("Support: actions", width) { Tag = "support" };
            actions.Add(Ui.Dim("Each action shows its plan first and asks for administrator permission. Most take effect at the next restart of Windows.", actions.Inner));
            Func<string, string, Button> act = (label, action) => Ui.Button(label, (s, e) => RunAction(action));
            actions.Add(Ui.WrapRow(actions.Inner, act("Desktop on the CPU route", "desktop-cpu"), act("Desktop on the GPU route", "desktop-gpu"), act("Reopen the GPU desktop path", "reopen-gpu-path")));
            actions.Add(Ui.WrapRow(actions.Inner, act("Confirm this start", "confirm-start"), act("Re-enable automatic clocks", "enable-dpm"), act("Undo last action", "undo")));
            p.Controls.Add(actions);

            var comps = new CardPanel("Support: installed components", width) { Tag = "support" };
            comps.Add(Ui.Label("Adapter: " + _inv.AdapterName + "; INF " + _inv.DriverVersion + " (" + _inv.DriverDate + "); release " + _inv.ReleaseVersion, Theme.Small, null, comps.Inner));
            foreach (var c in _inv.Components)
                comps.Add(Ui.Label(c.Role + ": " + (c.Exists ? c.Version : "missing") + "  " + c.Path, Theme.MonoSmall, c.Exists ? Theme.Dim : Theme.Warn, comps.Inner));
            foreach (var n in _inv.Notes) comps.Add(Ui.Label(n, Theme.Small, Theme.Warn, comps.Inner));
            p.Controls.Add(comps);
            return p;
        }
    }
}
