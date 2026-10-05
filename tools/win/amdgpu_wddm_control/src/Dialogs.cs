// The window's dialogs, all in the window's language and theme: Save / Discard / Keep editing (WU-011, Escape keeps
// editing), the confirmation of a planned change (WU-053), the restart question (WU-060: hints, "Later"), the
// Identify label on the monitor (WU-030, borderless, closes after 3 s) and the report preview (WU-067).
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Linq;
using System.Text;
using System.Windows.Forms;

namespace AmdgpuWddmControl
{
    public enum ThreeWay { Save, Discard, Keep }

    // A dark dialog: a title, text blocks and a row of buttons on the right.
    class ThemedDialog : Form
    {
        protected readonly FlowLayoutPanel Body, Buttons;
        protected readonly int Inner;

        public ThemedDialog(string title, int width = 560)
        {
            Text = title; BackColor = Theme.Back; ForeColor = Theme.Text; Font = Theme.Body;
            FormBorderStyle = FormBorderStyle.FixedDialog; MaximizeBox = false; MinimizeBox = false; ShowInTaskbar = false;
            StartPosition = FormStartPosition.CenterParent; AutoSize = true; AutoSizeMode = AutoSizeMode.GrowAndShrink;
            Inner = Theme.S(width);
            var outer = Ui.Stack(Inner + Theme.S(40));
            outer.Padding = Theme.Pad(20, 16, 20, 12);
            Body = Ui.Stack(Inner);
            Body.Controls.Add(Ui.Label(title, Theme.CardTitle, null, Inner));
            outer.Controls.Add(Body);
            Buttons = new FlowLayoutPanel { FlowDirection = FlowDirection.RightToLeft, AutoSize = true, WrapContents = false, MinimumSize = new Size(Inner, 0), Margin = Theme.Pad(0, 12, 0, 0) };
            outer.Controls.Add(Buttons);
            Controls.Add(outer);
        }

        protected Label Line(string text, Color? color = null, Font font = null)
        {
            var l = Ui.Label(text, font, color, Inner);
            Body.Controls.Add(l);
            return l;
        }

        protected Button Add(string text, DialogResult result, bool primary = false)
        {
            var b = Ui.Button(text, (s, e) => { DialogResult = result; Close(); }, primary);
            b.Margin = Theme.Pad(10, 0, 0, 0);
            Buttons.Controls.Add(b);
            return b;
        }
    }

    sealed class ThreeWayDialog : ThemedDialog
    {
        ThreeWay _answer = ThreeWay.Keep;

        ThreeWayDialog(string title, string text) : base(title, 460)
        {
            Line(text, Theme.Dim);
            var keep = Add(Strings.T("ui.unsaved.keep"), DialogResult.Cancel);
            var discard = Add(Strings.T("ui.unsaved.discard"), DialogResult.No);
            var save = Add(Strings.T("ui.unsaved.save"), DialogResult.Yes, true);
            save.Click += (s, e) => _answer = ThreeWay.Save;
            discard.Click += (s, e) => _answer = ThreeWay.Discard;
            keep.Click += (s, e) => _answer = ThreeWay.Keep;
            CancelButton = keep;           // Escape = keep editing
            AcceptButton = save;
        }

        public static ThreeWay Ask(IWin32Window owner, string title, string text)
        {
            using (var d = new ThreeWayDialog(title, text)) { d.ShowDialog(owner); return d._answer; }
        }

        // G-A11Y: Escape (the dialog's cancel button) keeps editing, Enter saves.
        public static bool EscapeKeepsEditing()
        {
            using (var d = new ThreeWayDialog("t", "t"))
                return d.CancelButton is Button && ((Button)d.CancelButton).Text == Strings.T("ui.unsaved.keep") && ((Button)d.AcceptButton).Text == Strings.T("ui.unsaved.save");
        }

        public static string[] ButtonTexts(){ return new[] { Strings.T("ui.unsaved.save"), Strings.T("ui.unsaved.discard"), Strings.T("ui.unsaved.keep") }; }
    }

    sealed class ConfirmDialog : ThemedDialog
    {
        ConfirmDialog(PlainDialog p, HintDecision hints) : base(p.Title)
        {
            Line(Strings.T("ui.confirm.changes"), null, Theme.Bold);
            foreach (var c in p.Changes) Line("•  " + c);
            Line("");
            foreach (var n in p.Notes) Line(n, Theme.Dim);
            if (hints != null) foreach (var h in hints.Lines) Line(h, Theme.Warn);
            Line(Strings.T("ui.confirm.admin"), Theme.Dim);
            var cancel = Add(Strings.T("ui.cancel"), DialogResult.Cancel);
            var go = Add(Strings.T("ui.continue"), DialogResult.OK, true);
            CancelButton = cancel; AcceptButton = go;
            ActiveControl = cancel;
        }

        public static bool Ask(IWin32Window owner, PlainDialog p, HintDecision hints)
        {
            using (var d = new ConfirmDialog(p, hints)) return d.ShowDialog(owner) == DialogResult.OK;
        }
    }

    sealed class RestartDialog : ThemedDialog
    {
        RestartDialog(HintDecision hints) : base(Strings.T("ui.restart.title"), 480)
        {
            Line(Strings.T("ui.restart.text"), Theme.Dim);
            foreach (var h in hints.Lines) Line(h, Theme.Warn);
            var later = Add(Strings.T("ui.restart.later"), DialogResult.Cancel);
            var now = Add(Strings.T("ui.restart.now"), DialogResult.OK, true);
            CancelButton = later; AcceptButton = later;     // Enter does not restart: the user clicks "Restart now"
            ActiveControl = later;
        }

        public static bool Ask(IWin32Window owner, HintDecision hints)
        {
            using (var d = new RestartDialog(hints ?? new HintDecision())) return d.ShowDialog(owner) == DialogResult.OK;
        }
    }

    // The number of a monitor drawn on that monitor: borderless, topmost, no focus taken, closes after 3 s.
    sealed class IdentifyForm : Form
    {
        readonly Timer _close = new Timer { Interval = 3000 };

        public IdentifyForm(Rectangle screen, string text)
        {
            FormBorderStyle = FormBorderStyle.None; ShowInTaskbar = false; TopMost = true; StartPosition = FormStartPosition.Manual;
            BackColor = Theme.Nav; ForeColor = Theme.Text;
            var size = Theme.Sz(320, 200);
            Bounds = new Rectangle(screen.X + Theme.S(40), screen.Y + Theme.S(40), size.Width, size.Height);
            var l = new Label { Text = text, Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleCenter, Font = new Font(Theme.Title.FontFamily, Theme.Title.Size * 2.2f, FontStyle.Bold, GraphicsUnit.Pixel), ForeColor = Theme.Text };
            Controls.Add(l);
            _close.Tick += (s, e) => Close();
        }

        protected override bool ShowWithoutActivation { get { return true; } }
        protected override void OnShown(EventArgs e) { base.OnShown(e); _close.Start(); }
        protected override void OnFormClosed(FormClosedEventArgs e) { _close.Dispose(); base.OnFormClosed(e); }
    }

    // The file list of a report before anything is written, with a look at each file's text. The report's content
    // stays English: it is for the people who fix the problem.
    sealed class ReportPreview : Form
    {
        public ReportPreview(BugReport report, string path)
        {
            Text = Strings.T("help.report.preview"); BackColor = Theme.Back; ForeColor = Theme.Text; Font = Theme.Body;
            ClientSize = Theme.Sz(900, 600); StartPosition = FormStartPosition.CenterParent; MinimizeBox = false;
            var top = new Panel { Dock = DockStyle.Top, Height = Theme.S(54), Padding = Theme.Pad(12, 8, 12, 0) };
            top.Controls.Add(Ui.Label(Strings.T("help.report.where", path), null, null, Theme.S(870)));
            var list = new ListView
            {
                Dock = DockStyle.Top, Height = Theme.S(200), View = View.Details, FullRowSelect = true, MultiSelect = false, HideSelection = false,
                BackColor = Theme.Card, ForeColor = Theme.Text, BorderStyle = BorderStyle.None, AccessibleName = Strings.T("help.report.files"),
            };
            Theme.DarkList(list);
            list.Columns.Add(Strings.T("help.report.file"), Theme.S(170)); list.Columns.Add(Strings.T("help.report.size"), Theme.S(90)); list.Columns.Add(Strings.T("help.report.contents"), Theme.S(600));
            foreach (var e in report.Entries) list.Items.Add(new ListViewItem(new[] { e.Name, (e.Data.Length / 1024.0).ToString("0.0", System.Globalization.CultureInfo.InvariantCulture) + " KB", e.Description }) { Tag = e });
            var text = Theme.DarkScroll(new TextBox
            {
                Dock = DockStyle.Fill, Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Both, WordWrap = false,
                BackColor = Theme.Nav, ForeColor = Theme.Text, Font = Theme.Mono, BorderStyle = BorderStyle.None, AccessibleName = Strings.T("help.report.contents"),
            });
            list.SelectedIndexChanged += (s, e) =>
            {
                if (list.SelectedItems.Count == 0) return;
                var entry = (ReportEntry)list.SelectedItems[0].Tag;
                var content = Encoding.UTF8.GetString(entry.Data);
                text.Text = content.Length > 200000 ? content.Substring(0, 200000) + "\r\n[...]" : content.Replace("\r\n", "\n").Replace("\n", "\r\n");
            };
            var bottom = new FlowLayoutPanel { Dock = DockStyle.Bottom, Height = Theme.S(48), FlowDirection = FlowDirection.RightToLeft, Padding = Theme.Pad(8) };
            var save = Ui.Button(Strings.T("help.report.save"), (s, e) => { DialogResult = DialogResult.OK; Close(); }, true);
            var cancel = Ui.Button(Strings.T("ui.cancel"), (s, e) => { DialogResult = DialogResult.Cancel; Close(); });
            bottom.Controls.Add(save); bottom.Controls.Add(cancel);
            Controls.Add(text); Controls.Add(list); Controls.Add(top); Controls.Add(bottom);
            AcceptButton = save; CancelButton = cancel;
            if (list.Items.Count > 0) list.Items[0].Selected = true;
        }
    }
}
