// The recovery view (WU-068, C14): "amdgpu_wddm_control.exe --recovery", from its own Start-menu shortcut. It works
// when the rest of the window cannot: it never loads bc250control.dll (Kmd.Blocked) and asks the driver nothing.
// It shows the installed release from the registry, the plain guides (with the Safe Mode guide), Repair, Restart
// Windows and the support report.
using System;
using System.Drawing;
using System.Linq;
using System.Threading.Tasks;
using System.Windows.Forms;
using Microsoft.Win32;

namespace AmdgpuWddmControl
{
    public sealed class RecoveryView : Form
    {
        readonly FlowLayoutPanel _page;
        readonly Label _result;

        public RecoveryView()
        {
            Kmd.Blocked = true;
            Text = Program.ProductName + " - " + Strings.T("recovery.title");
            BackColor = Theme.Back; ForeColor = Theme.Text; Font = Theme.Body;
            ClientSize = Theme.Sz(760, 700); MinimumSize = Theme.Sz(560, 420); StartPosition = FormStartPosition.CenterScreen;
            var scroll = Theme.DarkScroll(new Panel { Dock = DockStyle.Fill, AutoScroll = true, Padding = Theme.Pad(24, 18, 24, 18), BackColor = Theme.Back });
            int width = Theme.S(680);
            _page = Ui.Stack(width);
            _page.Controls.Add(Ui.Label(Strings.T("recovery.title"), Theme.Title, null, width));
            _page.Controls.Add(Ui.Dim(Strings.T("recovery.intro"), width));

            var installed = new CardPanel(Strings.T("home.driver.title"), width);
            string version = ReleaseVersion();
            installed.Add(Ui.Label(version != null ? Strings.T("drv.installed", version) : Strings.T("drv.installed.none"), null, null, installed.Inner));
            // How long Windows waits for the graphics before it resets them (TdrSetting.cs, BD-079). This view only
            // reads, so it shows the waiting time and says where it is changed; the main window's Help page does that.
            var tdr = new RecoverySnapshot();
            RecoveryProbe.ReadTdr(tdr);
            installed.Add(Ui.Label(tdr.TdrError != null ? Strings.T("tdr.unreadable") : TdrSetting.StateText(tdr.Tdr()), null,
                tdr.TdrError != null ? Theme.Warn : (Color?)null, installed.Inner));
            installed.Add(Ui.Dim(Strings.T("tdr.in-main-window"), installed.Inner));
            _page.Controls.Add(installed);

            foreach (var g in HelpGuides.RecoveryIds)
            {
                var card = new CardPanel(Strings.T("help.guide." + g), width);
                int n = 0;
                foreach (var step in HelpGuides.Steps(g)) card.Add(Ui.Label(++n + ".  " + step, null, null, card.Inner));
                _page.Controls.Add(card);
            }

            var actions = new CardPanel(Strings.T("recovery.actions"), width);
            bool repair = HelpGuides.RepairSetup() != null;
            var row = Ui.WrapRow(actions.Inner);
            if (repair) row.Controls.Add(Ui.Button(Strings.T("status.action.repair"), (s, e) => Say(HelpGuides.StartRepair() ? "help.repair.started" : "help.repair.failed"), true));
            row.Controls.Add(Ui.Button(Strings.T("status.action.restart"), (s, e) => Restart()));
            row.Controls.Add(Ui.Button(Strings.T("help.report.create"), (s, e) => Report()));
            row.Controls.Add(Ui.Button(Strings.T("driver.updates.releases"), (s, e) => UpdateCheck.OpenReleasePage(null)));
            actions.Add(row);
            if (!repair) actions.Add(Ui.Label(Strings.T("help.repair.unavailable"), null, Theme.Warn, actions.Inner));
            _result = actions.Add(Ui.Label("", Theme.Bold, Theme.Good, actions.Inner));
            _page.Controls.Add(actions);
            scroll.Controls.Add(_page);
            Controls.Add(scroll);
        }

        public string Describe()
        {
            return string.Join(Environment.NewLine, MainForm.All(this).Where(c => !string.IsNullOrEmpty(c.Text)).Select(c => c.Text));
        }

        static string ReleaseVersion()
        {
            try
            {
                using (var k = Registry.LocalMachine.OpenSubKey(HelpGuides.RepairPath))
                    return k == null ? null : k.GetValue("Version") as string;
            }
            catch (Exception) { return null; }
        }

        void Say(string id) { _result.Text = Strings.T(id); }

        void Restart()
        {
            if (!RestartDialog.Ask(this, null)) return;
            if (WindowsRestart.Request() != null) Say("action.restart-failed");
        }

        void Report()
        {
            Say("help.report.collecting");
            var report = new BugReport(Redactor.ForThisPc());
            Task.Run(() => report.Collect(false, false, true, _ => { })).ContinueWith(t =>
            {
                if (t.IsFaulted) { Say("help.report.failed"); return; }
                var path = BugReport.DefaultPath(DateTime.Now);
                using (var preview = new ReportPreview(report, path))
                    if (preview.ShowDialog(this) != DialogResult.OK) { Say("help.report.cancelled"); return; }
                try { report.Write(path); _result.Text = Strings.T("help.report.saved", path); }
                catch (Exception) { Say("help.report.not-saved"); }
            }, TaskScheduler.FromCurrentSynchronizationContext());
        }
    }
}
