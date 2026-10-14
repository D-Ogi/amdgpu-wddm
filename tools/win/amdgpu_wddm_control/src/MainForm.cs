// The window (WU-008): a header with search and the language switch, the nine pages in a navigation bar on the
// left, the page in the middle and a side panel on the right (the explanation of a setting, WU-064, and the guide
// panel, section 6). Pages are built from the models below, so a language switch, a resize or a refresh rebuilds
// them without losing unsaved edits. Live values come from Level-One escapes every 2 s, and only while Home or
// Performance is shown and the window is visible and not minimised (WU-078). Every change goes through a planned
// action (Recovery.Plan, one UAC prompt each); the window never writes HKLM itself.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace AmdgpuWddmControl
{
    public sealed partial class MainForm : Form
    {
        public static readonly string[] PageNames = { "home", "games", "graphics", "display", "performance", "driver", "system", "settings", "help" };

        readonly bool _smoke;
        readonly AppPrefs _prefs;

        // ---- models ----
        RecoverySnapshot _snap = new RecoverySnapshot();
        InventoryState _inv = new InventoryState();
        DriverCardView _drv;
        UpdateCache _upd;
        List<RecentLaunch> _recent = new List<RecentLaunch>();
        RecentListState _recentState = RecentListState.Missing;
        VideoMemoryState _vram;
        HwmonState _fan;            // the board's hardware monitor, read on the same 2 s poll
        StatusCard _status;
        GuideVerdict _verdict;
        GuideCause? _work;                      // the user-started work that runs (rank 4), null: none
        bool _laterRestart, _cuConfirmFailed;
        string _upgradeDone;        // the release of the upgrade-done verdict while it still holds (936 A1)
        string _lastResult;                 // the plain result of the last action, shown on the page that ran it
        string _lastResultPage;
        bool _lastResultOk;

        // ---- navigation ----
        sealed class Place { public string Page, Game; public int Scroll; }
        string _page = "home";
        readonly Stack<Place> _back = new Stack<Place>();
        string _explainId;                  // the setting whose explanation the side panel shows; null: the page's

        // ---- controls ----
        Panel _header, _nav, _side, _content;
        TextBox _search;
        ListBox _results;
        Button _backButton;
        GuidePanel _guide;
        Label _explainTitle, _explainText;
        readonly Dictionary<string, Button> _navButtons = new Dictionary<string, Button>();
        readonly Dictionary<string, Control> _anchors = new Dictionary<string, Control>();
        readonly Dictionary<string, Label> _live = new Dictionary<string, Label>();
        readonly Timer _timer = new Timer { Interval = 2000 };
        int _ticks;

        public MainForm() : this(false) { }

        // workArea: the working area of the screen this window is drawn for, in logical units, for a render or a
        // test. It is given to the constructor, because the window takes its own size and builds its first page
        // there - exactly as the live window does on the screen it opens on.
        public MainForm(bool smoke, AppPrefs prefs = null, Size workArea = default(Size))
        {
            _smoke = smoke;
            WorkAreaForLayout = workArea;
            _prefs = prefs ?? (smoke ? new AppPrefs(new MemoryPrefStore()) : AppPrefs.Live());
            Strings.Language = _prefs.EffectiveLanguage;
            Theme.Fonts();
            Text = Program.ProductName;
            BackColor = Theme.Back; ForeColor = Theme.Text; Font = Theme.Body;
            StartPosition = FormStartPosition.CenterScreen;
            ApplyScreenSize();
            KeyPreview = true;
            // The application's own icon (art\icon\nagi.ico), embedded by build.ps1 twice: as the Win32 icon of the
            // exe and as the resource read here. The resource carries every authored size, so the task bar and
            // Alt+Tab take the image drawn for the size they need; ExtractAssociatedIcon hands out one 32x32 image,
            // which a 150 % or 200 % screen rescales. An icon that cannot be read is no reason not to start.
            try
            {
                var iconAsm = System.Reflection.Assembly.GetExecutingAssembly();
                using (var iconStream = iconAsm.GetManifestResourceStream("nagi.ico"))
                {
                    if (iconStream != null) Icon = new Icon(iconStream);
                    else Icon = Icon.ExtractAssociatedIcon(iconAsm.Location);
                }
            }
            catch (Exception) { }
            BuildShell();
            _timer.Tick += (s, e) => Tick();
            if (!_smoke)
            {
                // The shell is built before the handle exists, from the screen the window is about to open on
                // (ApplyScreenSize). The window manager may still clamp the window to the desktop, and FitToScreens
                // can move it: whatever column is left after that is the column the page is built for, so the page
                // is rebuilt here when the two differ. Without it a 1240 px page stood in a 650 px column on the
                // owner's 800x600 screen and was cut on the right, which is the finding of GitHub #38.
                Load += (s, e) => { FitToScreens(); RebuildIfWidthChanged(); RefreshAll(); StartUpdateCheck(false); UpdateTimer(); };
                VisibleChanged += (s, e) => UpdateTimer();
                Resize += (s, e) => { UpdateTimer(); if (_guide != null) _guide.Paused = WindowState == FormWindowState.Minimized; };
                ResizeEnd += (s, e) => RebuildIfWidthChanged();
                UpdateCheck.Finished += OnUpdateFinished;
            }
        }

        public void CreateControlTree() { CreateControl(); }

        // ---- the shell -------------------------------------------------------------------------------------------

        int NavWidth { get { return Theme.S(210); } }
        int SideWidth { get { return Theme.S(300); } }

        // The working area of the screen that holds the window, in device pixels. A test and a render set it through
        // WorkAreaForLayout (logical units), so that neither reads the screen of the PC it happens to run on; the
        // live window asks Windows for the screen the window is on now.
        public Size WorkAreaForLayout { get; set; }

        Size ScreenWorkArea()
        {
            if (WorkAreaForLayout.Width > 0 && WorkAreaForLayout.Height > 0) return Theme.Sz(WorkAreaForLayout.Width, WorkAreaForLayout.Height);
            if (_smoke) return Theme.Sz(LargeScreenWidth, LargeScreenHeight);
            try
            {
                var screen = IsHandleCreated ? Screen.FromHandle(Handle) : Screen.FromPoint(Cursor.Position);
                return screen.WorkingArea.Size;
            }
            catch (Exception) { return Theme.Sz(LargeScreenWidth, LargeScreenHeight); }
        }

        // What a render or a test that says nothing about the screen gets: a screen with room for the whole window,
        // which is the screen every render until this rule assumed.
        const int LargeScreenWidth = 2560, LargeScreenHeight = 1440;

        // The size the window opens at, and the smallest size a person may drag it to: the design size on a screen
        // that has room for it, and the screen itself where it has not. Both have to come from the screen. A fixed
        // 900 px smallest width made the window wider than an 800x600 screen, Windows could not clamp it, and the
        // page lost its right-hand edge however the small-screen rule answered (GitHub #38).
        public static Size DesignSize { get { return Theme.Sz(1240, 760); } }
        public static Size SmallestSize { get { return Theme.Sz(900, 560); } }

        void ApplyScreenSize()
        {
            var want = DesignSize;
            var least = SmallestSize;
            var work = ScreenWorkArea();
            if (work.Width > 0 && work.Height > 0)
            {
                int margin = Theme.S(24);
                want = new Size(Math.Max(Theme.S(640), Math.Min(want.Width, work.Width - margin)),
                                Math.Max(Theme.S(440), Math.Min(want.Height, work.Height - margin)));
                least = new Size(Math.Min(least.Width, want.Width), Math.Min(least.Height, want.Height));
            }
            MinimumSize = least;
            ClientSize = want;
        }

        // The owner's rule of 2026-10-12 (LayoutRules.SmallScreen): no side panel at all on a small screen.
        public bool SmallScreen { get { var w = ScreenWorkArea(); return LayoutRules.SmallScreen.IsDevice(w.Width, w.Height, Theme.Scale); } }

        // The side panel: never on a small screen, and on a larger one only while the window is wide enough for it.
        public bool SideShown { get { return !SmallScreen && ClientSize.Width >= Theme.S(1000); } }

        // The page column: what is left between the bar and the side panel, at most 780 px at 96 DPI.
        public int ColumnWidth
        {
            get
            {
                int room = ClientSize.Width - NavWidth - (SideShown ? SideWidth : 0) - Theme.S(48) - SystemInformation.VerticalScrollBarWidth - Theme.S(4);
                return Math.Max(Theme.S(380), Math.Min(Theme.S(780), room));
            }
        }

        int _builtWidth;
        bool _builtSmall;           // the answer of the small-screen rule the shell in the window was built with

        // The column width the page now in the content panel was built at. ColumnWidth above is the width of this
        // moment, and the two differ whenever ClientSize changed after the page was built: the window manager clamps a
        // client size larger than the desktop to the maximum tracking size, so a 1240 px client becomes 1028 px on a
        // 1024x768 session-0 desktop, and the page built before the handle existed keeps the wider column it was given.
        // The --smoke-render width check uses this width, never the live one (b24 lab round 3).
        int _pageColumn;

        public int PageColumnWidth { get { return _pageColumn; } }

        void RebuildIfWidthChanged()
        {
            if (Math.Abs(ColumnWidth - _builtWidth) > Theme.S(8)) { BuildShell(); }
        }

        // The screen under the window can change while it is open: the person drags the window to another monitor, or
        // Windows changes the desktop under it. The small-screen rule is answered again then, and the shell is rebuilt
        // only when the answer changed, because a rebuild throws away every control of the page.
        void RebuildIfScreenChanged()
        {
            if (_smoke || IsDisposed || _building || _moving || !IsHandleCreated) return;
            if (SmallScreen == _builtSmall) return;
            // The window may now stand on a screen of another size: its own size and the smallest size a person may
            // drag it to are the new screen's, and the page is built for the column that leaves.
            ApplyScreenSize();
            BuildShell();
        }

        bool _building;

        void BuildShell()
        {
            _building = true;
            try
            {
            SuspendLayout();
            var old = Controls.Cast<Control>().ToList();
            Controls.Clear();
            foreach (var c in old) c.Dispose();
            _navButtons.Clear();

            _header = new Panel { Dock = DockStyle.Top, Height = Theme.S(60), BackColor = Theme.Nav, Padding = Theme.Pad(16, 12, 16, 8) };
            var left = new FlowLayoutPanel { Dock = DockStyle.Left, AutoSize = true, WrapContents = false, BackColor = Color.Transparent };
            // The Nagi mark beside the product name (NagiMark.cs); decorative, the name says the same.
            var mark = new NagiMarkBox(Theme.S(30)) { Margin = Theme.Pad(0, 2, 8, 0) };
            left.Controls.Add(mark);
            var brand = Ui.Label(Program.ProductName, Theme.Brand);
            brand.Margin = Theme.Pad(0, 4, 18, 0);
            left.Controls.Add(brand);
            _backButton = Ui.Button("← " + Strings.T("ui.back"), (s, e) => GoBack());
            _backButton.Margin = Theme.Pad(0, 0, 10, 0);
            _backButton.AccessibleName = Strings.T("ui.back");
            _backButton.Enabled = _back.Count > 0;
            left.Controls.Add(_backButton);
            _search = new TextBox { Width = Theme.S(250), Font = Theme.Body, BackColor = Theme.Card, ForeColor = Theme.Text, BorderStyle = BorderStyle.FixedSingle, Margin = Theme.Pad(0, 3, 0, 0), AccessibleName = Strings.T("ui.search") };
            _search.HandleCreated += (s, e) => SendMessage(_search.Handle, EM_SETCUEBANNER, (IntPtr)1, Strings.T("ui.search") + "...");
            _search.TextChanged += (s, e) => ShowResults();
            _search.KeyDown += SearchKey;
            left.Controls.Add(_search);
            _header.Controls.Add(left);
            var langs = new FlowLayoutPanel { Dock = DockStyle.Right, AutoSize = true, WrapContents = false, BackColor = Color.Transparent };
            foreach (var l in Strings.Languages)
            {
                string lang = l;
                var b = Ui.Button(LanguageName(l), (s, e) => SetLanguage(lang));
                b.Margin = Theme.Pad(4, 0, 0, 0);
                b.Font = lang == Strings.Language ? Theme.Bold : Theme.Body;
                b.BackColor = lang == Strings.Language ? Theme.CardHi : Theme.Nav;
                b.AccessibleName = LanguageName(l);
                b.Tag = l;
                b.AccessibleDescription = lang == Strings.Language ? Strings.T("ui.language.current") : null;
                langs.Controls.Add(b);
            }
            _header.Controls.Add(langs);
            // The search box takes what the brand, Back and the language buttons leave. In a narrow window or with large
            // text the brand goes first (the title bar names the app), then the language buttons show short codes;
            // their accessible names stay the full language names.
            Func<int> room = () => ClientSize.Width - _header.Padding.Horizontal - langs.PreferredSize.Width - (left.PreferredSize.Width - _search.Width) - Theme.S(24);
            if (room() < Theme.S(150)) { brand.Visible = false; left.Controls.Remove(brand); }
            if (room() < Theme.S(150))
                foreach (Button b in langs.Controls) b.Text = ((string)b.Tag).ToUpperInvariant();
            _search.Width = Math.Max(Theme.S(110), Math.Min(Theme.S(250), room()));
            _header.Height = Math.Max(Theme.S(60), Math.Max(left.PreferredSize.Height, langs.PreferredSize.Height) + _header.Padding.Vertical);

            _nav = new FlowLayoutPanel { Dock = DockStyle.Left, Width = NavWidth, BackColor = Theme.Nav, FlowDirection = FlowDirection.TopDown, WrapContents = false, Padding = Theme.Pad(8, 12, 8, 0) };
            foreach (var name in PageNames)
            {
                string page = name;
                var b = new Button
                {
                    Text = Strings.T("nav." + name), Width = NavWidth - Theme.S(16), Height = Theme.S(44), FlatStyle = FlatStyle.Flat, TextAlign = ContentAlignment.MiddleLeft,
                    ForeColor = Theme.Text, BackColor = Theme.Nav, Font = Theme.Bold, Margin = Theme.Pad(0, 0, 0, 4), Cursor = Cursors.Hand, Padding = Theme.Pad(12, 0, 0, 0),
                    AccessibleName = Strings.T("nav." + name), UseMnemonic = false,
                };
                b.FlatAppearance.BorderSize = 0;
                b.GotFocus += (s, e) => { b.FlatAppearance.BorderSize = 2; b.FlatAppearance.BorderColor = Theme.Focus; };
                b.LostFocus += (s, e) => { b.FlatAppearance.BorderSize = 0; };
                b.Click += (s, e) => Navigate(page);
                _nav.Controls.Add(b);
                _navButtons[name] = b;
            }

            _side = new Panel { Dock = DockStyle.Right, Width = SideWidth, BackColor = Theme.Back, Padding = Theme.Pad(0, 18, 16, 16), Visible = SideShown };
            var sideStack = Ui.Stack(SideWidth - Theme.S(16));
            sideStack.Dock = DockStyle.Fill;
            var explain = new CardPanel(null, SideWidth - Theme.S(16));
            _explainTitle = explain.Add(Ui.Label("", Theme.CardTitle, null, explain.Inner));
            _explainText = explain.Add(Ui.Label("", null, Theme.Dim, explain.Inner));
            sideStack.Controls.Add(explain);
            _guide = new GuidePanel(SideWidth - Theme.S(16), _prefs);
            sideStack.Controls.Add(_guide);
            _side.Controls.Add(sideStack);

            _content = Theme.DarkScroll(new Panel { Dock = DockStyle.Fill, BackColor = Theme.Back, Padding = Theme.Pad(24, 18, 24, 18), AutoScroll = true });

            _results = new ListBox
            {
                Visible = false, Font = Theme.Body, BackColor = Theme.CardHi, ForeColor = Theme.Text, BorderStyle = BorderStyle.FixedSingle, IntegralHeight = false,
                Width = Theme.S(420), Height = Theme.S(220), AccessibleName = Strings.T("ui.search.results"),
            };
            _results.KeyDown += ResultsKey;
            _results.DoubleClick += (s, e) => JumpToResult();

            Controls.Add(_content); Controls.Add(_side); Controls.Add(_nav); Controls.Add(_header); Controls.Add(_results);
            _results.BringToFront();
            Font = Theme.Body;
            ResumeLayout(true);
            _builtWidth = ColumnWidth;
            _builtSmall = SmallScreen;
            ShowPage(_page, null, false);
            }
            finally { _building = false; }
        }

        static string LanguageName(string lang)
        {
            switch (lang) { case "pl": return "Polski"; case "ja": return "日本語"; case "ko": return "한국어"; default: return "English"; }
        }

        void SetLanguage(string lang)
        {
            if (lang == Strings.Language) return;
            _prefs.Language = lang;
            Strings.Language = lang;
            Theme.Fonts();
            BuildShell();
        }

        // Runtime language switch for the render gate: the same window, another language, nothing written.
        public void SwitchLanguageForTest(string lang)
        {
            Strings.Language = lang;
            Theme.Fonts();
            BuildShell();
        }

        const int EM_SETCUEBANNER = 0x1501;

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        static extern IntPtr SendMessage(IntPtr hwnd, int msg, IntPtr wParam, string lParam);

        // ---- navigation (WU-011) -----------------------------------------------------------------------------------

        public string CurrentPage { get { return _page; } }

        Place Here() { return new Place { Page = _page, Game = _game, Scroll = _content != null ? -_content.AutoScrollPosition.Y : 0 }; }

        // Leaving a page with unsaved changes asks Save / Discard / Keep editing; Escape keeps editing.
        bool LeaveAllowed()
        {
            if (!Dirty(_page)) return true;
            switch (ThreeWayDialog.Ask(this, Strings.T("ui.unsaved.title"), Strings.T("ui.unsaved.text", Strings.T("nav." + _page))))
            {
                case ThreeWay.Save: return SavePage(_page);
                case ThreeWay.Discard: DiscardPage(_page); return true;
                default: return false;
            }
        }

        public void Navigate(string page, string anchor = null)
        {
            if (page != _page && !LeaveAllowed()) return;
            if (page != _page) _back.Push(Here());
            _explainId = null;
            ShowPage(page, anchor, true);
            if (page == "system") ReadSystemTuning();
        }

        void GoBack()
        {
            if (_back.Count == 0) return;
            if (_back.Peek().Page != _page && !LeaveAllowed()) return;
            var p = _back.Pop();
            if (p.Game != null) _game = p.Game;
            _explainId = null;
            ShowPage(p.Page, null, true);
            if (p.Page == "system") ReadSystemTuning();
            _content.AutoScrollPosition = new Point(0, p.Scroll);
        }

        void ShowPage(string page, string anchor, bool focus)
        {
            // A rebuild of the same page keeps the keyboard focus on the control with the same name (G-A11Y) and the
            // place the person had scrolled to. The card that held the focus is kept as well: the control itself can
            // be gone or disabled after the rebuild (the Start button of the benchmark card while its run goes on),
            // and then the focus goes to the next control of the same card instead of leaving the page.
            string focusName = null, focusCard = null;
            if (page == _page && ActiveControl != null && _content.Contains(ActiveControl))
            {
                focusName = FocusKey(ActiveControl);
                focusCard = CardOf(ActiveControl);
            }
            int at = -_content.AutoScrollPosition.Y;
            int scroll = page == _page ? at : 0;
            var cards = scroll > 0 ? Cards() : new List<CardBox>();
            // Every page is built with the panel at its top. A panel measures its scroll range from where its
            // children sit in the scrolled client, so a page built into a scrolled panel comes out with a range too
            // large by the scroll offset: the view then stands past the end of the new content, and every further
            // rebuild adds its offset again (b23 lab round, F1). A rebuild of the same page puts the place back at
            // the end of this method; another page opens at its top, where a new page belongs.
            if (at != 0) _content.AutoScrollPosition = Point.Empty;
            if (page == "graphics" && TuningAnchor(anchor)) _tuningOpen = true;
            _page = page;
            _anchors.Clear(); _live.Clear();
            _content.SuspendLayout();
            var old = _content.Controls.Cast<Control>().ToList();
            _content.Controls.Clear();
            foreach (var c in old) c.Dispose();
            int column = ColumnWidth;
            var built = BuildPage(page, column);
            _pageColumn = column;
            _content.Controls.Add(built);
            _content.ResumeLayout(true);
            foreach (var kv in _navButtons)
            {
                kv.Value.BackColor = kv.Key == page ? Theme.CardHi : Theme.Nav;
                kv.Value.AccessibleDescription = kv.Key == page ? Strings.T("ui.nav.current") : null;
            }
            if (_backButton != null) _backButton.Enabled = _back.Count > 0;
            ShowExplain(_explainId);
            UpdateGuide();
            UpdateTimer();
            // The focus goes back before the place: giving a control the focus scrolls it into view, which would move
            // a view that had just been put back (b23 lab round, F1: the page moved by its own scroll offset).
            KeepFocus(focusName, focusCard);
            if (anchor == null && scroll > 0) _content.AutoScrollPosition = new Point(0, PagePlace.Keep(scroll, cards, Cards()));
            if (anchor != null)
            {
                Control target;
                if (_anchors.TryGetValue(anchor, out target))
                {
                    _content.ScrollControlIntoView(target);
                    if (focus && !_smoke) target.Focus();
                    if (Strings.Has("help.setting." + SettingOf(anchor))) ShowExplain(SettingOf(anchor));
                }
            }
        }

        static string FocusKey(Control c) { return c.GetType().Name + "|" + (c.AccessibleName ?? c.Text); }

        // The card that holds a control: the innermost marked anchor around it, because a page marks sections inside
        // cards as well. null for a control that belongs to no marked card.
        string CardOf(Control c)
        {
            string card = null;
            int deepest = -1;
            foreach (var kv in _anchors)
            {
                if (kv.Value != c && !kv.Value.Contains(c)) continue;
                int depth = 0;
                for (var p = kv.Value; p != null && p != _content; p = p.Parent) depth++;
                if (depth > deepest) { deepest = depth; card = kv.Key; }
            }
            return card;
        }

        // The keyboard focus after a rebuild, by the rule of PageFocus.After. The gates build pages without a window,
        // where no control can take the focus, so they never ask for it.
        void KeepFocus(string focusName, string focusCard)
        {
            if (focusName == null || _smoke) return;
            var controls = All(_content).ToList();
            var page = controls.Select(c => new FocusCandidate(FocusKey(c), CardOf(c), c.CanFocus, c.CanSelect && c.TabStop)).ToList();
            int at = PageFocus.After(focusName, focusCard, page);
            if (at >= 0) controls[at].Focus();
        }

        public static IEnumerable<Control> All(Control root)
        {
            foreach (Control c in root.Controls)
            {
                yield return c;
                foreach (var k in All(c)) yield return k;
            }
        }

        static string SettingOf(string anchor){ return anchor.Substring(anchor.IndexOf('.') + 1); }

        Control BuildPage(string page, int width)
        {
            switch (page)
            {
                case "games": return BuildGames(width);
                case "graphics": return BuildGraphics(width);
                case "display": return BuildDisplay(width);
                case "performance": return BuildPerformance(width);
                case "driver": return BuildDriver(width);
                case "settings": return BuildSettings(width);
                case "system": return BuildSystemTuning(width);
                case "help": return BuildHelp(width);
                default: return BuildHome(width);
            }
        }

        // The page frame: title, intro, then the cards.
        FlowLayoutPanel Frame(string page, int width)
        {
            var p = Ui.Stack(width);
            p.BackColor = Theme.Back;
            p.Controls.Add(Ui.Label(Strings.T("page." + page + ".title"), Theme.Title, null, width));
            var intro = Ui.Dim(Strings.T("page." + page + ".intro"), width);
            intro.Margin = Theme.Pad(0, 2, 0, 14);
            p.Controls.Add(intro);
            return p;
        }

        void Mark(string id, Control c) { _anchors[id] = c; }

        // Where the marked cards of the page stand, for the rule that keeps the place of a rebuilt page
        // (PagePlace.Keep). A card that was built but never laid out has no place yet, and is left out.
        List<CardBox> Cards()
        {
            var list = new List<CardBox>();
            if (_content == null) return list;
            foreach (var kv in _anchors)
            {
                var c = kv.Value;
                if (c == null || c.IsDisposed || c.Parent == null) continue;
                list.Add(new CardBox(kv.Key, ContentTop(c), c.Height));
            }
            return list;
        }

        // The top edge of a control in the page, which is the same number whatever the page is scrolled to: the
        // panel moves the page it holds, so the place of the page inside the panel is its padding.
        int ContentTop(Control c)
        {
            var page = _content.Controls.Count > 0 ? _content.Controls[0] : null;
            int y = _content.Padding.Top;
            for (var p = c; p != null && p != page && p != _content; p = p.Parent) y += p.Top;
            return y;
        }

        // Brings a row of controls of a marked card into reach, without touching the side panel or the keyboard
        // focus. The arithmetic is ours (PagePlace.Bring), not ScrollControlIntoView: the person must be able to
        // tell where the page goes, and a gate without a window must measure the same thing the window does.
        void BringIntoView(string card, string row)
        {
            Control c, r;
            if (_content == null || !_anchors.TryGetValue(card, out c) || !_anchors.TryGetValue(row, out r)) return;
            int scroll = -_content.AutoScrollPosition.Y;
            int place = PagePlace.Bring(scroll, _content.ClientSize.Height, ContentTop(c), ContentTop(r), r.Height);
            if (place != scroll) _content.AutoScrollPosition = new Point(0, place);
        }

        // ---- the side panel ----------------------------------------------------------------------------------------

        void ShowExplain(string setting)
        {
            _explainId = setting;
            if (_explainTitle == null) return;
            if (setting == null)
            {
                _explainTitle.Text = Strings.T("ui.explain.page");
                _explainText.Text = Strings.T("help.page." + _page);
            }
            else
            {
                _explainTitle.Text = Strings.Has("search." + SearchIdOf(setting)) ? Strings.T("search." + SearchIdOf(setting)) : Strings.T("ui.explain.setting");
                _explainText.Text = Strings.T("help.setting." + setting);
            }
            if (!SideShown && setting != null && !_smoke)
                MessageBox.Show(this, _explainText.Text, _explainTitle.Text, MessageBoxButtons.OK, MessageBoxIcon.None);
        }

        static string SearchIdOf(string setting)
        {
            var hit = SettingsSearch.Index.FirstOrDefault(e => e.Id.EndsWith("." + setting, StringComparison.Ordinal));
            return hit != null ? hit.Id : setting;
        }

        Button Explain(string setting, string name)
        {
            return Ui.Explain(name, () => ShowExplain(setting));
        }

        void UpdateGuide()
        {
            if (_guide == null) return;
            var causes = new List<GuideCause>();
            if (_status != null) causes.AddRange(_status.Causes);
            if (_status != null && _status.Healthy) causes.Add(GuideCause.AllGood);      // explicit, only after a read (497.4)
            if (_page == "home" && !_prefs.GettingStartedDismissed) causes.Add(GuideCause.Welcome);
            if (_page == "help") causes.Add(GuideCause.About);
            if ((_page == "settings" || _page == "display" || _page == "graphics") && Guide.ShowUnaskedTip(_prefs.ShowTipsAutomatically)) causes.Add(GuideCause.PageTip);
            _verdict = Guide.Choose(causes);
            string text = _verdict == null ? Strings.T("status.unreadable")
                : _verdict.Cause == GuideCause.PageTip && Strings.Has("guide.tip." + _page) ? Strings.T("guide.tip." + _page) : Strings.T(_verdict.TextId);
            if (Guide.ShowNameTip(_nameTipShowing, _verdict)) text = Strings.T(Guide.NameTipId, Strings.T("brand.expansion"));
            _guide.Show(_verdict, text, Visible && WindowState != FormWindowState.Minimized);
        }

        public GuideVerdict Verdict { get { return _verdict; } }

        // ---- data --------------------------------------------------------------------------------------------------

        void ReadRecent()
        {
            var list = RecentLaunches.Read(new LocalRecentFiles());
            _recent = list.Entries;
            _recentState = list.State;
        }

        public void RefreshAll()
        {
            if (_page == "system") { ReadSystemTuning(); return; }
            if (_fixture) { ComputeStatus(); ShowPage(_page, null, false); return; }
            try { _inv = Inventory.Read(false); } catch (Exception) { _inv = new InventoryState(); }
            try { Kmd.RefreshBoardCapabilities();
            _snap = RecoveryProbe.Read("window"); } catch (Exception) { _snap = new RecoverySnapshot { DriverError = "unreadable", ReadFailed = true }; }
            var vram = Kmd.VideoMemory();
            _vram = vram.Value;
            _uma = Kmd.BoardMemoryQuery().Value;
            _fan = Kmd.Hwmon().Value;
            _upd = UpdateCheck.LoadCache();
            ReadRecent();
            ReadBenchmark();
            ReadDriverCard(!_smoke && !ReadOnlyProbe);
            ComputeStatus();
            ShowPage(_page, null, false);
            MaybeAutomaticDiagnostics();
            MaybeNagiWallpaper();
        }

        // The Nagi wallpaper of a verified install, once per install for this account (Wallpaper.cs), when the
        // sign-in run has not set it yet. Off the UI thread, once per window, and never in a render or a probe.
        bool _wallpaperChecked;

        void MaybeNagiWallpaper()
        {
            if (_wallpaperChecked || _smoke || ReadOnlyProbe || _fixture) return;
            _wallpaperChecked = true;
            var prefs = _prefs;
            Crash.Background("wallpaper at start", () => NagiWallpaper.RunIfDue(prefs));
        }

        int _drvRead;       // the newest Driver card read; an older one that ends later is dropped

        // The Driver card reads the installer's files and every verify report it needs: in the window off the UI thread
        // (936 A3), the card keeps its last answer until the read ends.
        void ReadDriverCard(bool background)
        {
            uint? reply = _snap.Dpm != null ? _snap.Dpm.Version : _snap.Health != null ? _snap.Health.Version : _snap.Interop != null ? _snap.Interop.Version : (uint?)null;
            if (reply == 0) reply = null;
            string installed = _inv.ReleaseVersion.Length > 0 ? _inv.ReleaseVersion : null, dir = _inv.ReleaseDir, date = _inv.DriverDate;
            Func<DriverCardView> read = () =>
            {
                try { return DriverCard.Decide(DriverCardProbe.Read(reply, DriverCardProbe.BootId(), installed, dir, date)); }
                catch (Exception) { return null; }
            };
            int id = ++_drvRead;
            if (!background) { ApplyDriverCard(read()); return; }
            Crash.Background("driver card read", () =>
            {
                var view = read();
                try
                {
                    BeginInvoke((Action)(() =>
                    {
                        if (id != _drvRead || IsDisposed) return;
                        string before = _drv != null ? _drv.ReportText : null, latch = _upgradeDone;
                        ApplyDriverCard(view);
                        ComputeStatus();
                        if ((view != null ? view.ReportText : null) != before || _upgradeDone != latch) ShowPage(_page, null, false); else UpdateGuide();
                    }));
                }
                catch (InvalidOperationException) { }
            });
        }

        void ApplyDriverCard(DriverCardView view)
        {
            _drv = view;
            _upgradeDone = DriverCard.UpgradeLatch(_upgradeDone, _drv, _prefs.LastSeenRelease);
            if (!_smoke && !ReadOnlyProbe && _drv != null && DriverCard.UpgradeVerified(_drv))
                _prefs.LastSeenRelease = _drv.InstalledVersion;
            if (!_smoke && !ReadOnlyProbe && _drv != null && !_nameTipShowing && Guide.NameTipDue(_drv.InstalledVersion, _prefs.NameTipRelease))
                StartNameTip(_drv.InstalledVersion);
        }

        // The name tip of the guide panel (Guide.ShowNameTip): on for NameTipSeconds after the first read of a release
        // this account has not seen the tip for, then the panel goes back to its verdict. The release is recorded at
        // once, so a window closed within the 15 seconds does not show the tip again. Nothing waits for it.
        bool _nameTipShowing;

        void StartNameTip(string release)
        {
            _prefs.NameTipRelease = release;
            _nameTipShowing = true;
            var t = new Timer { Interval = Guide.NameTipSeconds * 1000 };
            t.Tick += (s, e) =>
            {
                t.Stop();
                t.Dispose();
                _nameTipShowing = false;
                if (!IsDisposed) UpdateGuide();
            };
            t.Start();
        }

        void ComputeStatus()
        {
            _status = HomeStatus.Compute(new StatusInputs
            {
                Snapshot = _snap, Driver = _drv, Work = _work, UpgradeDone = _upgradeDone, UpdateAvailable = UpdateCheck.IsCandidateNewer(_upd, InstalledVersion),
                CuConfirmFailed = _cuConfirmFailed, LaterRestart = _laterRestart,
            });
        }

        string InstalledVersion { get { return _drv != null ? _drv.InstalledVersion : null; } }

        // The live part every 2 s: the driver's own snapshots (Level-One), the session's DWM, and the status card.
        void Tick()
        {
            _ticks++;
            var dwm = new RecoverySnapshot();
            RecoveryProbe.ReadCompositor(dwm, "window");
            _snap.DwmNow = dwm.DwmNow; _snap.DwmHistory = dwm.DwmHistory;
            var dpm = Kmd.Dpm();
            _snap.Dpm = dpm.Value;
            if (dpm.Value == null) _snap.DriverError = dpm.Error; else _snap.DriverError = null;
            var health = Kmd.StartHealth(); if (health.Value != null) _snap.Health = health.Value;
            var cu = Kmd.CuMode(); if (cu.Value != null) _snap.Cu = cu.Value;
            _vram = Kmd.VideoMemory().Value;
            _fan = Kmd.Hwmon().Value;
            TickTuning();
            TickFan();
            var before = _status == null ? "" : string.Join("|", _status.Items.Select(i => i.Text));
            ComputeStatus();
            var after = string.Join("|", _status.Items.Select(i => i.Text));
            if (before != after && _page == "home" && !Dirty(_page)) ShowPage(_page, null, false);
            else { RefreshLiveLabels(); UpdateGuide(); }
        }

        // Whether the 2 s timer runs (G-PERF): only while the window is visible, not minimised, and on a page with live values.
        public static bool LiveTimerWanted(bool visible, bool minimized, string page) { return Sensors.PollWanted(visible, minimized, page); }

        // The graphics page polls only while its tuning cards are open or a tuning trial runs: the countdown, the clock
        // and the temperature there are the driver's, and the closed page has nothing that changes by itself (G-PERF).
        public static bool LiveTimerWanted(bool visible, bool minimized, string page, bool tuning) { return Sensors.PollWanted(visible, minimized, page, tuning); }

        bool TuningTrialRunning
        {
            get
            {
                var c = CurveNow; var u = CpuNow;
                return (c != null && c.Has(CurveState.FlagOnTrial)) || (u != null && u.Has(CpuState.FlagOnTrial));
            }
        }

        void UpdateTimer()
        {
            if (_smoke) { _timer.Enabled = false; return; }
            _timer.Enabled = LiveTimerWanted(Visible, WindowState == FormWindowState.Minimized, _page, TuningTrialRunning || TuningShown);
        }

        public bool TimerRunning { get { return _timer.Enabled; } }
        public int Ticks { get { return _ticks; } }
        public bool FrameTimerRunning { get { return _guide != null && _guide.Playing; } }

        // ---- window recovery (WU-036) ------------------------------------------------------------------------------

        void FitToScreens()
        {
            var areas = Screen.AllScreens.OrderByDescending(s => s.Primary).Select(s => s.WorkingArea).ToList();
            var fitted = DisplayInfo.Fit(Bounds, areas, MinimumSize, SystemInformation.CaptionHeight);
            if (fitted != Bounds) { WindowState = FormWindowState.Normal; Bounds = fitted; }
        }

        const int WM_DISPLAYCHANGE = 0x007E, WM_DPICHANGED = 0x02E0;

        protected override void WndProc(ref Message m)
        {
            base.WndProc(ref m);
            // A display or DPI change can turn the screen under the window into a small one, or back: the window fits
            // itself to the new desktop and then asks the small-screen rule again.
            if ((m.Msg == WM_DISPLAYCHANGE || m.Msg == WM_DPICHANGED) && !_smoke)
                BeginInvoke((Action)(() => { FitToScreens(); RebuildIfScreenChanged(); }));
            // A drag of the window runs a message loop of its own inside Windows. Nothing is rebuilt while it
            // lasts: a page thrown away and built again on every message of a drag costs the person the page they
            // were reading, and a rebuild from inside that loop disposes controls it is still working with. The
            // rule is answered once, when the drag ends.
            if (m.Msg == WM_ENTERSIZEMOVE) _moving = true;
            if (m.Msg == WM_EXITSIZEMOVE && !_smoke)
            {
                _moving = false;
                BeginInvoke((Action)RebuildIfScreenChanged);
            }
        }

        const int WM_ENTERSIZEMOVE = 0x0231, WM_EXITSIZEMOVE = 0x0232;
        bool _moving;               // Windows is running the loop of a window drag

        // The window was moved, possibly onto another monitor: the small-screen rule decides again. A move the
        // person is making waits for the end of the drag; a move the program made is answered after this message.
        protected override void OnMove(EventArgs e)
        {
            base.OnMove(e);
            if (_moving || _smoke || IsDisposed || !IsHandleCreated) return;
            BeginInvoke((Action)RebuildIfScreenChanged);
        }

        // ---- keyboard ----------------------------------------------------------------------------------------------

        protected override bool ProcessCmdKey(ref Message msg, Keys keyData)
        {
            if (keyData == (Keys.Control | Keys.F)) { _search.Focus(); _search.SelectAll(); return true; }
            if (keyData == (Keys.Alt | Keys.Left)) { GoBack(); return true; }
            if (keyData == Keys.F5) { RefreshAll(); return true; }
            return base.ProcessCmdKey(ref msg, keyData);
        }

        // ---- search (WU-009) ---------------------------------------------------------------------------------------

        List<SearchHit> _hits = new List<SearchHit>();

        void ShowResults()
        {
            _hits = SettingsSearch.Find(_search.Text, Strings.Language);
            _results.Items.Clear();
            foreach (var h in _hits)
                _results.Items.Add(h.Entry.Name + "  -  " + Strings.T("nav." + h.Entry.Page) + (h.Entry.ComingLater ? "  (" + Strings.T("ui.later.short") + ")" : ""));
            if (_hits.Count == 0 && _search.Text.Trim().Length > 0) _results.Items.Add(Strings.T("ui.search.none"));
            var at = PointToClient(_search.Parent.PointToScreen(new Point(_search.Left, _search.Bottom)));
            _results.Location = new Point(at.X, at.Y + Theme.S(2));
            _results.Visible = _search.Text.Trim().Length > 0;
            if (_results.Visible) _results.BringToFront();
        }

        void SearchKey(object sender, KeyEventArgs e)
        {
            if (e.KeyCode == Keys.Down && _results.Visible && _hits.Count > 0) { _results.Focus(); _results.SelectedIndex = 0; e.Handled = true; }
            else if (e.KeyCode == Keys.Enter && _hits.Count > 0) { _results.SelectedIndex = 0; JumpToResult(); e.Handled = e.SuppressKeyPress = true; }
            else if (e.KeyCode == Keys.Escape) { _search.Text = ""; e.Handled = e.SuppressKeyPress = true; }
        }

        void ResultsKey(object sender, KeyEventArgs e)
        {
            if (e.KeyCode == Keys.Enter) { JumpToResult(); e.Handled = true; }
            else if (e.KeyCode == Keys.Escape) { _search.Focus(); _search.Text = ""; e.Handled = true; }
        }

        void JumpToResult()
        {
            int i = _results.SelectedIndex;
            if (i < 0 || i >= _hits.Count) return;
            var entry = _hits[i].Entry;
            _results.Visible = false;
            _search.Text = "";
            Navigate(entry.Page, entry.Id);
        }

        // ---- closing -----------------------------------------------------------------------------------------------

        protected override void OnFormClosing(FormClosingEventArgs e)
        {
            if (!_smoke && e.CloseReason == CloseReason.UserClosing && !LeaveAllowed()) { e.Cancel = true; return; }
            base.OnFormClosing(e);
        }

        protected override void OnFormClosed(FormClosedEventArgs e)
        {
            _timer.Stop();
            UpdateCheck.Finished -= OnUpdateFinished;
            if (_guide != null) _guide.Stop();
            base.OnFormClosed(e);
        }
    }
}
