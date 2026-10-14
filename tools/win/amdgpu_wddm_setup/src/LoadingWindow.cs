// The start of setup with a window from the first moment (owner, 2026-10-14: "instalator z exe także ładuje się dość
// długo - także potrzebuje od razu okienka ładowania"). StartSequence.cs decides the order; this file does the steps:
//   SetupStart      the application context of every run with a window: the loading window first, then the
//                   administrator prompt, or the unpacking, and the setup window
//   LoadingWindow   the small window that is on the screen first: Nagi, the name of the program, one line of text and
//                   the progress strip of the setup window
//
// The copy that asks for administrator rights keeps its loading window until the elevated copy has its own on the
// screen: it makes a named event in the session's own namespace and gives its name to the elevated copy
// (--loading-handover), which sets it when its loading window is drawn. The event is made by the person's own copy,
// so it carries that account's default access list, which the elevated copy of the same account may set.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Security.AccessControl;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace AmdgpuWddmSetup
{
    sealed class SetupStart : ApplicationContext, IStartSteps
    {
        // How long the copy that asked for administrator rights keeps its loading window for the elevated copy, when
        // that copy neither sets the event nor ends. A copy without the event closes its window after a short moment.
        const int HandoverMs = 30000, NoHandoverMs = 1500;

        readonly SetupArgs _args;
        readonly string[] _argv;
        readonly StartSequence _sequence;
        LoadingWindow _loading;
        bool _closing;
        EventWaitHandle _handover;
        Process _elevated;
        CancellationTokenSource _stop;

        // One unpacking at a time (Payload.RunMutexName), held until the process ends: a static field, so that
        // nothing collects it while the install is still going on.
        static Mutex _run;

        public SetupStart(SetupArgs args, string[] argv)
        {
            _args = args;
            _argv = argv;
            _sequence = new StartSequence(this);
            _sequence.Begin();
        }

        public int ExitCode { get { return _sequence.ExitCode; } }

        public void ShowLoading()
        {
            _loading = new LoadingWindow(Native.ShowNagi(), Theme.Animations);
            _loading.Shown += (s, e) =>
            {
                // Drawn now, and only then the slow work: the posted call runs after the paint that Update forces.
                _loading.Update();
                _loading.BeginInvoke((Action)_sequence.LoadingShown);
            };
            _loading.FormClosing += (s, e) =>
            {
                if (_closing || e.CloseReason != CloseReason.UserClosing) return;
                if (!_sequence.CloseRequested()) e.Cancel = true;
            };
            MainForm = _loading;                        // closing it before the setup window opens ends the program
            _loading.Show();
        }

        // The elevated copy tells the copy that started it that its loading window is on the screen.
        public void HandOver()
        {
            if (_args.Handover == null) return;
            try
            {
                using (var e = EventWaitHandle.OpenExisting(@"Local\" + _args.Handover, EventWaitHandleRights.Modify | EventWaitHandleRights.Synchronize))
                    e.Set();
            }
            catch (Exception) { }
        }

        // Off the user-interface thread: the first opening of the file is the one Windows Defender looks at.
        public void Inspect()
        {
            var a = _args;
            Task.Run(() =>
            {
                bool carries = Payload.Carries(Payload.Present(), a.Package, SetupForm.HasPackage(SetupForm.DefaultPackage()));
                return new[] { (a.Mode != "prepare-offline" || carries) && !Native.IsElevated(), carries };
            }).ContinueWith(t =>
            {
                var r = t.Status == TaskStatus.RanToCompletion ? t.Result : new[] { a.Mode != "prepare-offline" && !Native.IsElevated(), false };
                if (t.IsFaulted) GC.KeepAlive(t.Exception);
                _sequence.Inspected(r[0], r[1]);
            }, TaskScheduler.FromCurrentSynchronizationContext());
        }

        public void SayPermission() { _loading.Say(Strings.T("loading.permission"), null); }

        public void Elevate()
        {
            var name = SetupArgs.HandoverPrefix + Guid.NewGuid().ToString("N");
            try { _handover = new EventWaitHandle(false, EventResetMode.ManualReset, @"Local\" + name); }
            catch (Exception) { _handover = null; }
            var argv = _handover != null ? _argv.Concat(new[] { "--loading-handover", name }).ToArray() : _argv;
            var owner = _loading.Handle;
            Task.Run(() => Native.RelaunchElevated(argv, owner)).ContinueWith(t =>
            {
                _elevated = t.Status == TaskStatus.RanToCompletion ? t.Result : null;
                if (t.IsFaulted) GC.KeepAlive(t.Exception);
                _sequence.Elevated(_elevated != null);
            }, TaskScheduler.FromCurrentSynchronizationContext());
        }

        // Until the elevated copy's loading window is drawn, that copy ended, or the bound passed.
        public void WaitHandover()
        {
            var handover = _handover;
            var process = _elevated;
            Task.Run(() =>
            {
                var clock = Stopwatch.StartNew();
                int bound = handover != null ? HandoverMs : NoHandoverMs;
                while (clock.ElapsedMilliseconds < bound)
                {
                    if (handover != null) { if (handover.WaitOne(100)) return; }
                    else Thread.Sleep(100);
                    try { if (process.HasExited) return; } catch (Exception) { return; }
                }
            }).ContinueWith(t =>
            {
                if (_handover != null) { _handover.Dispose(); _handover = null; }
                if (_elevated != null) { _elevated.Dispose(); _elevated = null; }
                _sequence.HandedOver();
            }, TaskScheduler.FromCurrentSynchronizationContext());
        }

        // On the user-interface thread, which holds the lock until the process ends. A second copy of the same file
        // would reset the folder the first one is installing from.
        public bool ClaimRun()
        {
            bool mine;
            _run = new Mutex(true, Payload.RunMutexName, out mine);
            if (mine) return true;
            try { return _run.WaitOne(0); } catch (AbandonedMutexException) { return true; }
        }

        public void SayUnpacking() { _loading.Say(Strings.T("loading.unpacking"), 0f); }

        public void Unpack()
        {
            _stop = new CancellationTokenSource();
            var cancel = _stop.Token;
            var loading = _loading;
            var a = _args;
            int shown = -1;
            // On the thread that unpacks: one message to the window for each whole percent, not one for each step.
            Action<long, long> progress = (done, total) =>
            {
                int percent = total <= 0 ? 0 : (int)Math.Min(100, done * 100 / total);
                if (percent == shown) return;
                shown = percent;
                try { loading.BeginInvoke((Action)(() => loading.Progress(percent / 100f))); } catch (InvalidOperationException) { }
            };
            Task.Run(() => Payload.Extract(Payload.Root, true, progress, cancel)).ContinueWith(t =>
            {
                string problem = null;
                bool cancelled = t.IsCanceled || t.IsFaulted && t.Exception.InnerException is OperationCanceledException;
                if (t.Status == TaskStatus.RanToCompletion)
                {
                    a.Package = t.Result;
                    a.PayloadPackage = t.Result;
                }
                else if (!cancelled)
                {
                    // The package in the file is damaged, it does not match the list it came with, or the folder it
                    // unpacks into cannot be made safe. Nothing is installed from it: the words say so, the detail
                    // goes where every unexpected detail goes.
                    var e = t.Exception.InnerException;
                    Program.Problem(a, "payload: " + e);
                    problem = e is Payload.FolderException ? "payload.folder" : "payload.damaged";
                }
                _sequence.Unpacked(problem, cancelled);
            }, TaskScheduler.FromCurrentSynchronizationContext());
        }

        public void StopUnpacking() { if (_stop != null) _stop.Cancel(); }

        public void ShowSetup()
        {
            var form = new SetupForm(_args, false);
            MainForm = form;                            // from now on the setup window ends the program
            form.Show();
            form.Update();                              // drawn before the loading window goes, so no gap shows
        }

        public void Tell(string messageId)
        {
            bool warn = messageId == "payload.damaged" || messageId == "payload.folder";
            MessageBox.Show(_loading, Strings.T(messageId), Program.ProductName, MessageBoxButtons.OK, warn ? MessageBoxIcon.Warning : MessageBoxIcon.Information);
        }

        public void CloseLoading()
        {
            _closing = true;
            _loading.Close();
        }
    }

    // The first window of a run. It reads nothing but the person's "Show Nagi" choice of the control app (one value
    // under HKCU) and the string tables; the picture is the guide's welcome picture, embedded in the exe. No title bar:
    // DWM rounds its corners and draws its frame on Windows 11 (Dwm.Apply, as for the setup window), and the task bar
    // shows it with the program's icon.
    sealed class LoadingWindow : Form
    {
        public const string Expression = "01-wave";

        // The stages the window says, for the layout gate: the words of each one.
        public static readonly string[] Stages = { "loading.starting", "loading.permission", "loading.unpacking" };

        readonly int _inner;
        readonly Label _line;
        readonly ProgressStrip _strip;
        readonly FlowLayoutPanel _row;
        readonly Size _area;
        readonly System.Windows.Forms.Timer _tick;
        Size _wanted;

        // workArea: the working area of the screen in device pixels, so the gate can name one. Size.Empty takes the
        // screen the window opens on.
        public LoadingWindow(bool nagi, bool animate, Size workArea = default(Size))
        {
            _area = workArea.Width > 0 && workArea.Height > 0 ? workArea : WorkingArea();
            Image art = nagi ? NagiArt.Load(Expression, SetupGuide.ArtPixels) : null;
            int artWidth = art != null ? Theme.S(112 + 20) : 0;
            int wanted = (int)Math.Round(Theme.S(360) * Theme.TextScale), room = _area.Width - artWidth - Theme.S(100);
            _inner = Math.Max(Theme.S(180), Math.Min(wanted, room));

            Text = Strings.T("ui.window-title");
            AccessibleName = Text;
            AutoScaleMode = AutoScaleMode.None;
            BackColor = Theme.Back; ForeColor = Theme.Text; Font = Theme.Body;
            FormBorderStyle = FormBorderStyle.None; MaximizeBox = false; MinimizeBox = false;
            ShowInTaskbar = true;
            StartPosition = FormStartPosition.CenterScreen;
            AutoSize = true; AutoSizeMode = AutoSizeMode.GrowAndShrink;
            try
            {
                var asm = System.Reflection.Assembly.GetExecutingAssembly();
                using (var s = asm.GetManifestResourceStream("nagi.ico"))
                    if (s != null) Icon = new Icon(s);
            }
            catch (Exception) { }

            _row = Ui.Row();
            _row.Padding = Theme.Pad(20, 18, 24, 18);
            if (art != null)
                _row.Controls.Add(new PictureBox
                {
                    Image = art, Size = Theme.Sz(112, 112), SizeMode = PictureBoxSizeMode.Zoom, BackColor = Theme.Back,
                    Margin = Theme.Pad(0, 0, 20, 0), AccessibleRole = AccessibleRole.None, AccessibleName = "", TabStop = false,
                });
            var text = Ui.Stack(_inner);
            text.Controls.Add(Ui.Label(Program.ProductName, Theme.Title, null, _inner));
            _line = Ui.Label(Strings.T("loading.starting"), null, Theme.Dim, _inner);
            _line.AccessibleRole = AccessibleRole.StaticText;
            text.Controls.Add(_line);
            _strip = new ProgressStrip(_inner) { Margin = Theme.Pad(0, 12, 0, 4), AccessibleName = Text };
            text.Controls.Add(_strip);
            _row.Controls.Add(text);
            Controls.Add(_row);
            Fit();

            // The band of the strip moves while the window is on the screen and Windows animates; a screen that is
            // drawn and not shown keeps the band where it is (ProgressStrip.Phase).
            _tick = new System.Windows.Forms.Timer { Interval = 33 };
            _tick.Tick += (s, e) => { _strip.Phase = (_strip.Phase + 0.012f) % 1f; if (_strip.Indeterminate) _strip.Invalidate(); };
            if (animate) Shown += (s, e) => _tick.Start();
            FormClosed += (s, e) => { _tick.Stop(); _tick.Dispose(); };
        }

        static Size WorkingArea()
        {
            try { var s = System.Windows.Forms.Screen.FromPoint(Cursor.Position); return s == null ? new Size(1024, 768) : s.WorkingArea.Size; }
            catch (Exception) { return new Size(1024, 768); }
        }

        protected override void OnHandleCreated(EventArgs e)
        {
            base.OnHandleCreated(e);
            Dwm.Apply(this);
        }

        // A window without a title bar has no edge of its own on Windows 10, where DWM draws no frame for it.
        protected override CreateParams CreateParams
        {
            get { var cp = base.CreateParams; cp.ClassStyle |= 0x00020000; return cp; }   // CS_DROPSHADOW
        }

        // The screen has the last word: a window that fits keeps sizing itself to its content; one that does not
        // takes the screen and scrolls.
        void Fit()
        {
            MaximumSize = Size.Empty;                   // the content's own size, not the one an earlier Fit allowed
            PerformLayout();
            _wanted = PreferredSize;
            if (_wanted.Width <= _area.Width && _wanted.Height <= _area.Height)
            {
                MaximumSize = _area;
                return;
            }
            AutoSize = false;
            MaximumSize = Size.Empty;
            Controls.Remove(_row);
            var scroll = Theme.DarkScroll(new Panel { Dock = DockStyle.Fill, AutoScroll = true, BackColor = Theme.Back });
            scroll.Controls.Add(_row);
            Controls.Add(scroll);
            Size = new Size(Math.Min(_wanted.Width, _area.Width), Math.Min(_wanted.Height, _area.Height));
            PerformLayout();
        }

        public Size Wanted { get { return _wanted; } }

        public bool Scrolls { get { return Controls.OfType<ScrollableControl>().Any(c => c.AutoScroll); } }

        // A new line, and the known part of the work (null: the band that moves). The window keeps its centre.
        public void Say(string text, float? done)
        {
            var centre = new Point(Left + Width / 2, Top + Height / 2);
            _line.Text = text;
            _strip.Indeterminate = done == null;
            _strip.Value = done ?? 0f;
            _strip.Invalidate();
            if (AutoSize) Fit();                        // a window that scrolls already keeps its size
            else PerformLayout();
            if (Visible) Location = new Point(centre.X - Width / 2, centre.Y - Height / 2);
        }

        public void Progress(float done)
        {
            if (IsDisposed) return;
            _strip.Indeterminate = false;
            _strip.Value = done;
            _strip.Invalidate();
        }

        // ---- the window's own layout gate (--smoke-loading) ----------------------------------------------------
        // One stage built without being shown, against one screen: it fits or scrolls, no two controls of one parent
        // overlap, nothing reaches outside its box, every text is in the tables and has no internals, and the picture
        // is there when the exe carries it.
        public static string Findings(string stage, Size workArea)
        {
            var w = new StringBuilder();
            string where = stage + " " + Strings.Language + " at " + Theme.Scale.ToString("0.##", System.Globalization.CultureInfo.InvariantCulture) +
                "x, text " + Theme.TextScale.ToString("0.##", System.Globalization.CultureInfo.InvariantCulture) + "x on " + workArea.Width + "x" + workArea.Height;
            using (var f = new LoadingWindow(true, false, workArea))
            {
                var handle = f.Handle;
                GC.KeepAlive(handle);
                f.Say(Strings.T(stage), stage == "loading.unpacking" ? (float?)0.4f : null);
                f.PerformLayout();
                if (f.Width > workArea.Width || f.Height > workArea.Height)
                    w.AppendLine(where + ": the window is " + f.Width + "x" + f.Height + ", the screen has " + workArea.Width + "x" + workArea.Height);
                if ((f.Wanted.Width > workArea.Width || f.Wanted.Height > workArea.Height) && !f.Scrolls)
                    w.AppendLine(where + ": the content wants " + f.Wanted.Width + "x" + f.Wanted.Height + " and the window does not scroll");
                foreach (var c in All(f))
                {
                    var kids = c.Controls.Cast<Control>().ToList();
                    for (int i = 0; i < kids.Count; i++)
                        for (int j = i + 1; j < kids.Count; j++)
                        {
                            var x = kids[i].Bounds; x.Intersect(kids[j].Bounds);
                            if (x.Width > 0 && x.Height > 0) w.AppendLine(where + ": " + Tell(kids[i]) + " overlaps " + Tell(kids[j]));
                        }
                    var parent = c.Parent;
                    bool scrolls = parent is ScrollableControl && ((ScrollableControl)parent).AutoScroll;
                    if (parent != null && !(parent is Form) && !scrolls)
                    {
                        if (c.Right > parent.ClientSize.Width - parent.Padding.Right + 1) w.AppendLine(where + ": " + Tell(c) + " ends at " + c.Right + ", its parent has " + (parent.ClientSize.Width - parent.Padding.Right));
                        if (c.Bottom > parent.ClientSize.Height - parent.Padding.Bottom + 1) w.AppendLine(where + ": " + Tell(c) + " ends " + c.Bottom + " down, its parent has " + (parent.ClientSize.Height - parent.Padding.Bottom));
                    }
                }
                var texts = All(f).Where(c => !(c is ProgressStrip) && !string.IsNullOrEmpty(c.Text)).Select(c => c.Text).ToList();
                if (texts.Count < 2) w.AppendLine(where + ": only " + texts.Count + " texts in the window");
                foreach (var t in texts)
                {
                    foreach (var hit in NoInternals.Find(t)) w.AppendLine(where + ": G-NOINT \"" + hit + "\"");
                    foreach (System.Text.RegularExpressions.Match m in System.Text.RegularExpressions.Regex.Matches(t, @"\[[a-z0-9.-]+\]")) w.AppendLine(where + ": missing string " + m.Value);
                }
                if (NagiArt.Has(Expression, SetupGuide.ArtPixels) && !All(f).OfType<PictureBox>().Any(p => p.Image != null))
                    w.AppendLine(where + ": the exe carries the picture and the window does not show it");
            }
            return w.ToString();
        }

        static IEnumerable<Control> All(Control root)
        {
            foreach (Control c in root.Controls)
            {
                yield return c;
                foreach (var k in All(c)) yield return k;
            }
        }

        static string Tell(Control c)
        {
            var t = c.Text ?? "";
            return c.GetType().Name + " " + c.Bounds + (t.Length == 0 ? "" : " \"" + (t.Length > 40 ? t.Substring(0, 40) : t) + "\"");
        }
    }
}
