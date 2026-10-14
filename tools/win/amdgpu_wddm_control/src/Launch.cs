// The start of the window (owner, 2026-10-14: "czasem Nagi Control App włącza się dość długo - trzeba dać jak
// najszybciej ekran ładowania", and "Da się włączyć kilka Nagi Control App na raz - kolejne nie powinny się
// otwierać"). LaunchSequence.cs decides the order and SingleInstance.cs which start owns the window; this file does
// the steps with real windows:
//   Launch          the application context of the window path: the loading window first, then the main window
//   LoadingWindow   the small window that is on the screen first: Nagi, the name of the app and one line of text
//   WindowFront     brings a window of this process to the front when a later start asks for it
//
// Before this change the main window's constructor and every reading of the computer ran before the first picture,
// all of it on the thread that draws (LaunchSequence.cs has the numbers). Now the loading window is built from the
// string tables and one embedded picture only, and it is drawn before anything slow starts.
using System;
using System.Diagnostics;
using System.Drawing;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace AmdgpuWddmControl
{
    sealed class Launch : ApplicationContext, ILaunchSteps
    {
        readonly SingleInstance _guard;
        readonly AppPrefs _prefs;
        readonly LaunchSequence _sequence;
        readonly Stopwatch _clock = Stopwatch.StartNew();
        SynchronizationContext _ui;
        LoadingWindow _loading;
        MainForm _main;
        WindowReading _reading;

        public Launch(SingleInstance guard)
        {
            _guard = guard;
            _prefs = AppPrefs.Live();
            Strings.Language = _prefs.EffectiveLanguage;
            Theme.Fonts();
            _sequence = new LaunchSequence(this);
            _sequence.Begin();
        }

        public void ShowLoading()
        {
            _loading = new LoadingWindow(_prefs.ShowNagi, !_prefs.ReduceAnimations && AppPrefs.WindowsAnimations() != false);
            _ui = SynchronizationContext.Current;       // the first control of this thread installed it
            _loading.Shown += (s, e) =>
            {
                // Drawn now, and only then the slow work: the posted call runs after the paint that Update forces.
                _loading.Update();
                _loading.BeginInvoke((Action)_sequence.LoadingShown);
            };
            _loading.Ticked += () => _sequence.Elapsed(_clock.Elapsed.TotalSeconds);
            _loading.FormClosed += (s, e) => { if (!_sequence.MainShown) Cancelled(); };
            MainForm = _loading;                        // closing it before the main window opens ends the program
            _loading.Show();
            // A later start sets the event; the window that is open then comes forward. A signal from before this
            // line stays on the event and arrives here at once.
            if (_guard != null)
                _guard.Listen(() => { var ui = _ui; if (ui != null) ui.Post(_ => Front(), null); });
        }

        // Off the user-interface thread. AppPrefs of its own, because the main window reads its own at the same time.
        public void StartReading()
        {
            var prefs = AppPrefs.Live();
            Task.Run(() => WindowReading.Read(prefs, false)).ContinueWith(t =>
            {
                // A reading that failed as a whole leaves the main window to read for itself (RefreshAll), as before.
                _reading = t.Status == TaskStatus.RanToCompletion ? t.Result : null;
                if (t.IsFaulted) GC.KeepAlive(t.Exception);
                _sequence.ReadingDone();
            }, TaskScheduler.FromCurrentSynchronizationContext());
        }

        // On the user-interface thread, while the reading runs: the window and its controls, not yet shown.
        public void BuildMain()
        {
            _main = new MainForm();
            _main.CreateControlTree();
            _sequence.MainBuilt();
        }

        public void ShowMain()
        {
            if (_reading != null) _main.UseStartReading(_reading);
            MainForm = _main;                           // from now on the main window ends the program
            _main.Show();
            _main.Update();                             // drawn before the loading window goes, so no gap shows
        }

        public void CloseLoading() { _loading.Close(); }

        public void SaySlow() { _loading.SaySlow(); }

        void Cancelled()
        {
            _sequence.Cancel();
            if (_main != null) { _main.Dispose(); _main = null; }
        }

        // A later start asked for the window: the main window once it is open, the loading window before that.
        void Front()
        {
            Form f = _sequence.MainShown ? (Form)_main : _loading;
            if (f != null && !f.IsDisposed) WindowFront.Bring(f);
        }
    }

    // The first window of a start. It reads nothing from the computer: the person's language and the "Show Nagi" and
    // animation choices came from AppPrefs before it, and the picture is the guide's welcome picture, embedded in the
    // exe. No title bar: a thin frame, a shadow and the name of the app in the window itself; the task bar shows it
    // with the app's icon, so a person who clicks elsewhere finds it again. After LaunchSequence.SlowSeconds it says
    // that the start takes longer and offers to close it.
    sealed class LoadingWindow : Form
    {
        public const string Expression = "01-wave";

        readonly int _inner;
        readonly Label _line;
        readonly FlowLayoutPanel _row, _buttons;
        readonly ActivityStrip _strip;
        readonly System.Windows.Forms.Timer _tick;
        readonly Size _area;
        Button _close;
        Size _wanted;

        // Every tick of the window's timer: the start counts its time from it (LaunchSequence.Elapsed).
        public event Action Ticked;
        public bool Slow { get; private set; }

        // workArea: the working area of the screen in device pixels, so the gate can name one. Size.Empty takes the
        // screen the window opens on.
        public LoadingWindow(bool nagi, bool animate, Size workArea = default(Size))
        {
            _area = workArea.Width > 0 && workArea.Height > 0 ? workArea : WorkingArea();
            Image art = nagi ? NagiArt.Load(Guide.Resource(Expression, Guide.ArtSize(Theme.Scale))) : null;
            int artWidth = art != null ? Theme.S(112 + 20) : 0;
            // The text column: the comfortable width, grown with the text scale of Windows like the sign-in notice's,
            // and never wider than the screen leaves room for beside the picture.
            int wanted = (int)Math.Round(Theme.S(340) * Theme.TextScale), room = _area.Width - artWidth - Theme.S(100);
            _inner = Math.Max(Theme.S(180), Math.Min(wanted, room));

            Text = Program.ProductName;
            AccessibleName = Text;
            BackColor = Theme.Back; ForeColor = Theme.Text; Font = Theme.Body;
            FormBorderStyle = FormBorderStyle.None; MaximizeBox = false; MinimizeBox = false;
            ShowInTaskbar = true;
            StartPosition = FormStartPosition.CenterScreen;
            AutoSize = true; AutoSizeMode = AutoSizeMode.GrowAndShrink;
            Padding = new Padding(1);                   // room for the frame line (OnPaint)
            try
            {
                var asm = System.Reflection.Assembly.GetExecutingAssembly();
                using (var stream = asm.GetManifestResourceStream("nagi.ico"))
                    if (stream != null) Icon = new Icon(stream);
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
            _line = Ui.Label(Strings.T("launch.starting"), null, Theme.Dim, _inner);
            _line.AccessibleRole = AccessibleRole.StaticText;
            text.Controls.Add(_line);
            _strip = new ActivityStrip(_inner, animate) { Margin = Theme.Pad(0, 12, 0, 4) };
            text.Controls.Add(_strip);
            _buttons = Ui.WrapRow(_inner);
            text.Controls.Add(_buttons);
            _row.Controls.Add(text);
            Controls.Add(_row);
            Fit();

            // 25 frames a second while the strip moves; twice a second when it does not, for the clock alone. The timer
            // runs only while the window is on the screen.
            _tick = new System.Windows.Forms.Timer { Interval = animate ? 40 : 500 };
            _tick.Tick += (s, e) => { _strip.Step(); var t = Ticked; if (t != null) t(); };
            Shown += (s, e) => _tick.Start();
            FormClosed += (s, e) => { _tick.Stop(); _tick.Dispose(); };
        }

        static Size WorkingArea()
        {
            try { var s = Screen.FromPoint(Cursor.Position); return s == null ? new Size(1024, 768) : s.WorkingArea.Size; }
            catch (Exception) { return new Size(1024, 768); }
        }

        // A drop shadow, because a window without a title bar has no other edge on a dark desktop.
        protected override CreateParams CreateParams
        {
            get { var cp = base.CreateParams; cp.ClassStyle |= 0x00020000; return cp; }   // CS_DROPSHADOW
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            base.OnPaint(e);
            using (var pen = new Pen(Theme.Line)) e.Graphics.DrawRectangle(pen, 0, 0, ClientSize.Width - 1, ClientSize.Height - 1);
        }

        // The screen has the last word, as for the sign-in notice (NoticeForm.Fit): a window that fits keeps sizing
        // itself to its content; one that does not takes the screen and scrolls, so the Close button stays reachable.
        void Fit()
        {
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

        // The start takes longer than it should: the line says so, and a Close button (and Escape) ends the start.
        // The window keeps its centre while it grows.
        public void SaySlow()
        {
            if (Slow) return;
            Slow = true;
            var centre = new Point(Left + Width / 2, Top + Height / 2);
            _line.Text = Strings.T("launch.slow");
            _close = Ui.Button(Strings.T("launch.close"), (s, e) => Close());
            _close.MaximumSize = new Size(Math.Max(Theme.S(60), _inner - _close.Margin.Horizontal), 0);
            _buttons.Controls.Add(_close);
            CancelButton = _close;
            Fit();
            if (Visible) Location = new Point(centre.X - Width / 2, centre.Y - Height / 2);
        }

        // ---- the window's own layout gate (--smoke-loading) ----------------------------------------------------
        // Both states, built without being shown, against one screen: it fits or scrolls, nothing overlaps or reaches
        // outside its box, every text is in the tables and in plain words, the picture is there when the exe carries
        // it, and the slow state closes with Escape. The checks are the sign-in notice's (NoticeForm.cs).
        public static string Findings(bool slow, Size workArea)
        {
            var w = new StringBuilder();
            string where = "loading" + (slow ? " (slow)" : "") + " " + Strings.Language + " at " +
                Theme.Scale.ToString("0.##", System.Globalization.CultureInfo.InvariantCulture) + "x, text " +
                Theme.TextScale.ToString("0.##", System.Globalization.CultureInfo.InvariantCulture) + "x on " + workArea.Width + "x" + workArea.Height;
            using (var f = new LoadingWindow(true, true, workArea))
            {
                var handle = f.Handle;
                GC.KeepAlive(handle);
                if (slow) f.SaySlow();
                f.PerformLayout();
                if (f.Width > workArea.Width || f.Height > workArea.Height)
                    w.AppendLine(where + ": the window is " + f.Width + "x" + f.Height + ", the screen has " + workArea.Width + "x" + workArea.Height);
                if ((f.Wanted.Width > workArea.Width || f.Wanted.Height > workArea.Height) && !f.Scrolls)
                    w.AppendLine(where + ": the content wants " + f.Wanted.Width + "x" + f.Wanted.Height + " and the window does not scroll");
                foreach (var p in NoticeForm.Overlaps(f)) w.AppendLine(where + ": " + p);
                foreach (var p in NoticeForm.Overflows(f)) w.AppendLine(where + ": " + p);
                var texts = NoticeForm.Texts(f).ToList();
                if (texts.Count < (slow ? 3 : 2)) w.AppendLine(where + ": only " + texts.Count + " texts in the window");
                foreach (var t in texts)
                {
                    if (t.StartsWith("[", StringComparison.Ordinal)) w.AppendLine(where + ": a text is missing from the tables: " + t);
                    foreach (var x in PlainWords.Findings(new[] { t })) w.AppendLine(where + ": G-NOINT: " + x);
                }
                foreach (var b in NoticeForm.Buttons(f))
                    if (string.IsNullOrEmpty(b.AccessibleName)) w.AppendLine(where + ": a button has no name for a screen reader");
                if (slow && !(f.CancelButton is Button)) w.AppendLine(where + ": Escape does not close the slow window");
                bool hasArt = NagiArt.Has(Expression, Guide.ArtSize(Theme.Scale));
                bool shown = Kids(f).OfType<PictureBox>().Any(p => p.Image != null);
                if (hasArt && !shown) w.AppendLine(where + ": the exe carries the picture and the window does not show it");
            }
            return w.ToString();
        }

        static System.Collections.Generic.IEnumerable<Control> Kids(Control root)
        {
            foreach (Control c in root.Controls)
            {
                yield return c;
                foreach (var k in Kids(c)) yield return k;
            }
        }
    }

    // A thin bar with a segment that runs across it while the start goes on: the window shows that it is alive
    // without a number it cannot know. With fewer animations chosen (in the app or in Windows) it is not there.
    sealed class ActivityStrip : Control
    {
        const int Cycle = 60;                           // ticks of 40 ms from one side to the other: 2.4 s
        readonly bool _animate;
        int _phase;

        public ActivityStrip(int width, bool animate)
        {
            _animate = animate;
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.UserPaint | ControlStyles.ResizeRedraw, true);
            Size = new Size(width, Theme.S(4));
            BackColor = Theme.Line;
            AccessibleRole = AccessibleRole.ProgressBar;
            AccessibleName = Strings.T("launch.starting");
            TabStop = false;
            Visible = animate;
        }

        public void Step()
        {
            if (!_animate) return;
            _phase = (_phase + 1) % Cycle;
            Invalidate();
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            e.Graphics.Clear(Theme.Line);
            int segment = Math.Max(1, Width / 3);
            int x = (int)((long)(Width + segment) * _phase / Cycle) - segment;
            using (var b = new SolidBrush(Theme.Teal)) e.Graphics.FillRectangle(b, x, 0, segment, Height);
        }
    }

    // Brings a window of this process to the front for a later start of the program (SingleInstance.cs).
    static class WindowFront
    {
        [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr hwnd);
        [DllImport("user32.dll")] static extern bool ShowWindow(IntPtr hwnd, int command);
        [DllImport("user32.dll")] static extern bool IsIconic(IntPtr hwnd);
        [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr hwnd);
        [DllImport("user32.dll")] static extern IntPtr GetLastActivePopup(IntPtr hwnd);
        [DllImport("user32.dll")] static extern bool AllowSetForegroundWindow(int processId);
        const int SW_RESTORE = 9, ASFW_ANY = -1;

        // The later start: the person just started it, so Windows lets it put a window in front. It hands that right
        // on, because the window to bring forward belongs to the first start, and Windows refuses a process that
        // asks for the front on its own while the person works elsewhere (it flashes the task bar button instead).
        public static void AllowAny()
        {
            try { AllowSetForegroundWindow(ASFW_ANY); } catch (Exception) { }
        }

        // Shown when hidden, restored when minimised (to the size it had before: SW_RESTORE, not a normal window),
        // and to the front. When one of its dialogs is open, the dialog comes forward, because the window under it
        // takes no input until the dialog closes.
        public static void Bring(Form f)
        {
            try
            {
                if (!f.Visible) f.Show();
                var h = f.Handle;
                if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
                var popup = GetLastActivePopup(h);
                SetForegroundWindow(popup != IntPtr.Zero && IsWindowVisible(popup) ? popup : h);
            }
            catch (Exception) { }
        }
    }
}
