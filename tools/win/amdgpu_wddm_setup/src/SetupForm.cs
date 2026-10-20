// The setup window: a title bar of its own (Chrome.cs), the steps on the left, the guide panel on the right and one
// screen in the middle (Welcome, Checking, Plan, Working, Restart, Result, Prepare). Every decision comes from the
// engine (docs/gui/interfaces-setup.md): the window runs a plan, shows its checks, notes, settings-impact plan,
// restarts and consents, runs the engine with the consents the user gave, and owns the restart (planned, never
// forced). Plain words only; the engine's technical details go into the support file.
//
// The look is the control application's (the accepted mockup): the design tokens are in Theme.cs and the widgets in
// Controls.cs, and this file only says which of them each screen uses.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text;
using System.Windows.Forms;

namespace AmdgpuWddmSetup
{
    public enum Screen { Welcome, Checking, Plan, Working, Restart, Result, Prepare }

    public sealed class SetupForm : Form
    {
        public const int DesignWidth = 1160, DesignHeight = 740, RailWidth = 220, GuideWidth = 300, ChromeHeight = 48, ButtonBarHeight = 76;

        // The countdown of the restart screen, in seconds (owner, 2026-10-12: "automatic after a visible countdown").
        public const int RestartCountdownSeconds = InstallFlow.RestartCountdownSeconds;

        readonly SetupArgs _args;
        readonly bool _smoke;
        readonly string _ownPackage;
        // Only a render fixture sets this: whether this setup.exe has its own package beside it. null = ask the disk.
        bool? _ownPackagePresent;
        readonly bool _showNagi;
        bool _small;                    // the answer of the small-screen rule the frame in the window was built with

        public Screen Current { get; private set; }
        string _package;
        bool _usePrepared;
        string _preparedDir, _preparedVersion, _preparedProblem;
        string _flow = "install";               // install, repair, continue, prepare
        string _runKind;                        // plan, install, prepare
        EngineClient _client;
        readonly List<EngineClient> _runs = new List<EngineClient>();
        EngineRun _active, _plan;
        EngineResult _planResult, _result;
        ResultView _view;
        bool? _systemTuningChoice;
        // The two shortcut options of the plan screen (owner, 2026-10-12), both on unless the command line says no.
        // The two shortcut answers: null until a switch or the person on the plan screen states one. A run with no
        // answer of its own passes no switch, so the answer an earlier run of the same install saved stands.
        bool? _startMenuShortcut, _desktopShortcut;
        // The wallpaper box of the same card, with the same rule (null: nothing stated).
        bool? _wallpaper;
        // The start-screen picture box of the same card, with the same rule (null: nothing stated).
        bool? _bootLogo;
        // True while the plan screen shown is the one that offers an install, with its options (G-SHORTCUT).
        bool _planOffersInstall;
        bool _restartLater;
        string _restartError, _supportSaved, _supportError;
        // The help card's button could not open Windows Security: the card then says where to find the page.
        bool _helpOpenFailed;
        string _prepDestination = "", _prepFirmware = "";
        bool _prepUseFirmware, _prepFromWelcome;
        DateTime? _runStarted;
        Timer _timer, _anim;
        // The restart this install needs starts by itself after a visible countdown (owner, 2026-10-12): nothing on
        // the restart screen waits for a click. Restart now takes the countdown's place and Later cancels it.
        Timer _countdown;
        int _restartLeft = RestartCountdownSeconds;
        bool _countdownDone;
        IconRow _countdownRow;

        ChromeBar _chrome;
        Panel _rail, _guide, _column;
        FlowLayoutPanel _content, _buttons;
        Label _guideTitle;
        Bubble _guideBubble;
        PictureBox _guideArt;
        CardPanel _guideCard;
        int _guideInner;
        ProgressStrip _progress;
        ResultMark _resultMark;
        IconRow _elapsedRow;
        int _contentWidth;
        // The progress screen on the window (Checking or Working), kept between engine events: what it shows, the
        // frame it was built with, and the controls that an event changes in place (UpdateWorking).
        WorkingView _working;
        string _workingFrame;
        readonly List<IconRow> _stageRows = new List<IconRow>();
        IconRow _noteRow, _waitingRow;
        FlatButton _cancel;
        CardPanel _checkCard;

        // For the gate --smoke-progress: the screens that Show built from nothing.
        public int Builds { get; private set; }

        public SetupForm(SetupArgs args, bool smoke)
        {
            _args = args;
            _systemTuningChoice = args.ApplySystemTuning ? (bool?)true : null;
            _startMenuShortcut = args.StartMenuShortcut;
            _desktopShortcut = args.DesktopShortcut;
            _wallpaper = args.Wallpaper;
            _bootLogo = args.BootLogo;
            _smoke = smoke;
            _ownPackage = args.Package ?? DefaultPackage();
            _package = _ownPackage;
            // On unless the control app's setting says 0 (Native.ShowNagi). A headless render asks nothing of this
            // PC's registry, so it takes the same default and --no-nagi draws the panel without her.
            _showNagi = smoke ? !args.NoNagi : Native.ShowNagi();
            Text = Strings.T("ui.window-title");
            SetWindowIcon();
            AutoScaleMode = AutoScaleMode.None;
            // The window draws its own title bar (ChromeBar): no caption of Windows, and the corners and the frame
            // come from DWM where Windows draws them.
            FormBorderStyle = FormBorderStyle.None;
            MaximizeBox = false;
            StartPosition = FormStartPosition.CenterScreen;
            BackColor = Theme.Back;
            ForeColor = Theme.Text;
            KeyPreview = true;
            ClientSize = WindowSize();
            BuildFrame();
            Strings.LanguageChanged += OnLanguageChanged;
            if (args.Mode == "repair") _flow = "repair";
            else if (args.Mode == "continue") _flow = "continue";
            else if (args.Mode == "prepare-offline") _flow = "prepare";
            if (!smoke)
            {
                _timer = new Timer { Interval = 250 };
                _timer.Tick += (s, e) => { PollEngine(); TurnTip(); };
                _timer.Start();
                _anim = new Timer { Interval = 33 };
                _anim.Tick += (s, e) => Animate();
                _anim.Start();
            }
        }

        // The application's own icon (art\icon\nagi.ico): the icon Windows shows in the task bar and in Alt+Tab, and
        // the icon of the file itself. The window draws its own title bar, so it shows no icon of its own.
        // The file travels twice in this exe: as its Win32 icon (build.ps1, /win32icon, the icon Explorer draws) and
        // as the resource read here. The resource carries all nine sizes, so Windows takes the size it needs from the
        // ones that were drawn for it; Icon.ExtractAssociatedIcon would hand out one 32x32 image, which a 150 % or
        // 200 % screen then rescales. A resource that cannot be read leaves the WinForms default icon in place: an
        // icon is never a reason for a setup not to start.
        void SetWindowIcon()
        {
            try
            {
                var asm = Assembly.GetExecutingAssembly();
                using (var s = asm.GetManifestResourceStream("nagi.ico"))
                {
                    if (s != null) { Icon = new Icon(s); return; }
                }
                var path = Assembly.GetEntryAssembly() != null ? Assembly.GetEntryAssembly().Location : null;
                if (!string.IsNullOrEmpty(path) && File.Exists(path)) Icon = Icon.ExtractAssociatedIcon(path);
            }
            catch (Exception) { }
        }

        // The window at this screen's scale, never larger than the screen it is on. The window has no frame of
        // Windows and cannot be resized or maximised, so a client that is taller than the work area would put its
        // button bar under the taskbar, out of reach: at a 150 % scale the design size is 1740 x 1110 device pixels,
        // which does not fit the 1920 x 1032 work area of a 1080p screen. The content column scrolls, so a shorter
        // window stays usable. A headless render keeps the design size, so every render is the same picture.
        Size WindowSize()
        {
            var want = Theme.Sz(DesignWidth, DesignHeight);
            var work = WorkArea();
            if (work.Width <= 0 || work.Height <= 0) return want;
            int margin = Theme.S(24);
            return new Size(Math.Max(Theme.S(640), Math.Min(want.Width, work.Width - margin)),
                            Math.Max(Theme.S(480), Math.Min(want.Height, work.Height - margin)));
        }

        // The working area of the screen the window is on, in device pixels. --work-area gives it in logical units, so
        // that a headless render draws the same picture on any PC; without it a render takes a screen with room for
        // the whole window, which is the screen every render until this rule assumed.
        Size WorkArea()
        {
            if (_args.WorkWidth > 0 && _args.WorkHeight > 0) return Theme.Sz(_args.WorkWidth, _args.WorkHeight);
            if (_smoke) return Theme.Sz(LargeScreenWidth, LargeScreenHeight);
            try
            {
                var screen = IsHandleCreated ? System.Windows.Forms.Screen.FromHandle(Handle) : System.Windows.Forms.Screen.FromPoint(Cursor.Position);
                return screen.WorkingArea.Size;
            }
            catch (Exception) { return Theme.Sz(LargeScreenWidth, LargeScreenHeight); }
        }

        const int LargeScreenWidth = 2560, LargeScreenHeight = 1440;

        // The owner's rule of 2026-10-12 (SmallScreen.cs): a small screen gets no guide panel at all.
        public bool SmallScreenNow { get { var w = WorkArea(); return SmallScreen.IsDevice(w.Width, w.Height, Theme.Scale); } }

        // Whether the right-hand panel, with or without the guide character, is in the window at all.
        public bool GuidePanelShown { get { return !_small; } }

        // The folder that holds the package, next to the exe. The unpacked release holds setup.exe in its top folder
        // and the whole package in files\ beside it (owner, 2026-10-11), so that is where this looks first. The kept
        // repair set keeps its copy of setup.exe in the package folder itself, so the exe's own folder is the second
        // place. Neither has a manifest when only setup.exe was copied out of the zip: files\ is then the answer, and
        // the welcome screen says to unpack the whole zip (PackageMissing).
        public const string FilesDir = Payload.FilesDir;

        // The package the exe belongs to: <package>\files\ beside setup.exe, or the exe's own folder.
        public static string DefaultPackage()
        {
            var dir = Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location);
            var files = Path.Combine(dir, FilesDir);
            if (HasPackage(files)) return files;
            if (HasPackage(dir)) return dir;
            return files;
        }

        // A folder that holds this release's files: the manifest and the installer the window starts (Payload.cs
        // holds the rule, because a single-file setup checks the folder it unpacked with the same one).
        public static bool HasPackage(string dir) { return Payload.HasPackage(dir); }

        protected override void OnHandleCreated(EventArgs e)
        {
            base.OnHandleCreated(e);
            Dwm.Apply(this);
        }

        protected override void OnShown(EventArgs e)
        {
            base.OnShown(e);
            Start();
        }

        // The first screen for the arguments the window was started with.
        public void Start()
        {
            if (_flow == "continue") { RunInstall(); return; }
            if (_flow == "prepare") { _prepFromWelcome = false; Show(Screen.Prepare); return; }
            Show(Screen.Welcome);
        }

        protected override void OnFormClosing(FormClosingEventArgs e)
        {
            // The engine is never left running without its window: closing waits until it ends.
            if (_client != null && !_client.Finished && e.CloseReason == CloseReason.UserClosing)
            {
                e.Cancel = true;
                SetGuideNote(Strings.T("working.close-blocked"));
                return;
            }
            base.OnFormClosing(e);
        }

        // A restart of this install is still to come, so the files this run needs must stay where they are.
        bool RestartPending()
        {
            if (_view != null && _view.Kind == ViewKind.Restart) return true;
            return _result != null && _result.RestartRequired;
        }

        // A single-file setup unpacked the package it carries into %ProgramData%\amdgpu-wddm\setup\<version>
        // (Payload.cs). That folder goes when the run is over, and stays while a restart is pending: the engine
        // staged its own copy for the continuation, but a person who starts the file again before the restart
        // should still find the release it came from.
        //
        // Every fresh install ends its first phase with a restart, and the phase after it runs from the
        // continuation closure, whose setup.exe is the plain window and carries no payload. So the window that ends
        // the install - the one with no payload of its own - removes the folder of the version it just installed,
        // by name; otherwise the whole unpacked release would stay in %ProgramData% for good, which is what every
        // install of a single file used to do.
        // BD-163: a run that unpacked the package itself removes what it unpacked even when it only planned or dry
        // ran. A plan and a dry run change nothing on the computer, so the unpacked release must not stay behind
        // either. A pending restart is the one reason to keep the folder, and it is read first for that reason.
        protected override void OnFormClosed(FormClosedEventArgs e)
        {
            base.OnFormClosed(e);
            if (RestartPending()) return;
            if (!string.IsNullOrEmpty(_args.PayloadPackage)) { Payload.Cleanup(_args.PayloadPackage); return; }
            if (_smoke || _args.DryRun) return;
            Payload.CleanupVersion(PackageVersion(_package));
        }

        // The window lets go of everything it owns here, and not in OnFormClosing: a close that is cancelled (by
        // another handler) would otherwise leave a window whose language switch no longer rebuilds it, and a form
        // that is disposed without being closed (every form of a headless run) would never let go at all.
        protected override void Dispose(bool disposing)
        {
            if (disposing)
            {
                Strings.LanguageChanged -= OnLanguageChanged;
                foreach (var t in new[] { _timer, _anim, _countdown })
                    if (t != null) { t.Stop(); t.Dispose(); }
                _timer = _anim = _countdown = null;
            }
            base.Dispose(disposing);
        }

        // The wheel scrolls the page wherever the focus is. Windows sends the wheel to the focused window and it
        // bubbles up the parent chain only: from a button of the title bar it never reaches the page.
        protected override void OnMouseWheel(MouseEventArgs e)
        {
            base.OnMouseWheel(e);
            if (_content == null || !_content.VerticalScroll.Visible) return;
            int step = SystemInformation.MouseWheelScrollLines * Theme.S(20);
            int y = -_content.AutoScrollPosition.Y - e.Delta * step / 120;
            _content.AutoScrollPosition = new Point(0, Math.Max(0, Math.Min(_content.VerticalScroll.Maximum, y)));
        }

        const int WM_DPICHANGED = 0x02E0, WM_DISPLAYCHANGE = 0x007E;

        // The window is per-monitor DPI aware (app.manifest) and every size in it is a pixel count taken at the scale
        // the process started with: a move to a screen of another scale rebuilds the window at the new one.
        protected override void WndProc(ref Message m)
        {
            if (m.Msg == WM_DPICHANGED && !_smoke)
            {
                int dpi = (int)((uint)m.WParam.ToInt64() & 0xFFFF);
                if (dpi >= 72 && dpi <= 480 && Math.Abs(dpi / 96f - Theme.Scale) > 0.01f)
                {
                    Theme.Init(dpi / 96f, Theme.SystemTextScale());
                    var suggested = (Native.Rect)System.Runtime.InteropServices.Marshal.PtrToStructure(m.LParam, typeof(Native.Rect));
                    SuspendLayout();
                    // The bounds first, then the frame: the frame measures its page column against the client size it
                    // really has, and the small-screen rule against the screen the window now stands on.
                    Bounds = new Rectangle(suggested.Left, suggested.Top, Math.Max(1, suggested.Right - suggested.Left), Math.Max(1, suggested.Bottom - suggested.Top));
                    BuildFrame();
                    ResumeLayout(true);
                    Show(Current);
                    Theme.DropRetired();
                    m.Result = IntPtr.Zero;
                    return;
                }
            }
            base.WndProc(ref m);
            // The desktop changed under the window: another resolution can turn the screen into a small one, or back.
            if (m.Msg == WM_DISPLAYCHANGE && !_smoke) BeginInvoke(new Action(RebuildIfScreenChanged));
            // The person is dragging the window. Nothing is rebuilt while that lasts: Windows runs its own message
            // loop for a drag, and a rebuild started from inside it would dispose the title bar that the drag began
            // on. The rule is answered once, when the drag ends.
            if (m.Msg == WM_ENTERSIZEMOVE) _moving = true;
            if (m.Msg == WM_EXITSIZEMOVE && !_smoke)
            {
                _moving = false;
                BeginInvoke(new Action(RebuildIfScreenChanged));
            }
        }

        const int WM_ENTERSIZEMOVE = 0x0231, WM_EXITSIZEMOVE = 0x0232;
        bool _moving;                   // Windows is running the loop of a window drag

        // The window was moved, possibly onto another monitor of the same scale: the small-screen rule decides again.
        // A move the person is making waits for the end of the drag (WM_EXITSIZEMOVE above); a move the program
        // itself made is answered at once, after this message, never inside it.
        protected override void OnMove(EventArgs e)
        {
            base.OnMove(e);
            if (_moving || _smoke || IsDisposed || !IsHandleCreated) return;
            BeginInvoke(new Action(RebuildIfScreenChanged));
        }

        bool _building;

        // The screen under the window can change while it is open. The frame is rebuilt only when the rule's answer
        // changed, because a rebuild throws away every control of the screen that is shown.
        void RebuildIfScreenChanged()
        {
            if (_smoke || IsDisposed || _building || _moving || !IsHandleCreated) return;
            if (SmallScreenNow == _small) return;
            SuspendLayout();
            var want = WindowSize();
            if (want != ClientSize) ClientSize = want;
            BuildFrame();
            ResumeLayout(true);
            Show(Current);
        }

        // ---- frame ------------------------------------------------------------------------------------------------

        // Removes every child of a container and disposes it. A control that is only removed keeps its window handle
        // until it is collected (WinForms parks it): a screen rebuilt on every engine event parked thousands of
        // windows, and the process limit of 10 000 ends in "Error creating window handle".
        static void ClearAndDispose(Control c)
        {
            while (c.Controls.Count > 0)
            {
                var k = c.Controls[0];
                c.Controls.RemoveAt(0);
                k.Dispose();
            }
        }

        void BuildFrame()
        {
            _building = true;
            try
            {
            SuspendLayout();
            ClearAndDispose(this);
            Text = Strings.T("ui.window-title");
            var languages = new[] { new[] { "en", "EN", "English" }, new[] { "pl", "PL", "Polski" }, new[] { "ja", "日本語", "日本語" }, new[] { "ko", "한국어", "한국어" } };
            _chrome = new ChromeBar(this, Program.ProductName, Theme.S(ChromeHeight), code => Strings.Language = code, languages,
                Strings.T("ui.minimise"), Strings.T("ui.close-window"));
            _rail = new Panel { Dock = DockStyle.Left, Width = Theme.S(RailWidth), BackColor = Theme.Nav, Padding = Theme.Pad(0, 22, 0, 0) };
            // The owner's rule of 2026-10-12: on a small screen the guide panel is not in the window at all, and the
            // width it would have taken goes to the screen's own column. The panel object stays (the tab order and
            // the frame read it), with no width and not visible, so nothing of it is drawn or reached.
            _small = SmallScreenNow;
            int guideWidth = _small ? 0 : Theme.S(GuideWidth);
            _guide = new Panel { Dock = DockStyle.Right, Width = guideWidth, BackColor = Theme.Back, Padding = Theme.Pad(0, 24, 24, 20), Visible = !_small };
            _column = new Panel { Dock = DockStyle.Fill, BackColor = Theme.Back };
            // The bar wraps: a long translation at a 150 % text size needs a second line of buttons, and a button that
            // does not fit cannot be pressed (the render gate refuses one that leaves the bar).
            _buttons = new FlowLayoutPanel
            {
                Dock = DockStyle.Bottom, FlowDirection = FlowDirection.RightToLeft, WrapContents = true, AutoSize = true,
                AutoSizeMode = AutoSizeMode.GrowAndShrink, MinimumSize = new Size(0, Theme.S(ButtonBarHeight)),
                Padding = Theme.Pad(24, 16, 24, 16), BackColor = Theme.Back,
            };
            _content = new FlowLayoutPanel
            {
                Dock = DockStyle.Fill, FlowDirection = FlowDirection.TopDown, WrapContents = false, AutoScroll = true,
                Padding = Theme.Pad(34, 0, 22, 8), BackColor = Theme.Back,
            };
            Theme.DarkScroll(_content);
            // A line between the page and the buttons. Without it the viewport ended flush against the bar and cut a
            // card title or a button in half at a 150 % text size, which reads as a drawing fault, not as "there is
            // more below".
            var divider = new Panel { Dock = DockStyle.Bottom, Height = Math.Max(1, Theme.S(1)), BackColor = Theme.Line, TabStop = false, AccessibleName = "" };
            // Docking is resolved from the last child to the first: the buttons take the bottom edge, the divider the
            // line above them, and the page what is left.
            _column.Controls.Add(_content);
            _column.Controls.Add(divider);
            _column.Controls.Add(_buttons);
            // The column the screen is built at: what this window really has between the rail and the guide panel,
            // never more than the design width gives. The two differ on a screen too small for the design size, where
            // the window itself is smaller (WindowSize): a column measured from the design width drew text wider than
            // the column it sits in, which is what the tester's 800x600 screen showed (GitHub #38).
            int frameWidth = Math.Min(Theme.S(DesignWidth), ClientSize.Width > 0 ? ClientSize.Width : Theme.S(DesignWidth));
            _contentWidth = Math.Max(Theme.S(260), frameWidth - Theme.S(RailWidth) - guideWidth - _content.Padding.Horizontal - SystemInformation.VerticalScrollBarWidth);
            Controls.Add(_column);
            Controls.Add(_guide);
            Controls.Add(_rail);
            Controls.Add(_chrome);
            if (!_small) BuildGuide();
            else { _guideTitle = null; _guideBubble = null; _guideArt = null; _guideCard = null; _guideInner = 0; }
            ResumeLayout(true);
            }
            finally { _building = false; }
        }

        void BuildGuide()
        {
            ClearAndDispose(_guide);
            int width = Theme.S(GuideWidth) - _guide.Padding.Right;
            // The card takes the whole height of the panel, as the accepted mockup fills its right column; the
            // character sits on its bottom edge, so the hard cut of her picture is the card's edge and not an
            // accidental clip in mid-card.
            int height = Math.Max(Theme.S(200), ClientSize.Height - Theme.S(ChromeHeight) - _guide.Padding.Vertical);
            var card = new CardPanel(null, width, Theme.GuideBack, null, Theme.Teal) { MinimumSize = new Size(width, height), MaximumSize = new Size(width, height) };
            _guideTitle = Ui.Label("", Theme.CardTitle, Theme.Teal, card.Inner);
            _guideTitle.Margin = Theme.Pad(0, 0, 0, 10);
            _guideBubble = new Bubble(card.Inner);
            // The text block stays at the top of the card; the picture is placed against the bottom edge below it.
            card.Body.Dock = DockStyle.None;
            card.Body.Location = new Point(card.Padding.Left, card.Padding.Top);
            // The picture is decorative: the tip beneath it carries the meaning, and a screen reader is given the
            // text, never the picture.
            _guideArt = new PictureBox
            {
                Size = new Size(card.Inner, card.Inner), SizeMode = PictureBoxSizeMode.Zoom, Visible = false, BackColor = Color.Transparent,
                Margin = Theme.Pad(0), AccessibleRole = AccessibleRole.None, AccessibleName = "", TabStop = false,
            };
            _guideInner = card.Inner;
            card.Add(_guideTitle);
            card.Add(_guideBubble);
            card.Controls.Add(_guideArt);
            card.Location = new Point(0, _guide.Padding.Top);
            _guideCard = card;
            _guide.Controls.Add(card);
        }

        // The glyph of each step. The last one is a flag, not a check mark: a check is the mark the rail draws for a
        // step that is done, and the same mark on the step that still waits reads as "already finished".
        static readonly string[] StepGlyphs = { Icons.Welcome, Icons.Shield, Icons.Info, Icons.Download, Icons.Flag };
        static readonly string[] PrepareGlyphs = { Icons.Folder, Icons.Download, Icons.Flag };

        void BuildRail()
        {
            ClearAndDispose(_rail);
            bool prepare = _flow == "prepare";
            var steps = prepare ? new[] { "rail.folder", "rail.prepare", "rail.finish" } : new[] { "rail.welcome", "rail.check", "rail.plan", "rail.install", "rail.finish" };
            var glyphs = prepare ? PrepareGlyphs : StepGlyphs;
            var states = Rail.States(steps.Length, RailIndex(), Current == Screen.Result ? _view : null, _runKind);
            var stack = Ui.Stack(Theme.S(RailWidth));
            stack.Location = new Point(0, _rail.Padding.Top);
            for (int i = 0; i < steps.Length; i++)
            {
                var state = states[i];
                var said = state == RailState.Done ? "rail.done" : state == RailState.Failed ? "rail.failed" : state == RailState.Current ? "rail.current" : null;
                stack.Controls.Add(new RailRow(Theme.S(RailWidth - 20), Theme.S(46), Strings.T(steps[i]), glyphs[i], state, said == null ? null : Strings.T(said)));
            }
            _rail.Controls.Add(stack);
        }

        int RailIndex()
        {
            if (_flow == "prepare")
                switch (Current) { case Screen.Prepare: return 0; case Screen.Working: return 1; default: return 2; }
            switch (Current)
            {
                case Screen.Welcome: return 0;
                case Screen.Checking: return 1;
                case Screen.Plan: return 2;
                case Screen.Working: return 3;
                default: return 4;
            }
        }

        // The language was switched, usually by a button of the title bar: the frame is rebuilt after that button's
        // own click has finished, because the rebuild disposes it.
        void OnLanguageChanged()
        {
            if (_smoke || !IsHandleCreated) { Relanguage(); return; }
            BeginInvoke(new Action(() => { if (!IsDisposed) Relanguage(); }));
        }

        void Relanguage()
        {
            Theme.Fonts();
            BuildFrame();
            Show(Current);
            // The fonts the frame used until a moment ago: they are disposed only now, when no control on the screen
            // holds them any more.
            Theme.DropRetired();
        }

        // ---- screens ----------------------------------------------------------------------------------------------

        static Panel Spacer(int height) { return new Panel { Size = new Size(1, Theme.S(height)), Margin = Theme.Pad(0), BackColor = Theme.Back, TabStop = false }; }

        public void Show(Screen screen)
        {
            // An engine event on the progress screen changes the screen in place: a stage's mark, the note, the Cancel
            // button, the value of the strip or a new check row. The window used to build the whole screen again for
            // every batch of events, also for the 'step' event of every change an install makes: the strip, the rows
            // and the rail became new windows several times a minute, and each new window showed the background for a
            // frame before it drew itself (owner, 2026-10-14: "co 6-8 sekund niebieski pasek od instalatora robi
            // krótkie mignięcie").
            if (screen == Current && (screen == Screen.Checking || screen == Screen.Working) && UpdateWorking(screen == Screen.Checking)) return;
            Builds++;
            // A short crossfade over the screen that leaves (200 ms or less). Only when the screen really changes and
            // never while an engine run is live: a stage list that grows during a run is not a new screen. Off without
            // Windows animation effects, and never in a headless render.
            bool same = screen == Current && _content.Controls.Count > 0;
            FadeOverlay fade = null;
            if (!_smoke && !same && (_client == null || _client.Finished) && IsHandleCreated && Visible && Theme.Animations && _column != null && _column.Width > 0)
            {
                try { fade = new FadeOverlay(_column, 180); } catch (Exception) { fade = null; }
            }
            // A rebuild of the same screen (an engine event, an option the user chose) keeps the reading position and
            // the control the keyboard was on: a user who tabbed to Cancel kept losing it several times a second.
            string focused = same ? FocusKey(ActiveControl) : null;
            int scroll = same ? -_content.AutoScrollPosition.Y : 0;
            Current = screen;
            // A progress screen that is built again (its stage list changed) keeps its strip: a new strip is a new
            // window, and its band would start again at the left.
            var strip = same && (screen == Screen.Checking || screen == Screen.Working) && _progress != null && !_progress.IsDisposed && _progress.Parent == _content ? _progress : null;
            if (strip != null) _content.Controls.Remove(strip);
            _progress = null;
            _resultMark = null;
            _elapsedRow = null;
            _working = null;
            _workingFrame = null;
            _stageRows.Clear();
            _noteRow = _waitingRow = null;
            _cancel = null;
            _checkCard = null;
            _centreContent = false;
            // Every control of the screen that leaves is disposed below, the countdown's line among them: its timer
            // stops here, and BuildRestart starts a new one when the restart screen is built again.
            _countdownRow = null;
            StopCountdown();
            SuspendLayout();
            _content.SuspendLayout();
            ClearAndDispose(_content);
            ClearAndDispose(_buttons);
            AcceptButton = null;
            CancelButton = null;
            // The space above the screen is a spacer, not panel padding: the scroll range of a FlowLayoutPanel does not
            // cover its top padding, which left the end of a long plan out of reach. The spacer is the first control,
            // so no label comes before it.
            _content.Controls.Add(Spacer(26));
            switch (screen)
            {
                case Screen.Welcome: BuildWelcome(); break;
                case Screen.Checking: BuildWorking(true, strip); break;
                case Screen.Plan: BuildPlan(); break;
                case Screen.Working: BuildWorking(false, strip); break;
                case Screen.Restart: BuildRestart(); break;
                case Screen.Result: BuildResult(); break;
                case Screen.Prepare: BuildPrepare(); break;
            }
            if (strip != null && strip.Parent == null) strip.Dispose();
            // The space below the screen is the last control's bottom margin: a spacer panel after a label took the
            // label's text as its name for a screen reader.
            var lastControl = _content.Controls[_content.Controls.Count - 1];
            lastControl.Margin = new Padding(lastControl.Margin.Left, lastControl.Margin.Top, lastControl.Margin.Right, Math.Max(lastControl.Margin.Bottom, Theme.S(16)));
            BuildRail();
            UpdateGuide();
            AssignTabOrder();
            Unnamed(this);
            _content.ResumeLayout(true);
            ResumeLayout(true);
            CentreContent();
            _content.AutoScrollPosition = new Point(0, Math.Max(0, scroll));
            // The keyboard starts on the action the screen is about, not on the close glyph of the title bar.
            var take = focused != null ? Find(_content, focused) ?? Find(_buttons, focused) : null;
            if (take == null) take = AcceptButton as Control;
            if (take == null) { var first = new List<Control>(); Walk(_content, first); take = first.FirstOrDefault(); }
            if (!_smoke && take != null && take.CanSelect) try { ActiveControl = take; } catch (Exception) { }
            if (fade != null) fade.Run(_column);
        }

        // A short screen (the result, the restart, a plan that has nothing to decide) stands in the middle of the
        // page instead of leaving two thirds of the window empty under it.
        bool _centreContent;

        void CentreContent()
        {
            if (!_centreContent || _content.Controls.Count == 0) return;
            var spacer = _content.Controls[0];
            int extent = _content.Controls.Cast<Control>().Where(HasVisibleFlag).Select(c => c.Bottom + c.Margin.Bottom).DefaultIfEmpty(0).Max();
            int room = _content.ClientSize.Height - _content.Padding.Vertical;
            int extra = (room - extent) / 2;
            if (extra <= Theme.S(8)) return;
            spacer.Height = spacer.Height + extra;
            _content.PerformLayout();
        }

        // A control the window can find again after it rebuilt the screen: what a screen reader would call it, and
        // what kind of control it is. Two controls of one screen never share both.
        static string FocusKey(Control c)
        {
            if (c == null) return null;
            var name = string.IsNullOrEmpty(c.AccessibleName) ? c.Text : c.AccessibleName;
            return string.IsNullOrEmpty(name) ? null : c.GetType().Name + "/" + name;
        }

        static Control Find(Control parent, string key)
        {
            foreach (Control k in parent.Controls)
            {
                if (k.CanSelect && FocusKey(k) == key) return k;
                var deep = Find(k, key);
                if (deep != null) return deep;
            }
            return null;
        }

        Label Title(string text)
        {
            var l = Ui.Label(text, Theme.Title, Theme.Text, _contentWidth);
            l.Margin = Theme.Pad(0, 0, 0, 8);
            _content.Controls.Add(l);
            return l;
        }

        StatusLine Status(string text, StatusLine.State state)
        {
            var s = new StatusLine(text, state, _contentWidth);
            _content.Controls.Add(s);
            return s;
        }

        Label Para(string text, Color? color = null)
        {
            var l = Ui.Label(text, Theme.Body, color ?? Theme.Text, _contentWidth);
            l.Margin = Theme.Pad(0, 2, 0, 10);
            _content.Controls.Add(l);
            return l;
        }

        IconRow Line(string glyph, Color colour, string text, Color? textColour = null)
        {
            var r = new IconRow(_contentWidth) { Glyph = glyph, GlyphColor = colour, Head = text, HeadColor = textColour ?? Theme.Text };
            _content.Controls.Add(r.Done());
            return r;
        }

        CardPanel Card(string title, string glyph = null)
        {
            var c = new CardPanel(title, _contentWidth, null, glyph);
            _content.Controls.Add(c);
            return c;
        }

        // The buttons are added in the order right to left, so the primary action comes first and ends up rightmost.
        // chevron: only an action that leads to a next step; the action that ends the window carries none.
        FlatButton AddButton(string id, EventHandler click, bool primary = false, bool chevron = false)
        {
            var b = Ui.Button(Strings.T(id), click, primary, chevron);
            b.Margin = Theme.Pad(10, 4, 0, 4);
            _buttons.Controls.Add(b);
            if (primary) AcceptButton = b;
            return b;
        }

        // A screen rebuilt from inside a control's own event (an option button, a check box): the event finishes
        // first, because the rebuild disposes the control that raised it.
        void ShowSoon(Screen screen)
        {
            if (_smoke || !IsHandleCreated) { Show(screen); return; }
            BeginInvoke(new Action(() => { if (!IsDisposed) Show(screen); }));
        }

        void BuildWelcome()
        {
            bool repair = _flow == "repair";
            Title(Strings.T(repair ? "welcome.repair.title" : "welcome.title"));
            var version = PackageVersion(_ownPackage);
            if (version != null) Status(Strings.T("welcome.version", version), StatusLine.State.Info);
            Para(Strings.T(repair ? "welcome.repair.body" : "welcome.body"), Theme.Dim);
            if (_args.DryRun) Line(Icons.Warning, Theme.Warn, Strings.T("welcome.dry-run"), Theme.Warn);
            // setup.exe without its files\ folder: somebody copied the exe out of the zip, or opened it inside the
            // zip viewer. Say so in words instead of letting the install run fail on a missing script.
            // A prepared folder brings its own files, so the sentence appears only while the package beside setup.exe
            // is the chosen source; otherwise it would contradict a valid folder and an enabled check button.
            bool ownMissing = _ownPackagePresent.HasValue ? !_ownPackagePresent.Value : !HasPackage(_ownPackage);
            if (ownMissing && !_usePrepared) Line(Icons.Warning, Theme.Warn, Strings.T("welcome.files-missing"), Theme.Warn);
            // The hero promises: what the release brings, in three lines a reader takes in at a glance.
            Line(Icons.Chip, Theme.Teal, Strings.T("welcome.promise.driver"));
            Line(Icons.Lock, Theme.Teal, Strings.T("welcome.promise.shown"));
            // The last promise carries the space before the card: a bare spacer panel after a row would take the text
            // of the nearest label before it as its name (G-A11Y).
            Line(Icons.Folder, Theme.Teal, Strings.T("welcome.promise.offline")).Margin = Theme.Pad(0, 3, 0, 18);

            var source = Card(Strings.T("welcome.source.title"), Icons.Download);
            var own = Ui.Radio(Strings.T(repair ? "welcome.source.kept" : "welcome.source.package"), source.Inner);
            var prepared = Ui.Radio(Strings.T("welcome.source.prepared"), source.Inner);
            own.Checked = !_usePrepared;
            prepared.Checked = _usePrepared;
            source.Add(own);
            source.Add(prepared);
            var path = new ThemeBox(source.Inner - Theme.S(190), Strings.T("welcome.prepared.path"), true);
            path.Box.Text = _preparedDir ?? "";
            var browse = Ui.Button(Strings.T("ui.choose-folder"), (s, e) =>
            {
                var dir = PickFolder(Strings.T("welcome.source.prepared"), _preparedDir);
                if (dir == null) return;
                SetPrepared(dir);
                Show(Screen.Welcome);
            });
            browse.Margin = Theme.Pad(0, 8, 0, 0);
            // The folder belongs to the prepared-folder choice only.
            path.Box.Enabled = browse.Enabled = _usePrepared;
            source.Add(Ui.Row(path, browse));
            if (_usePrepared)
            {
                if (_preparedProblem == null && _preparedDir != null)
                    source.Row(Icons.Check, Theme.Good, Strings.T("welcome.prepared.ok", _preparedVersion ?? "-"));
                else if (_preparedDir == null)
                    source.Row(Icons.Info, Theme.Dim, Strings.T("welcome.prepared.choose"));
                else
                    source.Row(Icons.Warning, Theme.Warn, Strings.T(_preparedProblem));
            }
            own.CheckedChanged += (s, e) => { if (own.Checked && _usePrepared) { _usePrepared = false; _package = _ownPackage; ShowSoon(Screen.Welcome); } };
            prepared.CheckedChanged += (s, e) => { if (prepared.Checked && !_usePrepared) { _usePrepared = true; _package = _preparedProblem == null && _preparedDir != null ? _preparedDir : _ownPackage; ShowSoon(Screen.Welcome); } };

            // Preparing a folder for another PC copies this package, so it needs the files too.
            if (!repair && !ownMissing) _content.Controls.Add(Ui.Quiet(Strings.T("welcome.prepare-link"), Icons.Folder, (s, e) => { _prepFromWelcome = true; _flow = "prepare"; Show(Screen.Prepare); }));

            var check = AddButton("welcome.check", (s, e) => RunPlan(), true, true);
            check.Enabled = _usePrepared ? (_preparedDir != null && _preparedProblem == null) : !ownMissing;
            CancelButton = AddButton("ui.close", (s, e) => Close());
        }

        void SetPrepared(string dir)
        {
            string version;
            _preparedDir = dir;
            _preparedProblem = PreparedFolder.Check(dir, out version);
            _preparedVersion = version;
            _usePrepared = true;
            _package = _preparedProblem == null ? dir : _ownPackage;
        }

        // Checking (a plan run) and Working (a real run) share the progress screen. WorkingView says what it shows; the
        // controls that an engine event can change are kept for UpdateWorking. strip: the strip of the same screen
        // built before, or null for a new one.
        void BuildWorking(bool checking, ProgressStrip strip)
        {
            var v = WorkingNow(checking);
            Title(Strings.T(v.TitleId));
            Status(Strings.T(v.BodyId), StatusLine.State.Busy);
            if (v.PendingRemovals) PendingRemovalLine();
            _progress = strip ?? new ProgressStrip(_contentWidth);
            _progress.AccessibleName = Strings.T(v.TitleId);
            _content.Controls.Add(_progress);
            _progress.SetDone(v.Progress);
            _elapsedRow = Line(Icons.Clock, Theme.Dim, Strings.T("working.elapsed", Elapsed()), Theme.Dim);
            _working = v;
            _workingFrame = WorkingFrame(v);
            if (checking)
            {
                _checkCard = Card(Strings.T("checking.list"), Icons.Shield);
                foreach (var c in v.Checks) AddCheck(_checkCard, c);
                if (v.Checks.Count == 0) _waitingRow = _checkCard.Row(Icons.Ring, Theme.Dim, Strings.T("checking.waiting"));
                return;
            }
            var stages = Card(Strings.T("working.stages"), Icons.Gear);
            for (int i = 0; i < v.StageIds.Count; i++)
            {
                var row = stages.Row(Icons.Ring, Theme.Dim, "");
                StageLook(row, v.StageIds[i], v.Marks[i]);
                _stageRows.Add(row);
            }
            _noteRow = Line(Icons.Info, Theme.Dim, Strings.T(v.NoteId), Theme.Dim);
            if (v.CancelShown)
            {
                _cancel = AddButton("ui.cancel", (s, e) => CancelRun());
                _cancel.Enabled = v.CancelEnabled;
            }
        }

        WorkingView WorkingNow(bool checking)
        {
            return WorkingView.Of(_active, _flow, checking, _client != null && _client.CancelRequested, PendingRemovalsCancelled());
        }

        // The frame of a progress screen and what the window built it for: a screen of another width or language is
        // built again.
        string WorkingFrame(WorkingView v)
        {
            return v.Frame + "|" + _contentWidth + "|" + Strings.Language;
        }

        // The look of one stage row: its glyph, its colours and its weight say whether the stage is done, stopped the
        // run, runs now or is still to come.
        static void StageLook(IconRow row, string id, StageMark mark)
        {
            row.Head = Strings.Has("stage." + id) ? Strings.T("stage." + id) : Strings.T("stage.other");
            row.Glyph = mark == StageMark.Done ? Icons.Check : mark == StageMark.Failed ? Icons.Cancel : mark == StageMark.Current ? Icons.Play : Icons.Ring;
            row.GlyphColor = mark == StageMark.Done ? Theme.Good : mark == StageMark.Failed ? Theme.Accent : mark == StageMark.Current ? Theme.Teal : Theme.Dim;
            row.HeadColor = mark == StageMark.Failed ? Theme.Accent : mark == StageMark.Waiting ? Theme.Dim : Theme.Text;
            row.Strong = mark == StageMark.Current;
            row.Done();
        }

        // An engine event on the progress screen that is on the window: the controls that show something new change,
        // and nothing is built. False when the screen must be built again: another frame (the title, the stage list,
        // the notice, the Cancel button), checks that are not the listed ones with more after them, or controls that
        // are gone (a new frame of the window after a change of language, scale or screen).
        bool UpdateWorking(bool checking)
        {
            if (_working == null || _working.Checking != checking || _progress == null || _progress.IsDisposed || _progress.Parent != _content) return false;
            var v = WorkingNow(checking);
            if (WorkingFrame(v) != _workingFrame || (checking && !v.ExtendsChecks(_working))) return false;
            for (int i = 0; i < v.Marks.Count; i++)
                if (v.Marks[i] != _working.Marks[i])
                {
                    StageLook(_stageRows[i], v.StageIds[i], v.Marks[i]);
                    _stageRows[i].Invalidate();
                }
            if (_noteRow != null && v.NoteId != _working.NoteId)
            {
                _noteRow.Head = Strings.T(v.NoteId);
                _noteRow.Done().Invalidate();
            }
            if (_cancel != null) _cancel.Enabled = v.CancelEnabled;
            _progress.SetDone(v.Progress);
            if (checking && v.Checks.Count > _working.Checks.Count)
            {
                if (_waitingRow != null)
                {
                    _checkCard.Body.Controls.Remove(_waitingRow);
                    _waitingRow.Dispose();
                    _waitingRow = null;
                }
                for (int i = _working.Checks.Count; i < v.Checks.Count; i++) AddCheck(_checkCard, v.Checks[i]);
                AssignTabOrder();
                Unnamed(_checkCard);
            }
            _working = v;
            return true;
        }

        // Cancel asks the engine to stop at its next safe point; the note then says so and the button is off.
        void CancelRun()
        {
            if (_client != null && _client.RequestCancel()) Show(Screen.Working);
        }

        string Elapsed()
        {
            var since = _runStarted.HasValue ? DateTime.UtcNow - _runStarted.Value : TimeSpan.Zero;
            if (since < TimeSpan.Zero) since = TimeSpan.Zero;
            return ((int)since.TotalMinutes) + ":" + since.Seconds.ToString("00");
        }

        void AddCheck(CardPanel card, CheckRow c)
        {
            var glyph = c.Result == "ok" ? Icons.Check : c.Result == "warn" ? Icons.Warning : Icons.Cancel;
            var colour = c.Result == "ok" ? Theme.Good : c.Result == "warn" ? Theme.Warn : Theme.Accent;
            var row = card.Row(glyph, colour, CheckText(c.Id));
            row.AccessibleName = Strings.T(c.Result == "ok" ? "check.mark.ok" : c.Result == "warn" ? "check.mark.warn" : "check.mark.fail") + ": " + CheckText(c.Id);
        }

        public static string CheckText(string id)
        {
            return id != null && Strings.Has("check." + id) ? Strings.T("check." + id) : Strings.T("check.other");
        }

        // The help card of a check the user fixes in Windows (HelpCards.cs): the steps, the pictures this exe carries,
        // and a button that opens the page in Windows Security. The button opens a page and changes nothing, so it
        // asks the user for no permission (G-CONSENT); the user turns the setting off there, and Try again or Install
        // reads it again.
        void AddHelpCard(HelpCard h)
        {
            if (h == null) return;
            var card = Card(Strings.T(h.TitleId), Icons.Shield);
            card.Add(Ui.Label(Strings.T(h.BodyId), null, Theme.Text, card.Inner)).Margin = Theme.Pad(0, 0, 0, 6);
            // The pictures start under the step text, past the icon column of the row (IconRow: icon and gap).
            int indent = Theme.S(30);
            bool pictures = false;
            for (int i = 0; i < h.Steps.Length; i++)
            {
                var step = h.Steps[i];
                card.Row(Icons.Chevron, Theme.Teal, (i + 1) + ". " + Strings.T(step.TextId));
                var image = HelpArt.Load(step.Image);
                if (image == null) continue;
                pictures = true;
                var shot = card.Add(new ShotBox(image, Math.Min(card.Inner - indent, Theme.S(HelpCards.ShotWidth)), Strings.T("help.picture", i + 1)));
                shot.Margin = new Padding(indent, Theme.S(2), 0, Theme.S(8));
            }
            if (pictures && Strings.Language != "en") card.Row(Icons.Info, Theme.Dim, Strings.T("help.pictures-english"), null, false, Theme.Small).HeadColor = Theme.Dim;
            if (h.NoteId != null) card.Row(Icons.Info, Theme.Dim, Strings.T(h.NoteId)).HeadColor = Theme.Dim;
            var open = card.Add(Ui.Button(Strings.T(h.ActionId), (s, e) => OpenHelp(h)));
            open.Margin = Theme.Pad(0, 8, 0, 2);
            if (_helpOpenFailed) card.Row(Icons.Warning, Theme.Warn, Strings.T("help.open-failed")).HeadColor = Theme.Warn;
        }

        void OpenHelp(HelpCard h)
        {
            if (_smoke) return;
            bool failed = !Native.OpenUri(h.ActionUri);
            if (failed == _helpOpenFailed) return;
            _helpOpenFailed = failed;
            ShowSoon(Current);
        }

        void BuildPlan()
        {
            _planOffersInstall = false;
            var d = _plan != null ? _plan.Decision : null;
            var version = d != null ? d.PackageVersion : PackageVersion(_package);
            var action = d != null ? d.Action : "install";
            switch (action)
            {
                case "upgrade": Title(Strings.T("plan.upgrade.title", d.InstalledVersion ?? "-", version)); break;
                case "repair": Title(Strings.T("plan.repair.title", version)); break;
                case "resume": Title(Strings.T("plan.resume.title", version)); break;
                case "already": Title(Strings.T("plan.already.title", version)); break;
                case "verify": Title(Strings.T("plan.verify.title", version)); break;
                default: Title(Strings.T("plan.install.title", version)); break;
            }
            if (action == "already")
            {
                _centreContent = true;
                Status(Strings.T("plan.already.body"), StatusLine.State.Good);
                AddButton("plan.repair-again", (s, e) => { _flow = "repair"; RunPlan(); }, true, true);
                CancelButton = AddButton("ui.close", (s, e) => Close());
                return;
            }
            if (action == "verify")
            {
                _centreContent = true;
                Status(Strings.T("plan.verify.body"), StatusLine.State.Info);
                AddButton("restart.now", (s, e) => RestartNow(), true, true);
                CancelButton = AddButton("ui.close", (s, e) => Close());
                return;
            }

            _planOffersInstall = true;
            // Checks: all passed, or the ones that need attention. Nothing on this screen asks the user for a
            // permission any more (owner, 2026-10-12): the Install button is enabled from the first moment, and
            // pressing it is the consent for what the card below states.
            var warn = _plan == null ? new List<CheckRow>() : _plan.Checks.Where(c => c.Result != "ok").ToList();
            if (warn.Count == 0) Status(Strings.T("plan.checks.ok"), StatusLine.State.Good);
            else Status(Strings.T("plan.checks.attention", warn.Count), warn.Any(c => c.Result == "fail") ? StatusLine.State.Bad : StatusLine.State.Warn);
            if (warn.Count > 0)
            {
                var checks = Card(Strings.T("plan.checks.title"), Icons.Shield);
                foreach (var c in warn) AddCheck(checks, c);
                AddHelpCard(HelpCards.ForChecks(warn.Select(c => c.Id)));
            }

            Button go = null;
            var steps = Card(Strings.T("plan.what.title"), Icons.Info);
            int restarts = d != null ? d.Restarts : 0;
            steps.Row(Icons.Restart, restarts > 0 ? Theme.Warn : Theme.Good,
                restarts > 2 ? Strings.T("plan.restarts.many", restarts) :
                    Strings.T(restarts <= 0 ? "plan.restarts.0" : restarts == 1 ? "plan.restarts.1" : "plan.restarts.2"),
                restarts > 0 ? Strings.T("plan.restarts.choice") : null);
            // What the installation changes about the way Windows starts, as plain information (owner, 2026-10-12:
            // "Przestan wymagac potwierdzen od uzytkownika"). The engine reads the same two states and acts on them
            // by itself: test mode is turned on, and BitLocker protection is paused for the restarts of the install.
            // The window states them here so that nothing happens to the PC that this screen did not say.
            bool testModeOff = _plan != null && _plan.Checks.Any(c => c.Id == "testsigning.inactive");
            // The Microsoft Visual C++ runtime the driver's programs need: when the plan found it missing or too
            // old, the install brings it (owner, 2026-10-12), and this screen says so before it happens.
            bool vcMissing = _plan != null && _plan.Checks.Any(c => c.Id == "vcruntime.missing");
            foreach (var id in InstallFlow.PlanChangeIds(testModeOff, PlanBitLocker(), vcMissing))
            {
                switch (id)
                {
                    case "plan.change.test-mode": steps.Row(Icons.Lock, Theme.Teal, Strings.T(id), Strings.T("plan.change.test-mode.note")); break;
                    case "plan.change.test-mode.on": steps.Row(Icons.Check, Theme.Good, Strings.T(id)); break;
                    case "plan.change.bitlocker": steps.Row(Icons.Shield, Theme.Teal, Strings.T(id)); break;
                    case "plan.change.vcruntime": steps.Row(Icons.Download, Theme.Teal, Strings.T(id)); break;
                    default: steps.Row(Icons.Shield, Theme.Warn, Strings.T(id)); break;
                }
            }
            var fw = d != null ? d.FirmwareSource : null;
            if (fw == "download" || fw == "folder" || fw == "package-folder") steps.Row(Icons.Download, Theme.Teal, Strings.T("plan.firmware." + fw));

            var notes = ReleaseNotes.Load(_package, Strings.Language);
            var nc = Card(Strings.T("plan.notes.title"), Icons.Sparkle);
            if (notes == null) nc.Row(Icons.Info, Theme.Dim, Strings.T("plan.notes.none"));
            else
            {
                if (notes.EnglishFallback) nc.Row(Icons.Info, Theme.Warn, Strings.T("plan.notes.english"));
                // The notes stand at their full height: the card already carries their title, so the block itself is
                // unnamed and a screen reader does not read that title twice.
                nc.Add(new NotesPanel(notes.Source, nc.Inner, ""));
            }

            var sc = Card(Strings.T("plan.settings.title"), Icons.Gear);
            var s0 = _plan != null ? _plan.Settings : null;
            if (s0 == null) sc.Row(Icons.Warning, Theme.Warn, Strings.T("plan.settings.none"));
            else
            {
                sc.Row(Icons.Info, Theme.Teal, Strings.T("plan.settings.summary", s0.Kept, s0.Added, s0.Updated, s0.Unchanged + s0.Command), null, true);
                foreach (var line in SettingsView.Lines(s0))
                {
                    var text = Strings.T(line.TextId, line.Args);
                    var decision = line.DecisionId != null && Strings.Has(line.DecisionId) ? Strings.T(line.DecisionId) : null;
                    sc.Row(Icons.Bullet, Theme.Dim, text, decision);
                }
                sc.Row(Icons.Lock, Theme.Dim, Strings.T("plan.settings.note"), null, false, Theme.Small);
            }

            // Where the control application can be started from afterwards (owner, 2026-10-12). Both options are on
            // when the screen opens, and neither holds the install back: a person who turns both off still installs
            // the driver by pressing Install.
            var shortcuts = Card(Strings.T("shortcuts.title"), Icons.Sparkle);
            var startMenu = Ui.Check(Strings.T("shortcuts.start-menu"), shortcuts.Inner);
            startMenu.Checked = _startMenuShortcut ?? (d == null || d.StartMenuShortcut);
            startMenu.CheckedChanged += (s, e) => { _startMenuShortcut = startMenu.Checked; };
            shortcuts.Add(startMenu);
            var desktop = Ui.Check(Strings.T("shortcuts.desktop"), shortcuts.Inner);
            desktop.Checked = _desktopShortcut ?? (d == null || d.DesktopShortcut);
            desktop.CheckedChanged += (s, e) => { _desktopShortcut = desktop.Checked; };
            shortcuts.Add(desktop);
            shortcuts.Add(Ui.Label(Strings.T("shortcuts.note"), Theme.Small, Theme.Dim, shortcuts.Inner));
            // The Nagi wallpaper (owner, 2026-10-13): on, in the same group, an option and not a question.
            var wallpaper = Ui.Check(Strings.T("shortcuts.wallpaper"), shortcuts.Inner);
            wallpaper.Checked = _wallpaper ?? (d == null || d.Wallpaper);
            wallpaper.CheckedChanged += (s, e) => { _wallpaper = wallpaper.Checked; };
            shortcuts.Add(wallpaper);
            shortcuts.Add(Ui.Label(Strings.T("shortcuts.wallpaper.note"), Theme.Small, Theme.Dim, shortcuts.Inner));
            // The picture on the start screen (owner, 2026-10-14: on by default in the installer, like the
            // wallpaper). The box opens on the answer the engine reports, which for a reinstall or an upgrade is
            // the answer the installed driver already carries.
            var bootLogo = Ui.Check(Strings.T("shortcuts.boot-logo"), shortcuts.Inner);
            bootLogo.Checked = _bootLogo ?? (d == null || d.BootLogo);
            bootLogo.CheckedChanged += (s, e) => { _bootLogo = bootLogo.Checked; };
            shortcuts.Add(bootLogo);
            shortcuts.Add(Ui.Label(Strings.T("shortcuts.boot-logo.note"), Theme.Small, Theme.Dim, shortcuts.Inner));

            var tuning = Card(Strings.T("tuning.title"), Icons.Gear);
            var tune = Ui.Check(Strings.T("tuning.opt-in"), tuning.Inner);
            tune.Checked = _systemTuningChoice ?? (d != null && d.SystemTuning);
            tune.CheckedChanged += (s, e) => { _systemTuningChoice = tune.Checked; };
            tuning.Add(tune);
            tuning.Add(Ui.Label(Strings.T("tuning.details"), Theme.Small, Theme.Dim, tuning.Inner));
            tuning.Add(Ui.Label(Strings.T("tuning.restore"), Theme.Small, Theme.Dim, tuning.Inner));

            // The bar flows right to left, so the first button added is the rightmost: the primary action takes the
            // place the eye and the mouse go to first, and the dismissive one stands away from it.
            go = AddButton(action == "upgrade" ? "plan.go.upgrade" : action == "repair" ? "plan.go.repair" : action == "resume" ? "plan.go.resume" : "plan.go.install", (s, e) => RunInstall(), true, true);
            // The primary action is never off: a person who opened this installer wants to install (owner,
            // 2026-10-12). A machine the installer must not change is refused by the preflight, which fails a check
            // and ends the run; it is not a question on this screen.
            go.Enabled = true;
            CancelButton = AddButton("ui.close", (s, e) => Close());
            if (_flow != "continue") AddButton("ui.back", (s, e) => Show(Screen.Welcome));
        }

        // For the render gate: the plan screen has no control that asks the user for a permission, and its primary
        // action is enabled (owner, 2026-10-12). Both are findings, so neither can come back.
        public IEnumerable<string> ConsentFindings()
        {
            if (Current != Screen.Plan) yield break;
            // The check boxes the plan screen may carry are the ones of InstallFlow.PlanOptionIds: the optional
            // Windows tuning and the two shortcuts. Each has a default, none of them holds the install back, and no
            // other control of this kind may appear.
            var allowed = InstallFlow.PlanOptionIds.Select(id => Strings.T(id)).ToList();
            foreach (var c in Descendants(_content))
            {
                if (c is RadioButton) yield return "the plan screen has an option button: \"" + c.Text + "\"";
                else if (c is CheckBox && !allowed.Contains(c.Text)) yield return "the plan screen has a check box: \"" + c.Text + "\"";
            }
            var primary = AcceptButton as Control;
            if (primary != null && !primary.Enabled) yield return "the primary action \"" + primary.Text + "\" is off";
        }

        // G-SHORTCUT (owner, 2026-10-12): the plan screen offers both shortcut options, both of them on when the
        // screen opens, and the Install button does not depend on either. The render gate reports what is wrong.
        public IEnumerable<string> ShortcutFindings()
        {
            // Only the plan screen that offers an install: the screens for a version that is already installed and
            // checked, or that waits for a restart, carry no options at all.
            if (Current != Screen.Plan || !_planOffersInstall) yield break;
            var boxes = Descendants(_content).OfType<CheckBox>().ToList();
            foreach (var id in new[] { "shortcuts.start-menu", "shortcuts.desktop", "shortcuts.wallpaper", "shortcuts.boot-logo" })
            {
                var box = boxes.FirstOrDefault(b => b.Text == Strings.T(id));
                if (box == null) { yield return "the plan screen has no option for " + id; continue; }
                if (!box.Checked) yield return id + " is off when the screen opens";
                if (!box.Enabled) yield return id + " cannot be changed";
            }
        }

        static IEnumerable<Control> Descendants(Control parent)
        {
            if (parent == null) yield break;
            foreach (Control k in parent.Controls)
            {
                yield return k;
                foreach (var deep in Descendants(k)) yield return deep;
            }
        }

        void BuildRestart()
        {
            _centreContent = true;
            _resultMark = NewResultMark(Theme.Teal, Icons.Restart);
            _content.Controls.Add(_resultMark);
            Title(Strings.T(_view.TitleId));
            Para(Strings.T(_view.BodyId), Theme.Dim);
            PendingRemovalNotice();
            if (_restartError != null) Line(Icons.Warning, Theme.Warn, Strings.T("restart.failed"), Theme.Warn);
            if (_restartLater)
            {
                Line(Icons.Check, Theme.Teal, Strings.T("restart.later.done"), Theme.Teal);
                AddButton("ui.close", (s, e) => Close(), true);
                CancelButton = AcceptButton;
                return;
            }
            Line(Icons.Info, Theme.Dim, Strings.T("restart.save-work"), Theme.Dim);
            // The restart happens by itself (owner, 2026-10-12): a visible countdown runs down and Windows restarts
            // at its end, whether anybody presses anything or not. The two buttons only shorten it or call it off.
            // A screen that cannot restart Windows at all (no offer in the result view) shows no countdown.
            if (_view.OfferRestart)
            {
                // Once it has run out it does not start again: a restart Windows refused would otherwise be asked
                // for every second, and the line then says that the restart is up to the user.
                if (_countdownDone) Line(Icons.Info, Theme.Dim, Strings.T("restart.countdown.over"), Theme.Dim);
                else _countdownRow = Line(Icons.Restart, Theme.Teal, Strings.T("restart.countdown", _restartLeft), Theme.Teal);
                AddButton("restart.now", (s, e) => RestartNow(), true);
                if (!_countdownDone) StartCountdown();
            }
            CancelButton = AddButton("restart.later", (s, e) => { _restartLater = true; StopCountdown(); Show(Screen.Restart); });
        }

        // The countdown of the restart screen. A headless render draws the first second and starts no timer, so a
        // render is a picture and never a restart.
        void StartCountdown()
        {
            StopCountdown();
            if (_smoke || _args.DryRun) return;
            _countdown = new Timer { Interval = 1000 };
            _countdown.Tick += (s, e) =>
            {
                _restartLeft--;
                if (_restartLeft <= 0) { _restartLeft = 0; _countdownDone = true; StopCountdown(); RestartNow(); return; }
                if (_countdownRow != null && !_countdownRow.IsDisposed)
                {
                    _countdownRow.Head = Strings.T("restart.countdown", _restartLeft);
                    _countdownRow.Done();
                    _countdownRow.Invalidate();
                }
            };
            _countdown.Start();
        }

        void StopCountdown()
        {
            if (_countdown == null) return;
            _countdown.Stop();
            _countdown.Dispose();
            _countdown = null;
        }

        // For the render gate and the tests: the seconds the restart screen shows, and whether its countdown runs.
        public int RestartSecondsLeft { get { return _restartLeft; } }
        public bool CountdownRunning { get { return _countdown != null && _countdown.Enabled; } }

        // The result icon, with its entrance when the window is shown and may animate: it is at its full size at
        // once in a headless render and on a PC whose animation effects are off.
        ResultMark NewResultMark(System.Drawing.Color colour, string glyph)
        {
            return new ResultMark(colour, glyph, Theme.S(64)) { Grow = !_smoke && Theme.Animations ? 0f : 1f };
        }

        void RestartNow()
        {
            if (_smoke) return;
            _restartError = Native.RequestPlannedRestart();
            if (_restartError == null) { Close(); return; }
            if (_view == null) _view = new ResultView { Kind = ViewKind.Restart, TitleId = "result.verify-before-restart.title", BodyId = "result.verify-before-restart.body", OfferRestart = true };
            Show(Screen.Restart);
        }

        void BuildResult()
        {
            var v = _view;
            var colour = v.Kind == ViewKind.Success ? Theme.Good : v.Kind == ViewKind.Problem ? Theme.Accent : v.Kind == ViewKind.Cancelled ? Theme.Warn : Theme.Teal;
            var glyph = v.Kind == ViewKind.Success ? Icons.Check : v.Kind == ViewKind.Problem ? Icons.Cancel : v.Kind == ViewKind.Cancelled ? Icons.Warning : Icons.Info;
            _resultMark = NewResultMark(colour, glyph);
            _content.Controls.Add(_resultMark);
            _centreContent = v.FailedChecks.Length == 0;
            Title(Strings.T(v.TitleId));
            if (v.AlreadyInstalled) Para(Strings.T("result.already-installed", v.PackageVersion ?? "-"));
            Para(Strings.T(v.BodyId), Theme.Dim);
            PendingRemovalNotice();
            if (v.FailedChecks.Length > 0)
            {
                var card = Card(Strings.T("result.failed-checks"), Icons.Shield);
                foreach (var id in v.FailedChecks) AddCheck(card, new CheckRow { Id = id, Result = "fail" });
                AddHelpCard(HelpCards.ForChecks(v.FailedChecks));
            }
            if (v.NothingChanged) Status(Strings.T("result.nothing-changed"), StatusLine.State.Good);
            else if (v.ChangesUnknown) Status(Strings.T("result.changes-unknown"), StatusLine.State.Warn);
            else if (v.ChangesMade && (v.Kind == ViewKind.Problem || v.Kind == ViewKind.Cancelled)) Status(Strings.T("result.changes-made"), StatusLine.State.Warn);
            if (v.Kind != ViewKind.Success || _flow == "prepare")
            {
                Line(Icons.Info, Theme.Dim, Strings.T("result.support"), Theme.Dim);
                // The engine collected the details of a graphics chip that did not start by itself: say so, so that
                // the user knows the file below already holds them and nobody has to be asked for more.
                if (_runs.Any(r => r.Result != null && !string.IsNullOrEmpty(r.Result.Diagnostics)))
                    Line(Icons.Info, Theme.Dim, Strings.T("result.support.diagnostics"), Theme.Dim);
                if (_supportSaved != null) Line(Icons.Check, Theme.Good, Strings.T("result.support.saved", _supportSaved), Theme.Good);
                if (_supportError != null) Line(Icons.Warning, Theme.Warn, Strings.T("result.support.failed"), Theme.Warn);
            }

            // The default button: Repair for a problem that offers it, Try again when a new try can help, else Close.
            bool repairFirst = v.OfferRepair && v.Kind == ViewKind.Problem;
            bool retryFirst = v.OfferRetry && v.RetryHelps && !v.OfferRepair && v.Kind != ViewKind.Success && v.Kind != ViewKind.Information;
            // Right to left: the action that leads on, then the other offers, then Close. Close ends the window, so
            // it carries no chevron even when it is the default.
            if (v.OfferRepair) AddButton("result.repair", (s, e) => { _flow = "repair"; RunPlan(); }, repairFirst, true);
            if (v.OfferRetry) AddButton("result.retry", (s, e) => { if (_flow == "prepare") Show(Screen.Prepare); else RunPlan(); }, retryFirst, true);
            CancelButton = AddButton("ui.close", (s, e) => Close(), !repairFirst && !retryFirst);
            if (v.Kind != ViewKind.Success || _flow == "prepare") AddButton("result.save-support", (s, e) => SaveSupport());
        }

        void PendingRemovalNotice()
        {
            if (PendingRemovalsCancelled()) PendingRemovalLine();
        }

        bool PendingRemovalsCancelled()
        {
            return (_active != null && _active.PendingRemovalsCancelled > 0) || _runs.Any(r => r.Run != null && r.Run.PendingRemovalsCancelled > 0);
        }

        void PendingRemovalLine() { Line(Icons.Info, Theme.Dim, Strings.T("install.pending-removals-cancelled"), Theme.Dim); }

        void BuildPrepare()
        {
            Title(Strings.T("prepare.title"));
            Status(Strings.T("prepare.body"), StatusLine.State.Info);
            var card = Card(Strings.T("prepare.destination"), Icons.Folder);
            var dest = new ThemeBox(card.Inner - Theme.S(190), Strings.T("prepare.destination"), false);
            dest.Box.Text = _prepDestination;
            Button go = null;
            dest.Box.TextChanged += (s, e) => { _prepDestination = dest.Box.Text.Trim(); if (go != null) go.Enabled = PrepareReady(); };
            var pick = Ui.Button(Strings.T("ui.choose-folder"), (s, e) => { var d = PickFolder(Strings.T("prepare.destination"), _prepDestination); if (d != null) { dest.Box.Text = d; } });
            pick.Margin = Theme.Pad(0, 8, 0, 0);
            card.Add(Ui.Row(dest, pick));
            card.Row(Icons.Info, Theme.Dim, Strings.T("prepare.destination.hint"), null, false, Theme.Small);

            var fwCard = Card(Strings.T("prepare.firmware.title"), Icons.Download);
            var use = Ui.Check(Strings.T("prepare.use-firmware"), fwCard.Inner);
            use.Checked = _prepUseFirmware;
            fwCard.Add(use);
            var fw = new ThemeBox(fwCard.Inner - Theme.S(190), Strings.T("prepare.firmware.folder"), !_prepUseFirmware);
            fw.Box.Text = _prepFirmware;
            fw.Box.TextChanged += (s, e) => { _prepFirmware = fw.Box.Text.Trim(); if (go != null) go.Enabled = PrepareReady(); };
            var fwBrowse = Ui.Button(Strings.T("ui.choose-folder"), (s, e) => { var d = PickFolder(Strings.T("prepare.firmware.folder"), _prepFirmware); if (d != null) { fw.Box.Text = d; use.Checked = true; } });
            fwBrowse.Margin = Theme.Pad(0, 8, 0, 0);
            fwBrowse.AccessibleName = Strings.T("prepare.firmware.folder") + ": " + Strings.T("ui.choose-folder");
            fwCard.Add(Ui.Row(fw, fwBrowse));
            fwCard.Row(Icons.Info, Theme.Dim, Strings.T(_prepUseFirmware ? "prepare.firmware.folder-note" : "prepare.firmware.download-note"), null, false, Theme.Small);
            use.CheckedChanged += (s, e) => { _prepUseFirmware = use.Checked; ShowSoon(Screen.Prepare); };

            go = AddButton("prepare.go", (s, e) => RunPrepare(), true, true);
            go.Enabled = PrepareReady();
            CancelButton = AddButton("ui.close", (s, e) => Close());
            if (_prepFromWelcome) AddButton("ui.back", (s, e) => { _flow = "install"; Show(Screen.Welcome); });
        }

        bool PrepareReady() { return _prepDestination.Length > 0 && (!_prepUseFirmware || _prepFirmware.Length > 0); }

        // ---- engine runs ------------------------------------------------------------------------------------------

        string RunRoot()
        {
            return _args.RunRoot ?? Path.Combine(Path.GetTempPath(), "amdgpu-wddm-setup");
        }

        EngineClient NewClient()
        {
            var c = new EngineClient(RunRoot(), !_smoke && Native.IsElevated());
            _runs.Add(c);
            return c;
        }

        void RunPlan()
        {
            var args = new List<string> { "-Plan" };
            args.AddRange(SetupArgs.TuningArguments(_systemTuningChoice));
            if (_flow == "repair") args.Add("-Repair");
            args.AddRange(_args.EngineArgs);
            StartRun("plan", Path.Combine(_package, "installer", "install.ps1"), args, Screen.Checking);
        }

        void RunInstall()
        {
            var args = new List<string>();
            args.AddRange(SetupArgs.TuningArguments(_systemTuningChoice));
            args.AddRange(InstallFlow.ShortcutArguments(_startMenuShortcut, _desktopShortcut));
            args.AddRange(InstallFlow.WallpaperArguments(_wallpaper));
            args.AddRange(InstallFlow.BootLogoArguments(_bootLogo));
            if (_flow == "repair") args.Add("-Repair");
            // Pressing Install is the consent (owner, 2026-10-12), so the switches that used to carry a user's answer
            // are given on every run. The engine of this package no longer needs them; an engine of an older release,
            // which a kept repair set may still hold, reads them and asks nothing either.
            args.AddRange(InstallFlow.ConsentArguments(PlanBitLocker()));
            if (_args.DryRun) args.Add("-DryRun");
            args.AddRange(_args.EngineArgs);
            StartRun("install", Path.Combine(_package, "installer", "install.ps1"), args, Screen.Working);
        }

        // The BitLocker state of the plan ('on', 'unknown', 'off', 'suspended' or null when no plan ran).
        string PlanBitLocker()
        {
            return _plan != null && _plan.Decision != null ? _plan.Decision.BitLocker : null;
        }

        void RunPrepare()
        {
            var args = new List<string> { "-Destination", _prepDestination };
            if (_prepUseFirmware) { args.Add("-FirmwareDir"); args.Add(_prepFirmware); }
            StartRun("prepare", Path.Combine(_ownPackage, "installer", "prepare-offline.ps1"), args, Screen.Working);
        }

        // Starts one engine run and shows its first screen. When the engine cannot start (a run folder that cannot be
        // made, a package folder that is gone, a launch that Windows refuses), the run has no result: the result screen
        // says so and stays; the support file says why when the run folder exists.
        bool StartRun(string kind, string script, List<string> args, Screen first)
        {
            _runKind = kind;
            _supportSaved = _supportError = null;
            _runStarted = DateTime.UtcNow;
            EngineClient client = null;
            try
            {
                client = NewClient();
                client.Start(script, args);
            }
            catch (Exception e)
            {
                if (client != null) { try { File.WriteAllText(client.OutputFile, "start failed: " + e); } catch (Exception) { } }
                _client = null;
                _active = client != null ? client.Run : null;
                _view = ResultView.For(null, _active);
                Show(Screen.Result);
                return false;
            }
            _client = client;
            _active = client.Run;
            Show(first);
            return true;
        }

        // --smoke-start-failure: the three ways the window starts the engine.
        public void StartForTest(string kind)
        {
            if (kind == "plan") RunPlan();
            else if (kind == "install") RunInstall();
            else { _prepDestination = Path.Combine(Path.GetTempPath(), "amdgpu-wddm-never-written"); RunPrepare(); }
        }
        public ResultView View { get { return _view; } }

        void PollEngine()
        {
            if (_client == null || IsDisposed || !IsHandleCreated) return;
            if (!_client.Poll()) return;
            EngineChanged();
        }

        // New events of the live run, or its end. A live run's screen changes in place (Show, UpdateWorking).
        void EngineChanged()
        {
            if (!_client.Finished) { Show(Current); return; }
            Finish(_client);
        }

        // The moving band of the progress strip, the entrance of the result icon and the elapsed time. Without the
        // Windows animation effects the strip stands still and the icon is there at once.
        void Animate()
        {
            if (IsDisposed) return;
            if (_progress != null && !_progress.IsDisposed && _progress.IsHandleCreated)
            {
                if (Theme.Animations) _progress.Step(0.012f);
                if (_elapsedRow != null)
                {
                    var text = Strings.T("working.elapsed", Elapsed());
                    if (_elapsedRow.Head != text) { _elapsedRow.Head = text; _elapsedRow.AccessibleName = text; _elapsedRow.Invalidate(); }
                }
            }
            if (_resultMark != null && !_resultMark.IsDisposed && _resultMark.IsHandleCreated && _resultMark.Grow < 1f)
            {
                _resultMark.Grow = Theme.Animations ? _resultMark.Grow + 0.12f : 1f;
                _resultMark.Invalidate();
            }
        }

        // --smoke-engine: a run made outside the window, shown as the window would show it.
        public void Adopt(EngineClient client, string kind)
        {
            _runs.Add(client);
            _runKind = kind;
            if (kind == "prepare") _flow = "prepare";
            Finish(client.Run, client.Result);
        }

        // A finished run: the screen follows the bound result only.
        public void Finish(EngineClient client)
        {
            _client = null;
            Finish(client.Run, client.Result);
        }

        public void Finish(EngineRun run, EngineResult result)
        {
            _active = run;
            _result = result;
            _view = ResultView.For(result, run);
            if (!_systemTuningChoice.HasValue && run.Decision != null) _systemTuningChoice = run.Decision.SystemTuning;
            if (_runKind == "plan")
            {
                _plan = run;
                _planResult = result;
            }
            else if (_view.Kind == ViewKind.Plan && run.Decision != null)
            {
                // needs-consent from a real run: the plan screen again, with this run's decision.
                _plan = run;
                _planResult = result;
            }
            switch (_view.Kind)
            {
                case ViewKind.Plan: Show(Screen.Plan); break;
                case ViewKind.Restart: Show(Screen.Restart); break;
                default: Show(Screen.Result); break;
            }
        }

        // ---- guide, support ---------------------------------------------------------------------------------------

        void UpdateGuide()
        {
            var causes = new List<SetupCause>();
            switch (Current)
            {
                case Screen.Welcome: case Screen.Prepare: causes.Add(SetupCause.Welcome); break;
                case Screen.Checking: case Screen.Working: causes.Add(SetupCause.WorkInProgress); break;
                case Screen.Restart: causes.Add(SetupCause.PendingRestart); break;
                case Screen.Plan: causes.Add(SetupCause.PageTip); break;
                case Screen.Result:
                    if (_view != null && _view.Kind == ViewKind.Problem) causes.Add(_result != null && _result.Outcome == "verify-failed" ? SetupCause.VerificationFailed : SetupCause.InstallStopped);
                    else if (_view != null && _view.Kind == ViewKind.Success && _result != null && _result.Outcome == "verified") causes.Add(SetupCause.UpgradeDone);
                    else causes.Add(SetupCause.PageTip);
                    break;
            }
            var cause = SetupGuide.Pick(causes);
            // On a small screen there is no panel to fill: the cause is still picked and kept, so that a move to a
            // larger screen fills the panel with this screen's tip and not with the first one of the window.
            if (_guideTitle != null) _guideTitle.Text = Strings.T(_showNagi ? "guide.title" : "guide.title.plain");
            // The words first, then the picture: the tip's height decides how much room is left for her.
            // The checks take less than a minute; an installation can take minutes. The guide says the same as the page.
            if (_guideBubble != null)
                _guideBubble.Say(GuideText(cause == SetupCause.Welcome && _flow == "prepare" ? "guide.prepare" : cause == SetupCause.WorkInProgress && Current == Screen.Checking ? "guide.checking" : SetupGuide.TipId(cause, Current == Screen.Working ? _tipTurn : 0)));
            ShowGuideArt(cause);
            GuideCause = cause;
        }

        public SetupCause GuideCause { get; private set; }

        // The text of one tip. The name tip carries the meaning of the name as an argument: that phrase is one string
        // (brand.expansion), so a new wording is one line in each table.
        static string GuideText(string id)
        {
            return id == SetupGuide.NameTipId ? Strings.T(id, Strings.T("brand.expansion")) : Strings.T(id);
        }

        // The turn of the Working screen's tips: the next one after SetupGuide.TipSeconds on that screen, back to the
        // first on any other screen. The window's poll timer drives it (never a headless render, whose fixture sets
        // the turn it draws).
        int _tipTurn;
        DateTime _tipSince;

        void TurnTip()
        {
            if (IsDisposed || Current != Screen.Working || GuideCause != SetupCause.WorkInProgress) { _tipTurn = 0; _tipSince = DateTime.MinValue; return; }
            var now = DateTime.UtcNow;
            if (_tipSince == DateTime.MinValue) { _tipSince = now; return; }
            if ((now - _tipSince).TotalSeconds < SetupGuide.TipSeconds) return;
            _tipTurn++;
            _tipSince = now;
            UpdateGuide();
        }

        // The still picture of this screen's cause, with "Show Nagi" on and its resource in the exe. Without either
        // one the slot is empty and the panel is the text-only panel. The picture is drawn at most 1.25 times the
        // @256 art, so it never blurs; the welcome screen gives her the whole width of the panel.
        void ShowGuideArt(SetupCause cause)
        {
            if (_guideArt == null || _guideCard == null) return;
            // Never wider than the card's inner width: the slot of the two lowest scales was 4 to 5 px wider than the
            // panel it sits in and its right edge was clipped.
            int box = Math.Min(_guideInner, SetupGuide.ArtBox(Theme.Scale, Current == Screen.Welcome || Current == Screen.Prepare));
            int size = SetupGuide.ArtSize(Theme.Scale);
            var expression = SetupGuide.Expression(cause);
            var img = _showNagi && NagiArt.Has(expression, size) ? NagiArt.Load(expression, size) : null;
            // Without her the tip is a plain card: a balloon with a tail would point at nobody.
            if (_guideBubble != null) _guideBubble.WithTail = img != null;
            // The picture stands on the bottom edge of the card and against its right edge, and never over the tip
            // above it: a card too short for the whole slot gets a smaller picture, not an overlap.
            _guideCard.PerformLayout();
            int bottom = _guideCard.Height - _guideCard.Depth - 1;
            int top = _guideCard.Body.Bottom + Theme.S(8);
            box = Math.Max(0, Math.Min(box, bottom - top));
            _guideArt.Size = new Size(box, box);
            _guideArt.Location = new Point(Math.Max(_guideCard.Padding.Left, _guideCard.Width - _guideCard.Padding.Right - box), bottom - box);
            _guideArt.Image = img;
            _guideArt.Visible = img != null && box > Theme.S(40);
            // The tail of the speech bubble points at her head, not at the background beside her.
            if (_guideBubble != null && _guideArt.Visible)
                _guideBubble.TailAt = (float)(_guideArt.Left - _guideCard.Padding.Left + box / 2) / Math.Max(1, _guideInner);
        }

        // For the render gate: whether the guide panel drew the picture, and whether it was meant to.
        public bool GuideArtShown { get { return _guideArt != null && HasVisibleFlag(_guideArt) && _guideArt.Image != null; } }
        public bool GuideArtWanted { get { return GuidePanelShown && _showNagi && NagiArt.Has(SetupGuide.Expression(GuideCause), SetupGuide.ArtSize(Theme.Scale)); } }

        // The owner's rule of 2026-10-12, as the render gate reads it: on a small screen the right-hand panel takes no
        // width, is not visible and holds nothing; on a larger screen it is there with its card. Both directions are
        // findings, so neither can drift.
        public IEnumerable<string> SmallScreenFindings()
        {
            var work = WorkArea();
            bool small = SmallScreen.IsDevice(work.Width, work.Height, Theme.Scale);
            if (small != _small)
                yield return "the frame was built for a " + (_small ? "small" : "large") + " screen, the rule says " + (small ? "small" : "large");
            if (small)
            {
                if (_guide != null && (_guide.Width > 0 || HasVisibleFlag(_guide) || _guide.Controls.Count > 0))
                    yield return "the guide panel is in the window on a " + work.Width + "x" + work.Height + " screen (width " + (_guide == null ? 0 : _guide.Width) + ")";
                if (_guideArt != null || _guideCard != null) yield return "the guide card was built on a small screen";
                if (GuideArtShown) yield return "the guide art shows on a small screen";
            }
            else
            {
                if (_guide == null || !HasVisibleFlag(_guide) || _guide.Width <= 0) yield return "the guide panel is missing on a screen that has room for it";
                if (_guideCard == null) yield return "the guide card is missing on a screen that has room for it";
            }
            // Whatever the screen, the column the page was built at must fit the room the window really has.
            int room = _content == null ? 0 : _content.ClientSize.Width - _content.Padding.Horizontal;
            if (_content != null && _contentWidth > room + Theme.S(2))
                yield return "the page column is " + _contentWidth + " px, the window has room for " + room;
        }

        void SetGuideNote(string text) { if (_guideBubble != null) _guideBubble.Say(text); }

        void SaveSupport()
        {
            using (var dlg = new SaveFileDialog { Filter = "Zip (*.zip)|*.zip", FileName = "amdgpu-wddm-setup-" + DateTime.UtcNow.ToString("yyyyMMdd'T'HHmmss'Z'") + ".zip", InitialDirectory = Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory) })
            {
                if (dlg.ShowDialog(this) != DialogResult.OK) return;
                WriteSupport(dlg.FileName);
            }
            Show(Screen.Result);
        }

        public bool WriteSupport(string path)
        {
            try
            {
                SupportFile.Write(path, _runs, Summary(), Redactor.ForThisPc());
                _supportSaved = path;
                _supportError = null;
                return true;
            }
            catch (Exception e) { _supportError = e.Message; _supportSaved = null; return false; }
        }

        public string Summary()
        {
            var w = new StringBuilder();
            w.AppendLine(Program.ProductName + " " + Program.VersionText + ", " + DateTime.UtcNow.ToString("o"));
            w.AppendLine("arguments: " + string.Join(" ", _args.Raw));
            w.AppendLine("flow: " + _flow + "; screen: " + Current + "; language: " + Strings.Language + "; elevated: " + (!_smoke && Native.IsElevated()));
            w.AppendLine("package: " + _package + (_usePrepared ? " (prepared folder)" : ""));
            if (_plan != null && _plan.Decision != null)
            {
                var d = _plan.Decision;
                w.AppendLine("decision: " + d.Action + ", installed " + (d.InstalledVersion ?? "-") + ", package " + d.PackageVersion + ", phase " + (d.Phase ?? "-") +
                    ", restarts " + d.Restarts + ", consents " + string.Join(",", d.Consents) + ", firmware " + d.FirmwareSource + ", compatibility " +
                    (d.CompatibilityOk ? "ok" : "not verified (" + string.Join(",", d.CompatibilityReasons) + ")"));
            }
            if (_plan != null && _plan.Settings != null)
                foreach (var r in _plan.Settings.Rows) w.AppendLine("  setting " + r.Group + "\\" + r.Name + ": " + r.Decision + ", current " + Json.Show(r.Current) + ", value " + Json.Show(r.Value) + ", default " + Json.Show(r.Default));
            if (_result != null)
                w.AppendLine("result: " + _result.Outcome + " exit " + _result.ExitCode + " " + _result.MessageId + ", mutated " + _result.Mutated + ", step " + (_result.Step ?? "-") + ", detail " + (_result.Detail ?? "-"));
            if (_active != null)
            {
                foreach (var c in _active.Checks) w.AppendLine("  check " + c.Id + " " + c.Result + ": " + c.Detail);
                if (_active.Problems.Count > 0) w.AppendLine("event problems: " + string.Join("; ", _active.Problems));
            }
            if (_restartError != null) w.AppendLine("restart request: " + _restartError);
            return w.ToString();
        }

        // ---- helpers ----------------------------------------------------------------------------------------------

        string PickFolder(string description, string start)
        {
            if (_smoke) return null;
            using (var dlg = new FolderBrowserDialog { Description = description, ShowNewFolderButton = true })
            {
                if (!string.IsNullOrEmpty(start) && Directory.Exists(start)) dlg.SelectedPath = start;
                return dlg.ShowDialog(this) == DialogResult.OK ? dlg.SelectedPath : null;
            }
        }

        public static string PackageVersion(string package)
        {
            try
            {
                var m = Json.Parse(File.ReadAllText(Path.Combine(package ?? "", "manifest.json"), Encoding.UTF8));
                return Json.Str(m, "version");
            }
            catch (Exception) { return null; }
        }

        // Tab order: the screen from top to bottom, then its buttons from left to right, and the title bar last. The
        // first Tab of the window must not land on the close glyph, which Space would then press.
        void AssignTabOrder()
        {
            int i = 0;
            _column.TabIndex = i++;
            _rail.TabIndex = i++; _rail.TabStop = false;
            _guide.TabIndex = i++; _guide.TabStop = false;
            _chrome.TabIndex = i++;
            _content.TabIndex = 0;
            _buttons.TabIndex = 1;
            int n = 0;
            foreach (Control c in _content.Controls) Number(c, ref n);
            var bs = _buttons.Controls.Cast<Control>().ToList();
            for (int k = 0; k < bs.Count; k++) bs[k].TabIndex = bs.Count - 1 - k;
        }

        static void Number(Control c, ref int n)
        {
            c.TabIndex = n++;
            int m = 0;
            foreach (Control k in c.Controls) Number(k, ref m);
        }

        // Layout panels have no accessible name in WinForms. This does not stop Windows' window proxy, which names an
        // unnamed window after the nearest label before it in its parent: the layout itself keeps bare panels away from
        // labels (G-A11Y checks it). The empty name keeps WinForms from offering a name of its own.
        static void Unnamed(Control c)
        {
            foreach (Control k in c.Controls)
            {
                if (k is Panel && k.AccessibleName == null) k.AccessibleName = "";
                Unnamed(k);
            }
        }

        // ---- the progress gate (--smoke-progress) ------------------------------------------------------------------
        // An install run of 30 s and a plan run of 10 s, played against a window that is never shown. Animate runs at
        // the 33 ms ticks of its timer, and EngineChanged at each 250 ms tick of the poll that read new events, as
        // PollEngine calls it. The events are the ones the engine writes: a 'step' for every change (install.ps1), a
        // stage now and then, the cancel offer and its end, a check of the plan every second; the Cancel button is
        // pressed once. The strip stays one control with one window handle, no event builds the screen again, and
        // the strip is drawn again only when what it shows changes: its value, or the moving band while no stage is
        // known. A stage that the list does not know builds the screen once, with the same strip.
        public static List<string> ProgressGate(string runRoot, StringBuilder log)
        {
            var findings = new List<string>();
            Strings.Language = "en";
            foreach (var plan in new[] { false, true })
                using (var form = new SetupForm(SetupArgs.Parse(new[] { "--run-root", runRoot }), true))
                    form.PlayRun(plan, plan ? 10000 : 30000, log, findings);
            return findings;
        }

        void PlayRun(bool plan, int ms, StringBuilder log, List<string> findings)
        {
            var name = plan ? "plan" : "install";
            var client = NewClient();
            _client = client;
            _active = client.Run;
            _runKind = name;
            _runStarted = DateTime.UtcNow;
            int seq = 0;
            Action<string, string> ev = (type, fields) => client.Run.Apply("{\"schema\":\"" + EngineRun.EventSchema + "\",\"invocation\":\"" + client.Invocation +
                "\",\"seq\":" + (++seq) + ",\"utc\":\"2026-10-14T08:00:00Z\",\"type\":\"" + type + "\"" + (fields.Length > 0 ? "," + fields : "") + "}");
            ev("start", "\"mode\":\"" + (plan ? "plan" : "run") + "\",\"gui\":true,\"dry_run\":false,\"package\":\"P\",\"contract\":\"" + EngineRun.Contract + "\",\"phase\":null");
            if (!plan)
            {
                ev("stage", "\"id\":\"preflight\"");
                ev("decision", "\"action\":\"install\"");
                ev("cancel", "\"available\":true,\"where\":\"before-changes\"");
            }
            Show(plan ? Screen.Checking : Screen.Working);
            CreateHandles(this);
            PerformLayout();
            var strip = _progress;
            int made = ProgressStrip.Made, builds = Builds, drawn = strip.Invalidations, ticks = 0, batches = 0, changes = 0;
            var checks = new[] { "package.ok", "admin.ok", "windows.ok", "other" };
            var stages = new Dictionary<int, string> { { 7000, "test-signing" }, { 14000, "install" }, { 21000, "firmware" }, { 28000, "files" } };
            float? shown = strip.Indeterminate ? (float?)null : strip.Value;
            for (int t = 1; t <= ms; t++)
            {
                if (t % 33 == 0) { Animate(); ticks++; }
                if (t % 250 != 0) continue;
                int before = seq;
                if (plan && t % 1000 == 0)
                    ev("check", "\"id\":\"" + checks[(t / 1000) % checks.Length] + "\",\"result\":\"ok\",\"name\":\"x\",\"detail\":\"gate\"");
                if (!plan && t % 2000 == 0) ev("step", "\"description\":\"x\",\"dry_run\":false");
                if (!plan && t == 500) ev("install-action", "\"action\":\"install\",\"boot_id\":41");
                string stage;
                if (!plan && stages.TryGetValue(t, out stage)) ev("stage", "\"id\":\"" + stage + "\"");
                if (!plan && t == 14000) ev("cancel", "\"available\":false,\"where\":\"driver-install\"");
                if (seq != before) { batches++; EngineChanged(); }
                if (!plan && t == 1000) CancelRun();
                float? now = strip.Indeterminate ? (float?)null : strip.Value;
                if (now != shown) { changes++; shown = now; }
            }
            int redrawn = strip.Invalidations - drawn;
            int expected = changes + (strip.Indeterminate && Theme.Animations ? ticks : 0);
            log.AppendLine(name + ", " + ms / 1000 + " s: " + ticks + " animation ticks, " + batches + " polls with new events, " + (Builds - builds) +
                " screens built, " + (ProgressStrip.Made - made) + " strips made, " + strip.Handles + " handle of the strip, drawn " + redrawn +
                " times (" + changes + " value changes" + (strip.Indeterminate ? ", " + (Theme.Animations ? ticks + " band steps" : "no band steps: animation effects are off") : "") + ")");
            if (_progress != strip || ProgressStrip.Made != made) findings.Add(name + ": the strip was made again");
            if (strip.Handles != 1) findings.Add(name + ": the strip got " + strip.Handles + " window handles");
            if (Builds != builds) findings.Add(name + ": the screen was built " + (Builds - builds) + " times");
            if (redrawn != expected) findings.Add(name + ": the strip was drawn " + redrawn + " times, " + expected + " expected");
            if (plan)
            {
                int rows = _checkCard == null ? -1 : _checkCard.Body.Controls.Count;
                if (_waitingRow != null || rows != ms / 1000) findings.Add("plan: " + rows + " check rows for " + ms / 1000 + " checks");
                return;
            }
            if (changes != stages.Count) findings.Add("install: " + changes + " value changes for " + stages.Count + " stages");
            var marks = string.Join(",", _working.Marks.Select(m => m.ToString()));
            if (marks != "Done,Done,Done,Done,Current,Waiting,Waiting,Waiting" || _working.NoteId != "working.cancel.requested" || _cancel == null || _cancel.Enabled)
                findings.Add("install: the screen shows " + marks + ", " + _working.NoteId + ", Cancel " + (_cancel != null && _cancel.Enabled ? "on" : "off"));
            // A stage that the list does not know: the list changes, so the screen is built again, with the same strip.
            ev("stage", "\"id\":\"extras\"");
            EngineChanged();
            log.AppendLine("install, a stage the list does not know: " + (Builds - builds) + " screen built, " + (ProgressStrip.Made - made) + " strips made, " +
                strip.Handles + " handle of the strip, stages " + string.Join(",", _working.StageIds));
            if (Builds - builds != 1 || _progress != strip || ProgressStrip.Made != made || strip.Handles != 1 || _working.StageIds.IndexOf("extras") != 5)
                findings.Add("install: a new stage did not keep the strip");
        }

        // ---- headless rendering and checks (G-RENDER, G-A11Y, G-NOINT) --------------------------------------------

        // Builds a screen from fixture data without running an engine.
        public void ShowFixture(Fixture f)
        {
            _flow = f.Flow;
            _runKind = f.RunKind;
            _usePrepared = f.PreparedDir != null;
            _preparedDir = f.PreparedDir;
            _preparedProblem = f.PreparedProblem;
            _preparedVersion = f.PreparedVersion;
            _package = f.Package ?? _ownPackage;
            _ownPackagePresent = f.FilesMissing ? (bool?)false : null;
            _restartLater = f.RestartLater;
            // The second the countdown of the restart screen is drawn at, so a render is one fixed picture.
            _restartLeft = f.RestartSecondsLeft > 0 ? f.RestartSecondsLeft : RestartCountdownSeconds;
            _prepDestination = f.PrepareDestination ?? "";
            _prepUseFirmware = f.PrepareFirmware != null;
            _prepFirmware = f.PrepareFirmware ?? "";
            _prepFromWelcome = f.Flow == "prepare";
            _supportSaved = f.SupportSaved;
            _tipTurn = f.TipTurn;
            // A fixture is one picture built from nothing, never an update of the screen of the fixture before it.
            _working = null;
            // The elapsed time of a run in a drawn screen: a fixed 42 seconds, so every render of the same screen is
            // the same picture.
            _runStarted = DateTime.UtcNow - TimeSpan.FromSeconds(42);
            if (f.PlanEvents != null) { _plan = Fixtures.Run(f.PlanEvents); _planResult = Fixtures.Result(f.PlanResult); }
            _active = f.Events != null ? Fixtures.Run(f.Events) : _plan;
            if (f.Result != null)
            {
                _result = Fixtures.Result(f.Result);
                _view = ResultView.For(_result, _active);
            }
            else if (f.NoResult) { _result = null; _view = ResultView.For(null, _active); }
            Show(f.Screen);
        }

        public string Render(string path)
        {
            CreateHandles(this);
            // A form that is never shown lays out once without the scroll bar; the second pass wraps the text to the
            // width that remains beside it, as the shown window does.
            PerformLayout();
            foreach (Control c in _content.Controls) c.PerformLayout();
            _content.PerformLayout();
            PerformLayout();
            using (var bmp = new Bitmap(Width, Height))
            {
                DrawToBitmap(bmp, new Rectangle(0, 0, Width, Height));
                bmp.Save(path, System.Drawing.Imaging.ImageFormat.Png);
            }
            var w = new StringBuilder();
            foreach (var o in Overlaps(this)) w.AppendLine("overlap: " + o);
            int room = _content.ClientSize.Width - _content.Padding.Horizontal;
            foreach (Control c in _content.Controls)
                if (HasVisibleFlag(c) && c.Width > room) w.AppendLine("too wide: " + Describe(c) + " " + c.Width + " px, room " + room);
            var last = _content.Controls.Cast<Control>().Where(HasVisibleFlag).OrderBy(c => c.Bottom).LastOrDefault();
            if (last != null)
            {
                int extent = _content.DisplayRectangle.Bottom + _content.Padding.Bottom;
                if (last.Bottom > extent || last.Bottom > _content.ClientSize.Height && !_content.VerticalScroll.Visible)
                    w.AppendLine("outside the scroll range: " + Describe(last) + " ends at " + last.Bottom + ", range " + extent + ", scroll bar " + _content.VerticalScroll.Visible);
            }
            foreach (Control b in _buttons.Controls)
                if (b.Right > _buttons.ClientSize.Width || b.Left < 0) w.AppendLine("button outside the bar: " + Describe(b));
            w.Append(ChromeCheck());
            return w.ToString();
        }

        // G-CHROME: the window draws its own title bar and nothing of Windows' caption is left; the bar carries the
        // minimise and close glyphs, both inside the window and beside the language choice, and the rail stays inside
        // its column. A window that loses its own close button cannot be closed with the mouse at all.
        public string ChromeCheck()
        {
            var w = new StringBuilder();
            if (FormBorderStyle != FormBorderStyle.None) w.AppendLine("chrome: the window still has a caption of Windows (" + FormBorderStyle + ")");
            if (_chrome == null || !HasVisibleFlag(_chrome) || _chrome.Top != 0 || _chrome.Height <= 0) w.AppendLine("chrome: no title bar of our own at the top of the window");
            else
            {
                var glyphs = _chrome.Controls.Cast<Control>().OfType<FlatButton>().Where(b => b.Kind == ButtonKind.Chrome).ToList();
                if (glyphs.Count != 2) w.AppendLine("chrome: " + glyphs.Count + " window glyphs, 2 expected (minimise, close)");
                foreach (var b in glyphs)
                {
                    if (string.IsNullOrWhiteSpace(b.AccessibleName)) w.AppendLine("chrome: a window glyph has no accessible name");
                    if (b.Right > _chrome.Width || b.Left < 0 || b.Bottom > _chrome.Height) w.AppendLine("chrome: a window glyph is outside the title bar");
                }
                var pills = _chrome.Controls.Cast<Control>().OfType<FlowLayoutPanel>().SelectMany(p => p.Controls.Cast<Control>())
                    .Where(p => (p.Tag as string) == "lang").ToList();
                if (pills.Count != Strings.Languages.Length) w.AppendLine("chrome: " + pills.Count + " language choices, " + Strings.Languages.Length + " expected");
                if (glyphs.Count > 0 && pills.Count > 0 && pills.Max(p => p.Parent.Left + p.Right) > glyphs.Min(b => b.Left))
                    w.AppendLine("chrome: the language choice runs into the window glyphs");
            }
            foreach (Control c in _rail.Controls)
                foreach (Control row in c.Controls)
                    if (row.Right > _rail.Width) w.AppendLine("chrome: a step of the rail is wider than the rail");
            return w.ToString();
        }

        // G-BACKDROP (BD-137), in two halves: a control that paints itself leaves no pixel of its client rectangle
        // unpainted, and the colour it clears itself in is the colour the window really paints under it
        // (WrongBackdrop, below).
        //
        // Control.WmPaint runs two layers for a control with ControlStyles.UserPaint: the background layer, and that
        // one only when ControlStyles.Opaque is not set, then OnPaint (.NET Framework reference source, Control.cs,
        // WmPaint and PaintWithErrorHandling). ButtonBase sets Opaque, so for every button, option button and check
        // box of this window the whole background is OnPaint's own work. On the screen each control paints into a
        // buffer that BufferedGraphicsManager hands to the next control with the pixels of the last one still in it,
        // so a control that does not clear itself shows what was painted there before: the language pills each
        // carried the pill before them and a word from the step rail, and the two option buttons of "Where the files
        // come from" carried each other's text. A render into one bitmap cannot show this, because there the parent
        // has already drawn the right pixels underneath, which is why 27 screens x 22 renders passed over it; this
        // gate paints every control alone, over a colour no screen uses, and fails on whatever is left of it.
        public List<string> Backdrops()
        {
            var problems = new List<string>();
            var under = new Dictionary<Control, Bitmap>();
            try
            {
                foreach (var c in Painters(this))
                {
                    int w = c.Width, h = c.Height;
                    if (w <= 0 || h <= 0) continue;
                    int left;
                    using (var bmp = new Bitmap(w, h, System.Drawing.Imaging.PixelFormat.Format32bppRgb))
                    {
                        using (var g = Graphics.FromImage(bmp))
                        {
                            g.Clear(Sentinel);
                            var e = new PaintEventArgs(g, new Rectangle(0, 0, w, h));
                            if (!Style(c, ControlStyles.Opaque)) InvokePaintBackground(c, e);
                            InvokePaint(c, e);
                        }
                        left = Unpainted(bmp);
                    }
                    if (left > 0)
                        problems.Add(Current + ": " + Describe(c) + " " + w + "x" + h + " leaves " + left + " pixels of its background unpainted" +
                            " (parent " + (c.Parent == null ? "none" : c.Parent.GetType().Name + " BackColor " + Hex(c.Parent.BackColor)) +
                            ", backdrop " + Hex(Theme.Backdrop(c)) + ")");
                    var wrong = WrongBackdrop(c, under);
                    if (wrong != null) problems.Add(Current + ": " + Describe(c) + " " + wrong);
                }
            }
            finally { foreach (var b in under.Values) if (b != null) b.Dispose(); }
            return problems;
        }

        // The colour half of G-BACKDROP: the colour a control clears itself in is the colour the window really
        // paints under it.
        //
        // The pixel half above holds only that the control painted something. A control that fills its whole
        // rectangle in the wrong colour passes it and still shows, on the screen, a rectangle that does not match
        // what it sits on, which is the other half of the BD-137 picture ("a dark rectangle behind the text that
        // does not match the card"): a new container that paints a colour of its own over its BackColor and does not
        // carry IBackdrop, or an IBackdrop whose property has drifted from what its own OnPaint fills, hands its
        // children a colour that is not there. So the parent is painted alone (with its own background layer, which
        // is the rest of the chain above it: Control.PaintTransparentBackground paints the parent for a child with a
        // transparent back colour, .NET Framework reference source, Control.cs), and the colour it leaves where the
        // control sits must be the colour Theme.Backdrop answers for that control. The check is of the declaration
        // against the paint, so it reads every control the gate walks, whoever drew it.
        //
        // The parent is painted in its own coordinates, into a bitmap of its client rectangle, and the point is read
        // inside that: TextRenderer draws through GDI and does not follow the Graphics transform, so a parent painted
        // through a translation puts its title text where the child is and the check would read a letter of it.
        //
        // One point is read, in the middle of the control: a wrong backdrop is one flat colour against another over
        // the whole rectangle, while an edge of a control may legitimately run over a line, a rounded corner or the
        // shadow of an ancestor (the guide picture stands on the card's teal border). The point is taken in the
        // middle of the part of the control the parent's client rectangle really covers, because the page scrolls:
        // a card below the fold sits outside the viewport its parent paints. Where even that point is left the
        // sentinel colour, nothing paints there at all and there is nothing to compare.
        string WrongBackdrop(Control c, Dictionary<Control, Bitmap> cache)
        {
            var parent = c.Parent;
            if (parent == null) return null;
            var bmp = Under(parent, cache);
            if (bmp == null) return null;
            var covered = Rectangle.Intersect(new Rectangle(c.Left, c.Top, c.Width, c.Height), new Rectangle(0, 0, bmp.Width, bmp.Height));
            if (covered.Width <= 0 || covered.Height <= 0) return null;
            var at = new Point(covered.Left + covered.Width / 2, covered.Top + covered.Height / 2);
            var want = Theme.Backdrop(c);
            var got = bmp.GetPixel(at.X, at.Y);
            if ((got.ToArgb() & 0xFFFFFF) == (Sentinel.ToArgb() & 0xFFFFFF)) return null;
            if ((got.ToArgb() & 0xFFFFFF) == (want.ToArgb() & 0xFFFFFF)) return null;
            return c.Width + "x" + c.Height + " clears itself in " + Hex(want) + ", but the window paints " + Hex(got) +
                " under it at " + (at.X - c.Left) + "," + (at.Y - c.Top) + " of it (parent " + Describe(parent) + ")";
        }

        // What one container paints when nothing of its content is drawn over it, kept for the whole walk: a page
        // has many rows in one card and the card is painted once for all of them.
        Bitmap Under(Control parent, Dictionary<Control, Bitmap> cache)
        {
            Bitmap bmp;
            if (cache.TryGetValue(parent, out bmp)) return bmp;
            var size = parent.ClientSize;
            if (size.Width > 0 && size.Height > 0)
            {
                bmp = new Bitmap(size.Width, size.Height, System.Drawing.Imaging.PixelFormat.Format32bppRgb);
                using (var g = Graphics.FromImage(bmp))
                {
                    g.Clear(Sentinel);
                    var e = new PaintEventArgs(g, new Rectangle(0, 0, size.Width, size.Height));
                    InvokePaintBackground(parent, e);
                    InvokePaint(parent, e);
                }
            }
            cache[parent] = bmp;
            return bmp;
        }

        // A colour no palette of this window carries, in either theme.
        static readonly Color Sentinel = Color.FromArgb(0xFF, 0x00, 0xFF);

        static string Hex(Color c) { return c.A == 255 ? "#" + c.R.ToString("X2") + c.G.ToString("X2") + c.B.ToString("X2") : c.Name; }

        // The controls whose own painting the gate reads: the ones this program draws, and any control that was left
        // a transparent back colour. A stock control on a colour of its own is Windows' to paint. A control that is
        // not shown is skipped with everything inside it: a child keeps its own visible flag while its parent is
        // hidden, so a walk that recursed past a hidden container would report controls the user never sees (Walk,
        // for G-A11Y, and the control application's ShownIn skip the subtree the same way).
        static IEnumerable<Control> Painters(Control root)
        {
            foreach (Control k in root.Controls)
            {
                if (!HasVisibleFlag(k)) continue;
                if (Style(k, ControlStyles.UserPaint) &&
                    (k.GetType().Assembly == typeof(SetupForm).Assembly || k.BackColor.A != 255)) yield return k;
                foreach (var p in Painters(k)) yield return p;
            }
        }

        static int Unpainted(Bitmap bmp)
        {
            // The alpha byte of a 32bppRgb bitmap is not part of the picture, so only the colour is compared.
            int sentinel = Sentinel.ToArgb() & 0xFFFFFF, left = 0;
            var data = bmp.LockBits(new Rectangle(0, 0, bmp.Width, bmp.Height), System.Drawing.Imaging.ImageLockMode.ReadOnly,
                System.Drawing.Imaging.PixelFormat.Format32bppRgb);
            try
            {
                var row = new int[bmp.Width];
                for (int y = 0; y < bmp.Height; y++)
                {
                    System.Runtime.InteropServices.Marshal.Copy(data.Scan0 + y * data.Stride, row, 0, row.Length);
                    for (int x = 0; x < row.Length; x++) if ((row[x] & 0xFFFFFF) == sentinel) left++;
                }
            }
            finally { bmp.UnlockBits(data); }
            return left;
        }

        static readonly MethodInfo GetStyleMethod = typeof(Control).GetMethod("GetStyle", BindingFlags.NonPublic | BindingFlags.Instance);
        static bool Style(Control c, ControlStyles s) { return (bool)GetStyleMethod.Invoke(c, new object[] { s }); }

        // G-A11Y: every control a user can reach has a name, the keyboard reaches every one of them, Enter and Escape
        // have their buttons, and nothing reachable is disabled-looking text.
        public List<string> Accessibility()
        {
            var problems = new List<string>();
            var reachable = new List<Control>();
            Walk(this, reachable);
            foreach (var c in reachable)
            {
                if (string.IsNullOrWhiteSpace(c.AccessibleName) && string.IsNullOrWhiteSpace(c.Text)) problems.Add(Current + ": " + Describe(c) + " has no accessible name");
                // Inside a group of option buttons Tab reaches the checked one and the arrow keys the others.
                bool inGroup = c is RadioButton && c.Parent.Controls.OfType<RadioButton>().Any(o => o.TabStop || o.Checked);
                if (!c.TabStop && !inGroup) problems.Add(Current + ": " + Describe(c) + " is not reachable with Tab");
                // An owner-drawn button says what pressing it does, as a system button does. A button that is off
                // offers no action, and Windows says so itself.
                if (c.Enabled && c is ButtonBase && string.IsNullOrEmpty(c.AccessibleDefaultActionDescription) && c.AccessibilityObject.DefaultAction == null)
                    problems.Add(Current + ": " + Describe(c) + " has no default action");
                if (c is ButtonBase && c.AccessibilityObject.Role == AccessibleRole.None) problems.Add(Current + ": " + Describe(c) + " has no accessible role");
            }
            // A layout panel without a name of its own takes, from Windows' window proxy, the text of the nearest label
            // before it in its parent (an empty AccessibleName counts as no name; gui-L4-b13 trials): the text is read
            // again. So a bare panel never follows a label in the same parent; a panel after a label needs a real name.
            foreach (var p in Panels(this))
            {
                if (p.AccessibleName == null) problems.Add(Current + ": " + Describe(p) + " has no explicit accessible name");
                if (!string.IsNullOrEmpty(p.AccessibleName)) continue;
                var label = p.Parent.Controls.Cast<Control>().TakeWhile(k => k != p).OfType<Label>().LastOrDefault();
                if (label != null) problems.Add(Current + ": a bare " + Describe(p) + " in " + Describe(p.Parent) + " follows the label \"" + label.Text + "\" and takes its text as its name");
            }
            var order = reachable.Select(TabPath).ToList();
            if (order.Distinct().Count() != order.Count) problems.Add(Current + ": two controls share a tab position");
            // The first Tab of the window: the screen or its buttons, never a glyph of the title bar. Space on the
            // first tab stop must not end setup. A screen that offers nothing at all (the engine is working and even
            // Cancel is not available) has only the title bar, and then the title bar is where Tab starts.
            var first = reachable.OrderBy(TabPath, StringComparer.Ordinal).FirstOrDefault();
            if (first != null && Inside(_chrome, first) && reachable.Any(c => !Inside(_chrome, c)))
                problems.Add(Current + ": the first control in the tab order is in the title bar (" + Describe(first) + ")");
            bool working = Current == Screen.Working || Current == Screen.Checking;
            if (!working && AcceptButton == null) problems.Add(Current + ": no default button for Enter");
            if (!working && CancelButton == null) problems.Add(Current + ": no button for Escape");
            return problems;
        }

        // The controls a user can reach: the system's buttons, option controls and text boxes, and the owner-drawn
        // controls that take the keyboard themselves (IFocusTarget).
        static void Walk(Control c, List<Control> into)
        {
            foreach (Control k in c.Controls)
            {
                if (!HasVisibleFlag(k)) continue;
                if (k is ButtonBase || k is TextBox || k is ListControl || k is IFocusTarget) into.Add(k);
                Walk(k, into);
            }
        }

        static bool Inside(Control parent, Control c)
        {
            for (var p = c; p != null; p = p.Parent) if (p == parent) return true;
            return false;
        }

        static IEnumerable<Control> Panels(Control c)
        {
            foreach (Control k in c.Controls)
            {
                if (k is Panel) yield return k;
                foreach (var p in Panels(k)) yield return p;
            }
        }

        // Where a control stands in the tab order: the chain of tab positions from the window down to it. It carries
        // nothing of the control's identity, so two controls that share a position inside one parent give one path
        // and the check below sees them.
        static string TabPath(Control c)
        {
            var parts = new List<string>();
            for (var p = c; p != null; p = p.Parent) parts.Insert(0, p.TabIndex.ToString("D4"));
            return string.Join("/", parts);
        }

        // Every text a user sees on the screen (for G-NOINT and the smoke summary). Controls tagged noint-exempt
        // (the release notes) are listed apart.
        public string VisibleText(bool includeExempt)
        {
            var w = new StringBuilder();
            Texts(this, w, includeExempt);
            return w.ToString();
        }

        static void Texts(Control c, StringBuilder w, bool exempt)
        {
            foreach (Control k in c.Controls)
            {
                if (!HasVisibleFlag(k)) continue;
                if (!exempt && (k.Tag as string) == "noint-exempt") continue;
                if (!string.IsNullOrEmpty(k.Text) && !(k is Form)) w.AppendLine(k.Text);
                if (!string.IsNullOrEmpty(k.AccessibleName)) w.AppendLine(k.AccessibleName);
                // An owner-drawn row draws its text itself: the window checks the words it painted, not only a Text.
                var row = k as IconRow;
                if (row != null) { w.AppendLine(row.Head); if (row.Note != null) w.AppendLine(row.Note); }
                var bubble = k as Bubble;
                if (bubble != null) w.AppendLine(bubble.Text);
                Texts(k, w, exempt);
            }
        }

        static readonly MethodInfo GetState = typeof(Control).GetMethod("GetState", BindingFlags.NonPublic | BindingFlags.Instance);
        static bool HasVisibleFlag(Control c) { return (bool)GetState.Invoke(c, new object[] { 2 }); }

        static void CreateHandles(Control c)
        {
            if (!c.IsHandleCreated) { var h = c.Handle; }
            foreach (Control k in c.Controls) CreateHandles(k);
        }

        static IEnumerable<string> Overlaps(Control parent)
        {
            var kids = parent.Controls.Cast<Control>().Where(HasVisibleFlag).ToList();
            for (int i = 0; i < kids.Count; i++)
                for (int j = i + 1; j < kids.Count; j++)
                    if (kids[i].Bounds.IntersectsWith(kids[j].Bounds))
                        yield return Describe(kids[i]) + " " + kids[i].Bounds + " overlaps " + Describe(kids[j]) + " " + kids[j].Bounds;
            foreach (var k in kids) foreach (var o in Overlaps(k)) yield return o;
        }

        static string Describe(Control c)
        {
            var row = c as IconRow;
            var text = row != null ? row.Head : c.Text;
            return c.GetType().Name + (string.IsNullOrEmpty(text) ? "" : " \"" + (text.Length > 40 ? text.Substring(0, 40) : text) + "\"");
        }
    }
}
