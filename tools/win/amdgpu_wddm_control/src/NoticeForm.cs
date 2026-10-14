// The window the person sees right after sign-in when the graphics driver is not running (owner, 2026-10-13:
// "Uzytkownik musi byc tego swiadomym od razu"). The rule that decides whether it opens at all, and which of its
// two cases it shows, is StartNotice.cs; this file is only the window and the one click it offers.
//
// It is this application started with --start-notice, from the logon task that already runs in the person's own
// session (the release's start-window.ps1). That task needs no rights and touches nothing of ours, so it starts
// this window and nothing else: the window reads the driver's state itself and runs the repair through the same
// elevated helper every other change of this application uses (one administrator prompt, Recovery.Plan ->
// RecoveryActions).
//
// What the window must survive, and how it does it:
//   Microsoft Basic Display at 1024x768, and 100 % to 200 % scaling. Nothing here has a fixed pixel position: one
//   top-down FlowLayoutPanel of auto-sized labels, a wrapping row of buttons, and AutoSize on the form, all built
//   from Theme.S() so the whole window grows with the scale. The content width is bounded by the working area of
//   the screen the window opens on, so a 200 % window on a 1024 px screen stays inside it instead of pushing its
//   buttons off the right edge (BD-137 found exactly that in the first setup window).
//   Four languages, chosen the way the control application chooses its language (AppPrefs.EffectiveLanguage: the
//   person's choice, else the language of Windows, else English).
//   No driver words. Everything the person reads comes from the string tables and goes through the G-NOINT gate
//   with every other sentence of this application.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace AmdgpuWddmControl
{
    sealed class NoticeForm : Form
    {
        readonly StartNoticeCase _case;
        readonly bool _smoke;
        readonly FlowLayoutPanel _body, _buttons, _outer;
        readonly int _inner;
        Label _result;
        Button _enable, _close;
        Size _wanted;                   // the size the content asked for, before the screen had its say (Fit)
        bool _restartOffered;
        // While the repair runs, this window counts the seconds in its own text every quarter second. The person
        // sees that something happens, and the changing text is also what keeps the desktop drawing: the driver
        // confirms a start only while pictures reach the screen (driver/kmd/start_health.c), which is the same
        // reason the small window after sign-in has a clock in it.
        readonly System.Windows.Forms.Timer _tick = new System.Windows.Forms.Timer { Interval = 250 };
        readonly System.Diagnostics.Stopwatch _clock = new System.Diagnostics.Stopwatch();

        // workArea: the working area of the screen in device pixels, so a render test can name one. Size.Empty
        // takes the screen the window opens on.
        public NoticeForm(StartNoticeCase noticeCase, bool smoke = false, Size workArea = default(Size))
        {
            _case = noticeCase;
            _smoke = smoke;
            var area = workArea.Width > 0 && workArea.Height > 0 ? workArea : WorkingArea();
            // The content column: the comfortable width, but never wider than the screen leaves room for. The
            // margin covers the window's own padding and border at every scale. The text scale of Windows
            // ("Make text bigger", up to 225 %) grows the fonts but not Theme.S(), so the column grows with it as
            // well: otherwise the same words need more room inside a column that stayed the same width, and the
            // widest button of a language is the first thing that does not fit.
            int wanted = (int)Math.Round(Theme.S(520) * Theme.TextScale), room = area.Width - Theme.S(80);
            _inner = Math.Max(Theme.S(240), Math.Min(wanted, room));

            Text = StartNotice.Title(_case);
            BackColor = Theme.Back; ForeColor = Theme.Text; Font = Theme.Body;
            FormBorderStyle = FormBorderStyle.FixedDialog; MaximizeBox = false; MinimizeBox = false;
            ShowInTaskbar = true;                  // the person may close it and find it again on the task bar
            TopMost = true;                        // it must be seen at once, over whatever the session opened
            StartPosition = FormStartPosition.CenterScreen;
            AutoSize = true; AutoSizeMode = AutoSizeMode.GrowAndShrink;
            AccessibleName = Text;
            // The application's own icon, from the same embedded resource the main window reads (build.ps1 embeds
            // art\icon\nagi.ico with every authored size). An icon that cannot be read is no reason not to warn.
            try
            {
                var asm = System.Reflection.Assembly.GetExecutingAssembly();
                using (var stream = asm.GetManifestResourceStream("nagi.ico"))
                    if (stream != null) Icon = new Icon(stream);
            }
            catch (Exception) { }

            var outer = _outer = Ui.Stack(_inner + Theme.S(40));
            outer.Padding = Theme.Pad(20, 16, 20, 12);
            _body = Ui.Stack(_inner);
            _body.Controls.Add(Ui.Label(Text, Theme.CardTitle, Theme.Warn, _inner));
            foreach (var line in StartNotice.Text(_case).Split('\n'))
                _body.Controls.Add(Ui.Label(line, null, null, _inner));
            outer.Controls.Add(_body);
            // A wrapping row, not a right-aligned one: three buttons in Japanese at 200 % on a small screen need
            // two lines, and a row that cannot wrap would put the last one outside the window.
            _buttons = Ui.WrapRow(_inner);
            _buttons.Margin = Theme.Pad(0, 12, 0, 0);
            outer.Controls.Add(_buttons);
            Controls.Add(outer);

            BuildButtons();
            CancelButton = _close;                                                      // Escape closes
            Fit(area);
            _tick.Tick += (s, e) => Result(Strings.T("notice.working") + " " +
                Strings.T("notice.working.seconds", (int)_clock.Elapsed.TotalSeconds), Theme.Warn);
            FormClosed += (s, e) => { _tick.Stop(); _tick.Dispose(); };
        }

        static Size WorkingArea()
        {
            try { var s = Screen.PrimaryScreen; return s == null ? new Size(1024, 768) : s.WorkingArea.Size; }
            catch (Exception) { return new Size(1024, 768); }
        }

        // The screen has the last word, and it never gets it by cutting the window off. Up to here the form sizes
        // itself to its content; this is where the working area is applied.
        //   It fits: the window keeps auto-sizing and takes the screen as its ceiling.
        //   It does not fit (a long translation at 200 % scaling with the text of Windows at 225 % on a 1024x768
        //   basic display): a maximum size on an auto-sizing form shrinks the form and clips whatever is below the
        //   cut, which here is the row of buttons - BD-137's shape, and the one thing this window must never lose.
        //   Such a window therefore stops auto-sizing, takes the screen and scrolls its content, so every button
        //   stays reachable. Findings() reports a window that neither fits nor scrolls.
        void Fit(Size area)
        {
            PerformLayout();
            _wanted = PreferredSize;
            if (_wanted.Width <= area.Width && _wanted.Height <= area.Height)
            {
                MaximumSize = new Size(area.Width, area.Height);
                return;
            }
            AutoSize = false;
            MaximumSize = Size.Empty;
            FormBorderStyle = FormBorderStyle.Sizable;      // a window this cramped should also be resizable
            Controls.Remove(_outer);
            var scroll = Theme.DarkScroll(new Panel { Dock = DockStyle.Fill, AutoScroll = true, BackColor = Theme.Back });
            scroll.Controls.Add(_outer);
            Controls.Add(scroll);
            Size = new Size(Math.Min(_wanted.Width, area.Width), Math.Min(_wanted.Height, area.Height));
            PerformLayout();
        }

        // The size the content asked for, before the screen had its say: a property, because a Form is a
        // marshal-by-reference class and reading a field of one from outside is refused by the compiler.
        public Size Wanted { get { return _wanted; } }

        // True when the content can be reached by scrolling, whatever the size of the window.
        public bool Scrolls
        {
            get { return Kids(this).Any(ScrollsItself); }
        }

        // AutoScroll lives on ScrollableControl, not on Control: a panel scrolls, a label cannot.
        static bool ScrollsItself(Control c)
        {
            var s = c as ScrollableControl;
            return s != null && s.AutoScroll;
        }

        void BuildButtons()
        {
            _buttons.Controls.Clear();
            _enable = null;
            if (StartNotice.OffersEnable(_case))
                _buttons.Controls.Add(_enable = Fitted(Ui.Button(Strings.T("status.action.enable-driver"), (s, e) => Enable(), true)));
            _buttons.Controls.Add(Fitted(Ui.Button(Strings.T("notice.open-app"), (s, e) => OpenApp())));
            _buttons.Controls.Add(_close = Fitted(Ui.Button(Strings.T("notice.close"), (s, e) => Close())));
        }

        // No button wider than the row that holds it. A wrapping row moves a control that does not fit to the next
        // line, but it cannot wrap one control, so a long sentence on a button ("Włącz sterownik karty graficznej z
        // powrotem..." at 200 % scaling with the text of Windows at 225 %) would put that button's right half
        // outside the window, and the row's own height would be computed for a line it then does not use. With a
        // maximum width the button breaks its words over two lines and grows downwards, where the column has room.
        Button Fitted(Button b)
        {
            b.MaximumSize = new Size(Math.Max(Theme.S(60), _inner - b.Margin.Horizontal), 0);
            return b;
        }

        // While the repair runs, everything but Close is off. Close stays on, and with it Escape: Form's Escape
        // handling clicks the cancel button, and a disabled button is not clicked, so a person who does not want
        // to wait out the repair would be left with the title bar's cross alone. The elevated helper runs in its
        // own process and finishes whether this window stays open or not.
        void Working(bool working)
        {
            foreach (Control c in _buttons.Controls) c.Enabled = !working || c == _close;
        }

        Label Result(string text, Color color)
        {
            if (_result == null)
            {
                _result = Ui.Label("", Theme.Bold, color, _inner);
                _result.AccessibleRole = AccessibleRole.StaticText;
                _body.Controls.Add(_result);
            }
            _result.ForeColor = color;
            _result.Text = text;
            return _result;
        }

        // The one click of case A. The repair itself is the enable-driver action: this window only asks for it,
        // waits off the user-interface thread and says what came back. The helper is bounded by itself, so this
        // wait ends on its own; every button but Close is off while it runs, and the window keeps drawing.
        void Enable()
        {
            Working(true);
            Result(Strings.T("notice.working"), Theme.Warn);
            if (_smoke) return;
            _clock.Restart();
            _tick.Start();
            Task.Run(() => Crash.Watched("enable driver", () => Program.RunElevatedCode("--action", "enable-driver"))).ContinueWith(t =>
            {
                int code = t.Status == TaskStatus.RanToCompletion ? t.Result : RecoveryRunner.Failed;
                _tick.Stop();
                Working(false);
                // Done, and refused because there is nothing to repair any more, both end this window's one job:
                // the button goes, because a second run would be refused, and a refusal shown as a failure would
                // offer a restart of Windows to somebody whose driver works.
                if (code == RecoveryRunner.Done) { Drop(_enable); Result(Strings.T("notice.done"), Theme.Good); return; }
                if (code == RecoveryRunner.Refused) { Drop(_enable); Result(Strings.T("notice.not-needed"), Theme.Good); return; }
                if (code == Program.NotElevated) { Result(Strings.T("action.not-elevated"), Theme.Warn); return; }
                Result(Strings.T(code == RecoveryRunner.RestartNeeded ? "notice.restart-needed" : "notice.action-failed"), Theme.Warn);
                OfferRestart();
            }, TaskScheduler.FromCurrentSynchronizationContext());
        }

        void Drop(Button b)
        {
            if (b == null) return;
            if (_enable == b) _enable = null;
            _buttons.Controls.Remove(b);
            b.Dispose();
        }

        // Added once, and only after an attempt that did not get the driver running: the person is then one click
        // from the thing that does work.
        void OfferRestart()
        {
            if (_restartOffered) return;
            _restartOffered = true;
            var b = Fitted(Ui.Button(Strings.T("ui.restart.now"), (s, e) =>
            {
                if (WindowsRestart.Request() != null) Result(Strings.T("action.restart-failed"), Theme.Warn);
            }));
            _buttons.Controls.Add(b);
            _buttons.Controls.SetChildIndex(b, 0);
        }

        void OpenApp()
        {
            try
            {
                System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(
                    System.Reflection.Assembly.GetExecutingAssembly().Location) { UseShellExecute = true });
                Close();
            }
            catch (Exception) { Result(Strings.T("notice.open-failed"), Theme.Warn); }
        }

        // ---- the window's own layout gate (BD-137) -------------------------------------------------------------
        // Every case, every language and every scale, built without being shown: nothing overlaps, nothing is cut
        // off on the right, nothing reaches outside the screen, and every button has a name a screen reader can
        // read. Program.cs --smoke-notice runs it in the build.
        public static string Findings(StartNoticeCase noticeCase, Size workArea)
        {
            var w = new StringBuilder();
            string where = (noticeCase == StartNoticeCase.Guard ? "guard" : "failed") + " " + Strings.Language +
                " at " + Theme.Scale.ToString("0.##", System.Globalization.CultureInfo.InvariantCulture) + "x on " + workArea.Width + "x" + workArea.Height;
            using (var f = new NoticeForm(noticeCase, true, workArea))
            {
                var handle = f.Handle;                      // builds the layout without showing the window
                GC.KeepAlive(handle);
                f.PerformLayout();
                // The window itself, against the screen. Two separate questions, because the first one answers
                // itself: the form is sized by Fit(), so it is inside the screen by construction. The question the
                // gate has to ask is the one Fit() answered, and with which content: a window whose content wanted
                // more room than the screen has must be able to scroll, or the row of buttons at its foot is cut
                // off and nothing on the screen says so (BD-137). Comparing the finished window with the screen,
                // as this gate did at first, can never fail and therefore measures nothing.
                if (f.Width > workArea.Width || f.Height > workArea.Height)
                    w.AppendLine(where + ": the window is " + f.Width + "x" + f.Height + ", the screen has " + workArea.Width + "x" + workArea.Height);
                if ((f.Wanted.Width > workArea.Width || f.Wanted.Height > workArea.Height) && !f.Scrolls)
                    w.AppendLine(where + ": the content wants " + f.Wanted.Width + "x" + f.Wanted.Height + " on a screen of " +
                        workArea.Width + "x" + workArea.Height + " and the window does not scroll, so the lowest row is cut off");
                foreach (var p in Overlaps(f)) w.AppendLine(where + ": " + p);
                foreach (var p in Overflows(f)) w.AppendLine(where + ": " + p);
                var texts = Texts(f).ToList();
                if (texts.Count < 3) w.AppendLine(where + ": only " + texts.Count + " texts in the window");
                foreach (var t in texts)
                {
                    if (t.StartsWith("[", StringComparison.Ordinal)) w.AppendLine(where + ": a text is missing from the tables: " + t);
                    foreach (var x in PlainWords.Findings(new[] { t })) w.AppendLine(where + ": G-NOINT: " + x);
                }
                foreach (var b in Buttons(f))
                    if (string.IsNullOrEmpty(b.AccessibleName)) w.AppendLine(where + ": a button has no name for a screen reader");
                if (!(f.CancelButton is Button)) w.AppendLine(where + ": Escape does not close the window");
            }
            return w.ToString();
        }

        static IEnumerable<Control> Kids(Control c) { return c.Controls.Cast<Control>(); }

        static IEnumerable<string> Texts(Control root)
        {
            foreach (var c in Kids(root))
            {
                if (!string.IsNullOrEmpty(c.Text)) yield return c.Text;
                foreach (var t in Texts(c)) yield return t;
            }
        }

        static IEnumerable<Button> Buttons(Control root)
        {
            foreach (var c in Kids(root))
            {
                if (c is Button) yield return (Button)c;
                foreach (var b in Buttons(c)) yield return b;
            }
        }

        static IEnumerable<string> Overlaps(Control parent)
        {
            var kids = Kids(parent).ToList();
            for (int i = 0; i < kids.Count; i++)
                for (int j = i + 1; j < kids.Count; j++)
                {
                    var a = kids[i].Bounds; a.Intersect(kids[j].Bounds);
                    if (a.Width > 0 && a.Height > 0)
                        yield return Tell(kids[i]) + " " + kids[i].Bounds + " overlaps " + Tell(kids[j]) + " " + kids[j].Bounds;
                }
            foreach (var k in kids) foreach (var o in Overlaps(k)) yield return o;
        }

        // Content that reaches outside the box that holds it, in both directions. A parent that scrolls is not
        // asked: there the content is meant to be larger than the box, and the person reaches it with the scroll
        // bar. Height matters as much as width: a window cut off at its foot loses the buttons, which is worse
        // than one cut off on the right, where only the end of a sentence goes missing.
        static IEnumerable<string> Overflows(Control root)
        {
            foreach (var c in Kids(root))
            {
                var parent = c.Parent;
                if (parent != null && !(parent is Form) && !ScrollsItself(parent))
                {
                    if (c.Right > parent.ClientSize.Width - parent.Padding.Right + 1)
                        yield return Tell(c) + " ends at " + c.Right + ", its parent " + Tell(parent) + " has " + (parent.ClientSize.Width - parent.Padding.Right);
                    if (c.Bottom > parent.ClientSize.Height - parent.Padding.Bottom + 1)
                        yield return Tell(c) + " ends " + c.Bottom + " down, its parent " + Tell(parent) + " has " + (parent.ClientSize.Height - parent.Padding.Bottom);
                }
                foreach (var o in Overflows(c)) yield return o;
            }
        }

        static string Tell(Control c)
        {
            var t = c.Text ?? "";
            return c.GetType().Name + (t.Length == 0 ? "" : " \"" + (t.Length > 40 ? t.Substring(0, 40) : t) + "\"");
        }
    }
}
