// The setup window: a header with the language choice, the steps on the left, the guide panel on the right and one
// screen in the middle (Welcome, Checking, Plan, Working, Restart, Result, Prepare). Every decision comes from the
// engine (docs/gui/interfaces-setup.md): the window runs a plan, shows its checks, notes, settings-impact plan,
// restarts and consents, runs the engine with the consents the user gave, and owns the restart (planned, never
// forced). Plain words only; the engine's technical details go into the support file.
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
        public const int DesignWidth = 1060, DesignHeight = 700, RailWidth = 196, GuideWidth = 250;

        readonly SetupArgs _args;
        readonly bool _smoke;
        readonly string _ownPackage;
        readonly bool _showNagi;

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
        bool _consentTestSigning;
        string _bitLocker = "";
        bool _restartLater;
        string _restartError, _supportSaved, _supportError;
        string _prepDestination = "", _prepFirmware = "";
        bool _prepUseFirmware, _prepFromWelcome;
        Timer _timer;

        Panel _header, _rail, _guide, _column;
        FlowLayoutPanel _content, _buttons;
        Label _guideTitle, _guideText;
        int _contentWidth;

        public SetupForm(SetupArgs args, bool smoke)
        {
            _args = args;
            _smoke = smoke;
            _ownPackage = args.Package ?? DefaultPackage();
            _package = _ownPackage;
            _showNagi = !smoke && Native.ShowNagi();
            Text = "amdgpu-wddm Setup";
            AutoScaleMode = AutoScaleMode.None;
            FormBorderStyle = FormBorderStyle.FixedSingle;
            MaximizeBox = false;
            StartPosition = FormStartPosition.CenterScreen;
            BackColor = Theme.Back;
            ForeColor = Theme.Text;
            KeyPreview = true;
            ClientSize = Theme.Sz(DesignWidth, DesignHeight);
            BuildFrame();
            Strings.LanguageChanged += OnLanguageChanged;
            if (args.Mode == "repair") _flow = "repair";
            else if (args.Mode == "continue") _flow = "continue";
            else if (args.Mode == "prepare-offline") _flow = "prepare";
            if (!smoke)
            {
                _timer = new Timer { Interval = 250 };
                _timer.Tick += (s, e) => PollEngine();
                _timer.Start();
            }
        }

        // The package the exe belongs to: <package>\setup\amdgpu_wddm_setup.exe.
        public static string DefaultPackage()
        {
            var dir = Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location);
            return Path.GetDirectoryName(dir);
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
            Strings.LanguageChanged -= OnLanguageChanged;
            base.OnFormClosing(e);
        }

        // ---- frame ------------------------------------------------------------------------------------------------

        void BuildFrame()
        {
            SuspendLayout();
            Controls.Clear();
            _header = new Panel { Dock = DockStyle.Top, Height = Theme.S(60), BackColor = Theme.Nav };
            _rail = new Panel { Dock = DockStyle.Left, Width = Theme.S(RailWidth), BackColor = Theme.Nav, Padding = Theme.Pad(0, 18, 0, 0) };
            _guide = new Panel { Dock = DockStyle.Right, Width = Theme.S(GuideWidth), BackColor = Theme.Back, Padding = Theme.Pad(0, 20, 20, 20) };
            _column = new Panel { Dock = DockStyle.Fill, BackColor = Theme.Back };
            _buttons = new FlowLayoutPanel
            {
                Dock = DockStyle.Bottom, FlowDirection = FlowDirection.RightToLeft, WrapContents = false, Height = Theme.S(64),
                Padding = Theme.Pad(20, 10, 20, 10), BackColor = Theme.Back,
            };
            _content = new FlowLayoutPanel
            {
                Dock = DockStyle.Fill, FlowDirection = FlowDirection.TopDown, WrapContents = false, AutoScroll = true,
                Padding = Theme.Pad(28, 0, 20, 0), BackColor = Theme.Back,
            };
            Theme.DarkScroll(_content);
            _column.Controls.Add(_content);
            _column.Controls.Add(_buttons);
            _contentWidth = Theme.S(DesignWidth - RailWidth - GuideWidth) - _content.Padding.Horizontal - SystemInformation.VerticalScrollBarWidth;
            Controls.Add(_column);
            Controls.Add(_guide);
            Controls.Add(_rail);
            Controls.Add(_header);
            BuildHeader();
            BuildGuide();
            ResumeLayout(true);
        }

        void BuildHeader()
        {
            _header.Controls.Clear();
            var logo = new Panel { Size = Theme.Sz(26, 26), Location = new Point(Theme.S(20), Theme.S(17)), BackColor = Color.Transparent };
            logo.Paint += (s, e) =>
            {
                using (var red = new Pen(Theme.Accent, Theme.S(3))) e.Graphics.DrawRectangle(red, Theme.S(2), Theme.S(2), Theme.S(15), Theme.S(15));
                using (var teal = new Pen(Theme.Teal, Theme.S(3))) e.Graphics.DrawRectangle(teal, Theme.S(9), Theme.S(9), Theme.S(15), Theme.S(15));
            };
            logo.AccessibleRole = AccessibleRole.Graphic;
            logo.AccessibleName = "";
            var brand = Ui.Label("amdgpu-wddm Setup", Theme.Brand, Theme.Text);
            brand.Location = new Point(Theme.S(56), Theme.S(16));
            _header.Controls.Add(logo);
            _header.Controls.Add(brand);
            var langs = Ui.Row();
            langs.Anchor = AnchorStyles.Top | AnchorStyles.Right;
            foreach (var l in new[] { new[] { "en", "EN", "English" }, new[] { "pl", "PL", "Polski" }, new[] { "ja", "日本語", "日本語" }, new[] { "ko", "한국어", "한국어" } })
            {
                var code = l[0];
                var b = Ui.Button(l[1], (s, e) => Strings.Language = code);
                b.Font = Theme.Small;
                b.BackColor = Strings.Language == code ? Theme.CardHi : Theme.Nav;
                b.FlatAppearance.BorderColor = Strings.Language == code ? Theme.Accent : Theme.Nav;
                b.Margin = Theme.Pad(0, 0, 6, 0);
                b.AccessibleName = Strings.T("ui.language", l[2]);
                b.Tag = "lang";
                langs.Controls.Add(b);
            }
            _header.Controls.Add(langs);
            langs.PerformLayout();
            langs.Location = new Point(Theme.S(DesignWidth) - langs.PreferredSize.Width - Theme.S(14), (Theme.S(60) - langs.PreferredSize.Height) / 2);
        }

        void BuildGuide()
        {
            _guide.Controls.Clear();
            var card = new CardPanel(null, Theme.S(GuideWidth - 20), Theme.GuideBack);
            _guideTitle = Ui.Label("", Theme.CardTitle, Theme.Teal, card.Inner);
            _guideText = Ui.Label("", Theme.Body, Theme.Text, card.Inner);
            card.Add(_guideTitle);
            card.Add(_guideText);
            card.Location = new Point(0, _guide.Padding.Top);
            _guide.Controls.Add(card);
        }

        void BuildRail()
        {
            _rail.Controls.Clear();
            var steps = _flow == "prepare" ? new[] { "rail.folder", "rail.prepare", "rail.finish" } : new[] { "rail.welcome", "rail.check", "rail.plan", "rail.install", "rail.finish" };
            int current = RailIndex();
            var stack = Ui.Stack(Theme.S(RailWidth));
            stack.Location = new Point(0, _rail.Padding.Top);
            for (int i = 0; i < steps.Length; i++)
            {
                bool on = i == current, done = i < current;
                var row = new Panel { Size = new Size(Theme.S(RailWidth - 16), Theme.S(40)), Margin = Theme.Pad(8, 0, 8, 4), BackColor = on ? Theme.CardHi : Theme.Nav };
                if (on) row.Controls.Add(new Panel { Dock = DockStyle.Left, Width = Theme.S(4), BackColor = Theme.Accent });
                var label = Ui.Label((done ? "✓  " : (i + 1) + "  ") + Strings.T(steps[i]), on ? Theme.Bold : Theme.Body, on ? Theme.Text : done ? Theme.Good : Theme.Dim, Theme.S(RailWidth - 40));
                label.Location = new Point(Theme.S(16), (Theme.S(40) - label.PreferredSize.Height) / 2);
                row.Controls.Add(label);
                row.AccessibleRole = AccessibleRole.StaticText;
                row.AccessibleName = Strings.T(steps[i]) + (on ? " (" + Strings.T("rail.current") + ")" : "");
                stack.Controls.Add(row);
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

        void OnLanguageChanged()
        {
            Theme.Fonts();
            BuildFrame();
            Show(Current);
        }

        // ---- screens ----------------------------------------------------------------------------------------------

        static Panel Spacer(int height) { return new Panel { Size = new Size(1, Theme.S(height)), Margin = Theme.Pad(0), BackColor = Theme.Back, TabStop = false }; }

        public void Show(Screen screen)
        {
            Current = screen;
            SuspendLayout();
            _content.SuspendLayout();
            _content.Controls.Clear();
            _buttons.Controls.Clear();
            AcceptButton = null;
            CancelButton = null;
            // The space above and below the screen is made of spacers, not panel padding: the scroll range of a
            // FlowLayoutPanel does not cover its top padding, which left the end of a long plan out of reach.
            _content.Controls.Add(Spacer(22));
            switch (screen)
            {
                case Screen.Welcome: BuildWelcome(); break;
                case Screen.Checking: BuildWorking(true); break;
                case Screen.Plan: BuildPlan(); break;
                case Screen.Working: BuildWorking(false); break;
                case Screen.Restart: BuildRestart(); break;
                case Screen.Result: BuildResult(); break;
                case Screen.Prepare: BuildPrepare(); break;
            }
            _content.Controls.Add(Spacer(12));
            BuildRail();
            UpdateGuide();
            AssignTabOrder();
            _content.ResumeLayout(true);
            ResumeLayout(true);
            _content.AutoScrollPosition = new Point(0, 0);
        }

        Label Title(string text)
        {
            var l = Ui.Label(text, Theme.Title, Theme.Text, _contentWidth);
            l.Margin = Theme.Pad(0, 0, 0, 10);
            _content.Controls.Add(l);
            return l;
        }

        Label Para(string text, Color? color = null)
        {
            var l = Ui.Label(text, Theme.Body, color ?? Theme.Text, _contentWidth);
            l.Margin = Theme.Pad(0, 2, 0, 8);
            _content.Controls.Add(l);
            return l;
        }

        CardPanel Card(string title)
        {
            var c = new CardPanel(title, _contentWidth);
            _content.Controls.Add(c);
            return c;
        }

        Button AddButton(string id, EventHandler click, bool primary = false)
        {
            var b = Ui.Button(Strings.T(id), click, primary);
            b.Margin = Theme.Pad(10, 4, 0, 4);
            _buttons.Controls.Add(b);
            if (primary) AcceptButton = b;
            return b;
        }

        void BuildWelcome()
        {
            bool repair = _flow == "repair";
            Title(Strings.T(repair ? "welcome.repair.title" : "welcome.title"));
            Para(Strings.T(repair ? "welcome.repair.body" : "welcome.body"));
            if (_args.DryRun) Para(Strings.T("welcome.dry-run"), Theme.Warn);
            var version = PackageVersion(_ownPackage);
            if (version != null) Para(Strings.T("welcome.version", version), Theme.Dim);

            var source = Card(Strings.T("welcome.source.title"));
            var own = Ui.Radio(Strings.T(repair ? "welcome.source.kept" : "welcome.source.package"), source.Inner);
            var prepared = Ui.Radio(Strings.T("welcome.source.prepared"), source.Inner);
            own.Checked = !_usePrepared;
            prepared.Checked = _usePrepared;
            source.Add(own);
            source.Add(prepared);
            var path = new TextBox
            {
                Text = _preparedDir ?? "", ReadOnly = true, BackColor = Theme.Nav, ForeColor = Theme.Text, BorderStyle = BorderStyle.FixedSingle,
                Font = Theme.Body, Width = source.Inner - Theme.S(170), Margin = Theme.Pad(0, 8, 8, 0), AccessibleName = Strings.T("welcome.prepared.path"), TabStop = true,
            };
            var browse = Ui.Button(Strings.T("ui.choose-folder"), (s, e) =>
            {
                var dir = PickFolder(Strings.T("welcome.source.prepared"), _preparedDir);
                if (dir == null) return;
                SetPrepared(dir);
                Show(Screen.Welcome);
            });
            source.Add(Ui.Row(path, browse));
            Label status = null;
            if (_usePrepared)
            {
                status = _preparedProblem == null && _preparedDir != null
                    ? Ui.Label(Strings.T("welcome.prepared.ok", _preparedVersion ?? "-"), null, Theme.Good, source.Inner)
                    : Ui.Label(Strings.T(_preparedDir == null ? "welcome.prepared.choose" : _preparedProblem), null, _preparedDir == null ? Theme.Dim : Theme.Warn, source.Inner);
                source.Add(status);
            }
            own.CheckedChanged += (s, e) => { if (own.Checked && _usePrepared) { _usePrepared = false; _package = _ownPackage; Show(Screen.Welcome); } };
            prepared.CheckedChanged += (s, e) => { if (prepared.Checked && !_usePrepared) { _usePrepared = true; _package = _preparedProblem == null && _preparedDir != null ? _preparedDir : _ownPackage; Show(Screen.Welcome); } };

            if (!repair)
            {
                var link = Ui.Button(Strings.T("welcome.prepare-link"), (s, e) => { _prepFromWelcome = true; _flow = "prepare"; Show(Screen.Prepare); });
                link.BackColor = Theme.Back;
                link.ForeColor = Theme.Teal;
                link.Margin = Theme.Pad(0, 4, 0, 0);
                _content.Controls.Add(link);
            }

            AddButton("ui.close", (s, e) => Close());
            var check = AddButton("welcome.check", (s, e) => RunPlan(), true);
            check.Enabled = !_usePrepared || (_preparedDir != null && _preparedProblem == null);
            CancelButton = (IButtonControl)_buttons.Controls[0];
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

        // Checking (a plan run) and Working (a real run) share the progress screen.
        void BuildWorking(bool checking)
        {
            var run = _active;
            string titleId = checking ? "checking.title" : _flow == "prepare" ? "working.prepare.title" : _flow == "repair" ? "working.repair.title" : _flow == "continue" ? "working.continue.title" : "working.title";
            Title(Strings.T(titleId));
            Para(Strings.T(checking ? "checking.body" : _flow == "prepare" ? "working.prepare.body" : "working.body"), Theme.Dim);
            var bar = new ProgressBar { Style = ProgressBarStyle.Marquee, MarqueeAnimationSpeed = 30, Width = _contentWidth, Height = Theme.S(6), Margin = Theme.Pad(0, 4, 0, 14) };
            bar.AccessibleName = Strings.T(titleId);
            _content.Controls.Add(bar);
            if (checking)
            {
                var card = Card(Strings.T("checking.list"));
                foreach (var c in run == null ? new List<CheckRow>() : run.Checks) AddCheck(card, c);
                if (run == null || run.Checks.Count == 0) card.Add(Ui.Label(Strings.T("checking.waiting"), null, Theme.Dim, card.Inner));
                return;
            }
            var stages = Card(Strings.T("working.stages"));
            var mode = run != null && run.Mode != null ? run.Mode : (_flow == "prepare" ? "prepare-offline" : "run");
            var action = run != null && run.Decision != null ? run.Decision.Action : null;
            var expected = EngineRun.ExpectedStages(mode, action).ToList();
            foreach (var s in run == null ? new List<string>() : run.Stages) if (!expected.Contains(s)) expected.Add(s);
            int current = run == null || run.CurrentStage == null ? -1 : expected.IndexOf(run.CurrentStage);
            for (int i = 0; i < expected.Count; i++)
            {
                bool done = i < current || (run != null && run.ResultOutcome != null && i <= current);
                bool on = i == current && !done;
                var mark = done ? "✓" : on ? "▶" : "·";
                var text = Strings.Has("stage." + expected[i]) ? Strings.T("stage." + expected[i]) : Strings.T("stage.other");
                stages.Add(Ui.Label(mark + "  " + text, on ? Theme.Bold : Theme.Body, done ? Theme.Good : on ? Theme.Text : Theme.Dim, stages.Inner));
            }
            string note;
            if (_client != null && _client.CancelRequested) note = "working.cancel.requested";
            else if (run != null && run.CancelAvailable) note = "working.cancel.available";
            else note = "working.cancel.unavailable";
            Para(Strings.T(note), Theme.Dim);
            if (_flow != "prepare")
            {
                var cancel = AddButton("ui.cancel", (s, e) => { if (_client != null && _client.RequestCancel()) Show(Screen.Working); });
                cancel.Enabled = run != null && run.CancelAvailable && !(_client != null && _client.CancelRequested);
            }
        }

        void AddCheck(CardPanel card, CheckRow c)
        {
            var mark = c.Result == "ok" ? "✓" : c.Result == "warn" ? "!" : "✗";
            var color = c.Result == "ok" ? Theme.Good : c.Result == "warn" ? Theme.Warn : Theme.Accent;
            var m = Ui.Label(mark, Theme.Bold, color);
            m.MinimumSize = new Size(Theme.S(22), 0);
            var t = Ui.Label(CheckText(c.Id), null, Theme.Text, card.Inner - Theme.S(30));
            card.Add(Ui.Row(m, t));
        }

        public static string CheckText(string id)
        {
            return id != null && Strings.Has("check." + id) ? Strings.T("check." + id) : Strings.T("check.other");
        }

        void BuildPlan()
        {
            var d = _plan != null ? _plan.Decision : null;
            var version = d != null ? d.PackageVersion : PackageVersion(_package);
            var action = d != null ? d.Action : "install";
            switch (action)
            {
                case "upgrade": Title(Strings.T("plan.upgrade.title", d.InstalledVersion ?? "-", version)); break;
                case "repair": Title(Strings.T("plan.repair.title", version)); break;
                case "resume": Title(Strings.T("plan.resume.title", version)); break;
                case "already": Title(Strings.T("plan.already.title", version)); Para(Strings.T("plan.already.body")); break;
                case "verify": Title(Strings.T("plan.verify.title", version)); Para(Strings.T("plan.verify.body")); break;
                default: Title(Strings.T("plan.install.title", version)); break;
            }
            if (action == "already")
            {
                AddButton("ui.close", (s, e) => Close());
                AddButton("plan.repair-again", (s, e) => { _flow = "repair"; RunPlan(); }, true);
                CancelButton = (IButtonControl)_buttons.Controls[0];
                return;
            }
            if (action == "verify")
            {
                AddButton("ui.close", (s, e) => Close());
                AddButton("restart.now", (s, e) => RestartNow(), true);
                CancelButton = (IButtonControl)_buttons.Controls[0];
                return;
            }

            // Checks: all passed, or the ones that need attention.
            var warn = _plan == null ? new List<CheckRow>() : _plan.Checks.Where(c => c.Result != "ok").ToList();
            var checks = Card(Strings.T("plan.checks.title"));
            if (warn.Count == 0) checks.Add(Ui.Label(Strings.T("plan.checks.ok"), null, Theme.Good, checks.Inner));
            foreach (var c in warn) AddCheck(checks, c);

            var steps = Card(Strings.T("plan.what.title"));
            int restarts = d != null ? d.Restarts : 0;
            steps.Add(Ui.Label(Strings.T(restarts <= 0 ? "plan.restarts.0" : restarts == 1 ? "plan.restarts.1" : "plan.restarts.2"), null, null, steps.Inner));
            if (restarts > 0) steps.Add(Ui.Label(Strings.T("plan.restarts.choice"), null, Theme.Dim, steps.Inner));
            var fw = d != null ? d.FirmwareSource : null;
            if (fw == "download" || fw == "folder" || fw == "package-folder") steps.Add(Ui.Label(Strings.T("plan.firmware." + fw), null, null, steps.Inner));

            var notes = ReleaseNotes.Load(_package, Strings.Language);
            var nc = Card(Strings.T("plan.notes.title"));
            if (notes == null) nc.Add(Ui.Label(Strings.T("plan.notes.none"), null, Theme.Dim, nc.Inner));
            else
            {
                if (notes.EnglishFallback) nc.Add(Ui.Label(Strings.T("plan.notes.english"), Theme.Small, Theme.Warn, nc.Inner));
                var box = new TextBox
                {
                    Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical, Text = notes.Text, Width = nc.Inner, Height = Theme.S(150),
                    BackColor = Theme.Nav, ForeColor = Theme.Text, BorderStyle = BorderStyle.None, Font = Theme.Body, Margin = Theme.Pad(0, 6, 0, 0),
                    AccessibleName = Strings.T("plan.notes.title"), TabStop = true, Tag = "noint-exempt",
                };
                Theme.DarkScroll(box);
                nc.Add(box);
            }

            var sc = Card(Strings.T("plan.settings.title"));
            var s0 = _plan != null ? _plan.Settings : null;
            if (s0 == null) sc.Add(Ui.Label(Strings.T("plan.settings.none"), null, Theme.Dim, sc.Inner));
            else
            {
                sc.Add(Ui.Label(Strings.T("plan.settings.summary", s0.Kept, s0.Added, s0.Updated, s0.Unchanged + s0.Command), null, null, sc.Inner));
                foreach (var line in SettingsView.Lines(s0))
                {
                    var text = Strings.T(line.TextId, line.Args) + (line.DecisionId != null && Strings.Has(line.DecisionId) ? ": " + Strings.T(line.DecisionId) : "");
                    sc.Add(Ui.Label("• " + text, null, Theme.Dim, sc.Inner));
                }
                sc.Add(Ui.Label(Strings.T("plan.settings.note"), Theme.Small, Theme.Dim, sc.Inner));
            }

            var consents = d != null ? d.Consents : new string[0];
            if (_planResult != null && _planResult.ConsentsNeeded.Length > 0) consents = consents.Union(_planResult.ConsentsNeeded).ToArray();
            Button go = null;
            if (consents.Contains("test-signing") || consents.Contains("bitlocker"))
            {
                var cc = Card(Strings.T("consent.title"));
                if (consents.Contains("test-signing"))
                {
                    var box = Ui.Check(Strings.T("consent.test-signing"), cc.Inner);
                    box.Checked = _consentTestSigning;
                    box.CheckedChanged += (s, e) => { _consentTestSigning = box.Checked; if (go != null) go.Enabled = ConsentsComplete(consents); };
                    cc.Add(box);
                    cc.Add(Ui.Label(Strings.T("consent.test-signing.note"), Theme.Small, Theme.Dim, cc.Inner));
                }
                if (consents.Contains("bitlocker"))
                {
                    cc.Add(Ui.Label(Strings.T("consent.bitlocker"), null, null, cc.Inner));
                    var key = Ui.Radio(Strings.T("consent.bitlocker.have-key"), cc.Inner);
                    var pause = Ui.Radio(Strings.T("consent.bitlocker.suspend"), cc.Inner);
                    key.Checked = _bitLocker == "HaveKey";
                    pause.Checked = _bitLocker == "Suspend";
                    key.CheckedChanged += (s, e) => { if (key.Checked) _bitLocker = "HaveKey"; if (go != null) go.Enabled = ConsentsComplete(consents); };
                    pause.CheckedChanged += (s, e) => { if (pause.Checked) _bitLocker = "Suspend"; if (go != null) go.Enabled = ConsentsComplete(consents); };
                    var group = Ui.Stack(cc.Inner);
                    group.Controls.Add(key);
                    group.Controls.Add(pause);
                    cc.Add(group);
                }
            }

            AddButton("ui.close", (s, e) => Close());
            go = AddButton(action == "upgrade" ? "plan.go.upgrade" : action == "repair" ? "plan.go.repair" : action == "resume" ? "plan.go.resume" : "plan.go.install", (s, e) => RunInstall(), true);
            go.Enabled = ConsentsComplete(consents);
            if (_flow != "continue") AddButton("ui.back", (s, e) => Show(Screen.Welcome));
            CancelButton = (IButtonControl)_buttons.Controls[0];
        }

        bool ConsentsComplete(string[] consents)
        {
            return (!consents.Contains("test-signing") || _consentTestSigning) && (!consents.Contains("bitlocker") || _bitLocker.Length > 0);
        }

        void BuildRestart()
        {
            Title(Strings.T(_view.TitleId));
            Para(Strings.T(_view.BodyId));
            if (_restartError != null) Para(Strings.T("restart.failed"), Theme.Warn);
            if (_restartLater)
            {
                Para(Strings.T("restart.later.done"), Theme.Teal);
                AddButton("ui.close", (s, e) => Close(), true);
                CancelButton = AcceptButton;
                return;
            }
            Para(Strings.T("restart.save-work"), Theme.Dim);
            AddButton("restart.later", (s, e) => { _restartLater = true; Show(Screen.Restart); });
            if (_view.OfferRestart) AddButton("restart.now", (s, e) => RestartNow(), true);
            CancelButton = (IButtonControl)_buttons.Controls[0];
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
            var color = v.Kind == ViewKind.Success ? Theme.Good : v.Kind == ViewKind.Problem ? Theme.Accent : v.Kind == ViewKind.Cancelled ? Theme.Warn : Theme.Teal;
            var mark = new Panel { Width = Theme.S(48), Height = Theme.S(4), BackColor = color, Margin = Theme.Pad(0, 0, 0, 10) };
            _content.Controls.Add(mark);
            Title(Strings.T(v.TitleId));
            Para(Strings.T(v.BodyId));
            if (v.FailedChecks.Length > 0)
            {
                var card = Card(Strings.T("result.failed-checks"));
                foreach (var id in v.FailedChecks) AddCheck(card, new CheckRow { Id = id, Result = "fail" });
            }
            if (v.NothingChanged) Para(Strings.T("result.nothing-changed"), Theme.Good);
            else if (v.ChangesUnknown) Para(Strings.T("result.changes-unknown"), Theme.Warn);
            else if (v.ChangesMade && (v.Kind == ViewKind.Problem || v.Kind == ViewKind.Cancelled)) Para(Strings.T("result.changes-made"), Theme.Warn);
            if (v.Kind != ViewKind.Success || _flow == "prepare")
            {
                Para(Strings.T("result.support"), Theme.Dim);
                if (_supportSaved != null) Para(Strings.T("result.support.saved", _supportSaved), Theme.Good);
                if (_supportError != null) Para(Strings.T("result.support.failed"), Theme.Warn);
            }

            AddButton("ui.close", (s, e) => Close(), v.Kind == ViewKind.Success || v.Kind == ViewKind.Information);
            if (v.OfferRepair) AddButton("result.repair", (s, e) => { _flow = "repair"; RunPlan(); }, v.Kind == ViewKind.Problem);
            if (v.OfferRetry) AddButton("result.retry", (s, e) => { if (_flow == "prepare") Show(Screen.Prepare); else RunPlan(); }, !v.OfferRepair && v.Kind != ViewKind.Success && v.Kind != ViewKind.Information);
            if (v.Kind != ViewKind.Success || _flow == "prepare") AddButton("result.save-support", (s, e) => SaveSupport());
            CancelButton = (IButtonControl)_buttons.Controls[0];
        }

        void BuildPrepare()
        {
            Title(Strings.T("prepare.title"));
            Para(Strings.T("prepare.body"));
            var card = Card(Strings.T("prepare.destination"));
            var dest = new TextBox
            {
                Text = _prepDestination, BackColor = Theme.Nav, ForeColor = Theme.Text, BorderStyle = BorderStyle.FixedSingle, Font = Theme.Body,
                Width = card.Inner - Theme.S(170), Margin = Theme.Pad(0, 8, 8, 0), AccessibleName = Strings.T("prepare.destination"), TabStop = true,
            };
            Button go = null;
            dest.TextChanged += (s, e) => { _prepDestination = dest.Text.Trim(); if (go != null) go.Enabled = PrepareReady(); };
            card.Add(Ui.Row(dest, Ui.Button(Strings.T("ui.choose-folder"), (s, e) => { var d = PickFolder(Strings.T("prepare.destination"), _prepDestination); if (d != null) { dest.Text = d; } })));
            card.Add(Ui.Label(Strings.T("prepare.destination.hint"), Theme.Small, Theme.Dim, card.Inner));

            var fwCard = Card(Strings.T("prepare.firmware.title"));
            var use = Ui.Check(Strings.T("prepare.use-firmware"), fwCard.Inner);
            use.Checked = _prepUseFirmware;
            fwCard.Add(use);
            var fw = new TextBox
            {
                Text = _prepFirmware, BackColor = Theme.Nav, ForeColor = Theme.Text, BorderStyle = BorderStyle.FixedSingle, Font = Theme.Body,
                Width = fwCard.Inner - Theme.S(170), Margin = Theme.Pad(0, 8, 8, 0), AccessibleName = Strings.T("prepare.firmware.folder"), TabStop = true,
                ReadOnly = !_prepUseFirmware,
            };
            fw.TextChanged += (s, e) => { _prepFirmware = fw.Text.Trim(); if (go != null) go.Enabled = PrepareReady(); };
            var fwBrowse = Ui.Button(Strings.T("ui.choose-folder"), (s, e) => { var d = PickFolder(Strings.T("prepare.firmware.folder"), _prepFirmware); if (d != null) { fw.Text = d; use.Checked = true; } });
            fwBrowse.AccessibleName = Strings.T("prepare.firmware.folder") + ": " + Strings.T("ui.choose-folder");
            fwCard.Add(Ui.Row(fw, fwBrowse));
            fwCard.Add(Ui.Label(Strings.T(_prepUseFirmware ? "prepare.firmware.folder-note" : "prepare.firmware.download-note"), Theme.Small, Theme.Dim, fwCard.Inner));
            use.CheckedChanged += (s, e) => { _prepUseFirmware = use.Checked; Show(Screen.Prepare); };

            AddButton("ui.close", (s, e) => Close());
            go = AddButton("prepare.go", (s, e) => RunPrepare(), true);
            go.Enabled = PrepareReady();
            if (_prepFromWelcome) AddButton("ui.back", (s, e) => { _flow = "install"; Show(Screen.Welcome); });
            CancelButton = (IButtonControl)_buttons.Controls[0];
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
            if (_flow == "repair") args.Add("-Repair");
            args.AddRange(_args.EngineArgs);
            StartRun("plan", Path.Combine(_package, "installer", "install.ps1"), args, Screen.Checking);
        }

        void RunInstall()
        {
            var args = new List<string>();
            if (_flow == "repair") args.Add("-Repair");
            if (_consentTestSigning) args.Add("-AcceptTestSigning");
            if (_bitLocker.Length > 0) { args.Add("-BitLocker"); args.Add(_bitLocker); }
            if (_args.DryRun) args.Add("-DryRun");
            args.AddRange(_args.EngineArgs);
            StartRun("install", Path.Combine(_package, "installer", "install.ps1"), args, Screen.Working);
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
            if (_client == null) return;
            if (!_client.Poll()) return;
            if (!_client.Finished) { Show(Current); return; }
            Finish(_client);
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
            _guideTitle.Text = Strings.T(_showNagi ? "guide.title" : "guide.title.plain");
            _guideText.Text = Strings.T(cause == SetupCause.Welcome && _flow == "prepare" ? "guide.prepare" : SetupGuide.TextId(cause));
            GuideCause = cause;
        }

        public SetupCause GuideCause { get; private set; }

        void SetGuideNote(string text) { if (_guideText != null) _guideText.Text = text; }

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
            w.AppendLine("amdgpu-wddm Setup " + Program.VersionText + ", " + DateTime.UtcNow.ToString("o"));
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

        // Tab order: the language choice, the screen from top to bottom, then the buttons from left to right.
        void AssignTabOrder()
        {
            int i = 0;
            _header.TabIndex = i++;
            _rail.TabIndex = i++; _rail.TabStop = false;
            _column.TabIndex = i++;
            _guide.TabIndex = i++; _guide.TabStop = false;
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
            _consentTestSigning = f.ConsentTestSigning;
            _bitLocker = f.BitLocker ?? "";
            _restartLater = f.RestartLater;
            _prepDestination = f.PrepareDestination ?? "";
            _prepUseFirmware = f.PrepareFirmware != null;
            _prepFirmware = f.PrepareFirmware ?? "";
            _prepFromWelcome = f.Flow == "prepare";
            _supportSaved = f.SupportSaved;
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
            return w.ToString();
        }

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
            }
            var order = reachable.Select(TabPath).ToList();
            if (order.Distinct().Count() != order.Count) problems.Add(Current + ": two controls share a tab position");
            bool working = Current == Screen.Working || Current == Screen.Checking;
            if (!working && AcceptButton == null) problems.Add(Current + ": no default button for Enter");
            if (!working && CancelButton == null) problems.Add(Current + ": no button for Escape");
            return problems;
        }

        static void Walk(Control c, List<Control> into)
        {
            foreach (Control k in c.Controls)
            {
                if (!HasVisibleFlag(k)) continue;
                if (k is ButtonBase || k is TextBox || k is ListControl) into.Add(k);
                Walk(k, into);
            }
        }

        static string TabPath(Control c)
        {
            var parts = new List<string>();
            for (var p = c; p != null; p = p.Parent) parts.Insert(0, p.TabIndex.ToString("D4") + p.GetHashCode());
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

        static string Describe(Control c) { return c.GetType().Name + (string.IsNullOrEmpty(c.Text) ? "" : " \"" + (c.Text.Length > 40 ? c.Text.Substring(0, 40) : c.Text) + "\""); }
    }
}
