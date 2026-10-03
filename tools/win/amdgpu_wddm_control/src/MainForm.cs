// The window: a navigation bar on the left and five pages (Overview, Performance, Applications, Recovery,
// Diagnostics). Live values come from the KMD's software snapshots every 2 s, and only while Overview or Performance
// is shown and the window is not minimized. The Applications page writes the registry through Program.RunElevated;
// the Recovery page and the clock settings go through the planned actions of Recovery.cs (DoAction).
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace AmdgpuWddmControl
{
    // Colours, fonts and one scale factor. The process is per-monitor DPI aware (app.manifest) and the form does no
    // automatic scaling, so every pixel size goes through S() and the fonts are sized in pixels with the same factor:
    // at 120 or 144 DPI text and boxes grow together, and an AutoSize label never runs into its neighbour.
    // The clock ceiling as "-  1500 MHz  +", drawn in the theme colours (a ComboBox draws its closed box white on
    // the dark theme): one 100 MHz step per click, held inside the 1000-2000 MHz grid.
    sealed class CeilingPicker : FlowLayoutPanel
    {
        readonly Label _value;
        int _index = -1;

        // A click on - or +, also at either end of the grid: the user chose a ceiling.
        public event EventHandler Stepped;

        public CeilingPicker()
        {
            AutoSize = true; WrapContents = false; Margin = Theme.Pad(0); BackColor = Color.Transparent;
            Controls.Add(Step("-", -1, "Lower clock ceiling"));
            _value = new Label
            {
                AutoSize = false, Size = Theme.Sz(96, 30), TextAlign = ContentAlignment.MiddleCenter, Font = Theme.Body,
                ForeColor = Theme.Text, BackColor = Theme.Nav, Margin = Theme.Pad(0, 6, 0, 6), AccessibleName = "Clock ceiling",
            };
            Controls.Add(_value);
            Controls.Add(Step("+", 1, "Higher clock ceiling"));
            Index = Array.IndexOf(DpmSettings.CeilingChoices, DpmSettings.DefaultMaxMHz);
        }

        Button Step(string text, int delta, string name)
        {
            var b = new Button
            {
                Text = text, Size = Theme.Sz(32, 30), FlatStyle = FlatStyle.Flat, Font = Theme.Bold, ForeColor = Theme.Text,
                BackColor = Theme.Card, Margin = Theme.Pad(0, 6, 0, 6), Cursor = Cursors.Hand, AccessibleName = name,
            };
            b.FlatAppearance.BorderColor = Theme.Line;
            b.Click += (s, e) => { Index += delta; if (Stepped != null) Stepped(this, EventArgs.Empty); };
            return b;
        }

        public int Index
        {
            get { return _index; }
            set { _index = Math.Max(0, Math.Min(DpmSettings.CeilingChoices.Length - 1, value)); _value.Text = Value + " MHz"; }
        }

        public uint Value { get { return DpmSettings.CeilingChoices[_index]; } }
    }

    static class Theme
    {
        public static readonly Color Back = Color.FromArgb(24, 24, 27), Nav = Color.FromArgb(16, 16, 18), Card = Color.FromArgb(36, 36, 40),
            Text = Color.FromArgb(232, 232, 236), Dim = Color.FromArgb(160, 160, 170), Accent = Color.FromArgb(237, 28, 36),
            Good = Color.FromArgb(90, 200, 120), Warn = Color.FromArgb(240, 180, 60), Line = Color.FromArgb(60, 60, 66);
        public static float Scale { get; private set; }
        public static Font Body, Bold, Title, CardTitle, Brand, Mono, MonoSmall;

        static Theme()
        {
            float dpi = 96;
            try { using (var g = Graphics.FromHwnd(IntPtr.Zero)) dpi = g.DpiX; } catch (Exception) { }
            Init(dpi / 96f);
        }

        // scale: 1.0 at 96 DPI. --smoke-render passes 1.25 or 1.5 to draw the pages as a 120 or 144 DPI screen shows them.
        public static void Init(float scale)
        {
            Scale = scale <= 0 ? 1 : scale;
            Body = Px("Segoe UI", 9.5f); Bold = Px("Segoe UI Semibold", 9.5f); Title = Px("Segoe UI Semibold", 15f);
            CardTitle = Px("Segoe UI Semibold", 11f); Brand = Px("Segoe UI Semibold", 13f); Mono = Px("Consolas", 9f); MonoSmall = Px("Consolas", 8.5f);
        }

        static Font Px(string name, float points) { return new Font(name, points * 96f / 72f * Scale, GraphicsUnit.Pixel); }

        public static int S(int px) { return (int)Math.Round(px * Scale); }
        public static Size Sz(int w, int h) { return new Size(S(w), S(h)); }
        public static Padding Pad(int all) { return new Padding(S(all)); }
        public static Padding Pad(int left, int top, int right, int bottom) { return new Padding(S(left), S(top), S(right), S(bottom)); }

        // A check box that stays readable: never disabled (a disabled check box draws its text etched in the system
        // colours, which on this dark background is the faded, doubled title the lab showed); a page hides what does not
        // apply instead.
        public static CheckBox Check(string text, Font font = null)
        {
            return new CheckBox { Text = text, AutoSize = true, ForeColor = Text, BackColor = Color.Transparent, Font = font ?? Body, Margin = Pad(0, 6, 0, 0), UseVisualStyleBackColor = false };
        }

        public static Label Hint(string text, int indent = 20, int width = 820)
        {
            var l = Label(text, null, Dim);
            l.Margin = Pad(indent, 0, 0, 4);
            l.MaximumSize = Sz(width - indent, 0);
            return l;
        }

        public static Label Narrow(Label l, int width) { l.MaximumSize = Sz(width, 0); return l; }

        [DllImport("uxtheme.dll", CharSet = CharSet.Unicode)]
        static extern int SetWindowTheme(IntPtr hwnd, string appName, string idList);

        [DllImport("user32.dll")]
        static extern IntPtr SendMessage(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam);

        const int LVM_GETHEADER = 0x101F;

        // Dark scroll bars on a scrolling control (the Explorer dark theme of Windows 10 1809 and later); an older
        // system keeps the light ones.
        public static T DarkScroll<T>(T c) where T : Control
        {
            c.HandleCreated += (s, e) => { try { SetWindowTheme(c.Handle, "DarkMode_Explorer", null); } catch (Exception) { } };
            return c;
        }

        // A list view in the theme colours: the column headers and the rows are drawn here (the system draws the
        // headers light and a selected row in the system highlight), the header's empty end uses the dark items
        // theme, and the scroll bars are dark.
        public static ListView DarkList(ListView lv)
        {
            DarkScroll(lv);
            lv.OwnerDraw = true;
            lv.HandleCreated += (s, e) =>
            {
                try { SetWindowTheme(SendMessage(lv.Handle, LVM_GETHEADER, IntPtr.Zero, IntPtr.Zero), "DarkMode_ItemsView", null); } catch (Exception) { }
            };
            const TextFormatFlags flags = TextFormatFlags.VerticalCenter | TextFormatFlags.Left | TextFormatFlags.EndEllipsis | TextFormatFlags.NoPrefix | TextFormatFlags.SingleLine;
            lv.DrawColumnHeader += (s, e) =>
            {
                using (var b = new SolidBrush(Nav)) e.Graphics.FillRectangle(b, e.Bounds);
                using (var pen = new Pen(Line))
                {
                    e.Graphics.DrawLine(pen, e.Bounds.Left, e.Bounds.Bottom - 1, e.Bounds.Right, e.Bounds.Bottom - 1);
                    e.Graphics.DrawLine(pen, e.Bounds.Right - 1, e.Bounds.Top + S(4), e.Bounds.Right - 1, e.Bounds.Bottom - S(5));
                }
                TextRenderer.DrawText(e.Graphics, e.Header.Text, lv.Font, Rectangle.Inflate(e.Bounds, -S(6), 0), Dim, flags);
            };
            lv.DrawItem += (s, e) => { };   // DrawSubItem draws every cell of the row
            lv.DrawSubItem += (s, e) =>
            {
                using (var b = new SolidBrush(e.Item.Selected ? Line : lv.BackColor)) e.Graphics.FillRectangle(b, e.Bounds);
                var fore = e.Item.UseItemStyleForSubItems ? e.Item.ForeColor : e.SubItem.ForeColor;
                TextRenderer.DrawText(e.Graphics, e.SubItem.Text, lv.Font, Rectangle.Inflate(e.Bounds, -S(6), 0), fore, flags);
            };
            // An owner-drawn list redraws only the first cell of a row under the mouse; redraw the whole row.
            lv.MouseMove += (s, e) => { var item = lv.GetItemAt(e.X, e.Y); if (item != null) lv.Invalidate(item.Bounds); };
            return lv;
        }

        public static Button Button(string text, EventHandler click, bool primary = false)
        {
            var b = new Button
            {
                Text = text, AutoSize = true, FlatStyle = FlatStyle.Flat, ForeColor = Text, Font = Bold,
                BackColor = primary ? Accent : Card, Padding = Theme.Pad(10, 4, 10, 4), Margin = Theme.Pad(0, 6, 10, 6), Cursor = Cursors.Hand,
            };
            b.FlatAppearance.BorderColor = primary ? Accent : Line;
            // A disabled primary button must not keep the accent colour: it would look ready to click.
            if (primary) b.EnabledChanged += (s, e) => { b.BackColor = b.Enabled ? Accent : Card; b.FlatAppearance.BorderColor = b.Enabled ? Accent : Line; };
            b.Click += click;
            return b;
        }

        public static Label Label(string text, Font font = null, Color? color = null)
        {
            return new Label { Text = text, AutoSize = true, Font = font ?? Body, ForeColor = color ?? Text, Margin = Theme.Pad(0, 3, 12, 3), MaximumSize = Theme.Sz(860, 0) };
        }
    }

    // A titled block with key/value rows; values are updated by key.
    sealed class Card : Panel
    {
        readonly TableLayoutPanel _rows;
        public readonly Dictionary<string, Label> Values = new Dictionary<string, Label>();

        public Card(string title)
        {
            BackColor = Theme.Card; AutoSize = true; AutoSizeMode = AutoSizeMode.GrowAndShrink; Padding = Theme.Pad(14, 10, 14, 12);
            Margin = Theme.Pad(0, 0, 14, 14);
            var stack = new FlowLayoutPanel { FlowDirection = FlowDirection.TopDown, AutoSize = true, WrapContents = false, Dock = DockStyle.Fill };
            stack.Controls.Add(Theme.Label(title, Theme.CardTitle));
            _rows = new TableLayoutPanel { ColumnCount = 2, AutoSize = true, Margin = Theme.Pad(0, 6, 0, 0) };
            _rows.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, Theme.S(190)));
            _rows.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            stack.Controls.Add(_rows);
            Controls.Add(stack);
        }

        public void Row(string key)
        {
            _rows.Controls.Add(Theme.Label(key, null, Theme.Dim));
            var value = Theme.Label("-");
            _rows.Controls.Add(value);
            Values[key] = value;
        }

        public void Add(Control c) { _rows.Controls.Add(c); _rows.SetColumnSpan(c, 2); }

        public void Set(string key, string text, Color? color = null)
        {
            Label l;
            if (!Values.TryGetValue(key, out l)) return;
            l.Text = string.IsNullOrEmpty(text) ? "-" : text;
            l.ForeColor = color ?? Theme.Text;
        }
    }

    public sealed class MainForm : Form
    {
        readonly bool _smoke;
        readonly Panel _content = Theme.DarkScroll(new Panel { Dock = DockStyle.Fill, BackColor = Theme.Back, Padding = Theme.Pad(24, 18, 24, 18), AutoScroll = true });
        readonly Dictionary<string, Panel> _pages = new Dictionary<string, Panel>();
        readonly Dictionary<string, Button> _nav = new Dictionary<string, Button>();
        readonly Label _status = Theme.Label("", null, Theme.Dim);
        readonly Timer _timer = new Timer { Interval = 2000 };
        string _page;

        Card _driver, _gpu, _perfNow;
        ListView _components;
        CheckBox _dpmOn, _ceilingOn;
        CeilingPicker _ceiling;
        Label _perfStored, _perfResult, _perfDirty;
        Button _perfApply, _perfRevert;
        uint? _storedMode, _storedMax;
        bool _driverInstalled, _loadingPerf;
        ListView _profiles;
        FlowLayoutPanel _switches, _otherSwitches;
        Label _noSelection, _profileResult, _profilesDirty, _reportStatus;
        Button _profilesApply, _profilesRevert;
        readonly Dictionary<string, CheckBox> _switchBoxes = new Dictionary<string, CheckBox>();
        readonly List<CheckBox> _otherBoxes = new List<CheckBox>();
        // Applications: the stored profiles and the unsaved edits (image -> every checked name), written only by Apply.
        SortedDictionary<string, string> _storedProfiles = new SortedDictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        readonly Dictionary<string, List<string>> _edits = new Dictionary<string, List<string>>(StringComparer.OrdinalIgnoreCase);
        string _shownImage;
        bool _loadingProfile;
        CheckBox _withDxdiag, _withCaps;
        Button _reportButton;
        InventoryState _inventory;
        FlowLayoutPanel _states;
        Label _undoText, _recoveryResult;
        TextBox _recoveryLog;
        CeilingPicker _recoveryCeiling;
        CheckBox _recoveryCeilingOn;
        readonly List<Button> _actionButtons = new List<Button>();
        List<StateLine> _stateLines = new List<StateLine>();

        public MainForm() : this(false) { }

        public MainForm(bool smoke)
        {
            _smoke = smoke;
            Text = Program.ProductName;
            BackColor = Theme.Back; ForeColor = Theme.Text; Font = Theme.Body;
            ClientSize = Theme.Sz(1180, 720); MinimumSize = Theme.Sz(820, 560); StartPosition = FormStartPosition.CenterScreen;
            try { Icon = Icon.ExtractAssociatedIcon(System.Reflection.Assembly.GetExecutingAssembly().Location); } catch (Exception) { }

            var nav = new FlowLayoutPanel { Dock = DockStyle.Left, Width = Theme.S(200), BackColor = Theme.Nav, FlowDirection = FlowDirection.TopDown, WrapContents = false, Padding = Theme.Pad(0, 16, 0, 0) };
            var brand = Theme.Label("amdgpu-wddm", Theme.Brand); brand.Margin = Theme.Pad(18, 0, 0, 0);
            var sub = Theme.Label("ASRock BC-250", null, Theme.Dim); sub.Margin = Theme.Pad(18, 0, 0, 18);
            var bar = new Panel { Width = Theme.S(200), Height = Theme.S(3), BackColor = Theme.Accent, Margin = Theme.Pad(0, 0, 0, 14) };
            nav.Controls.Add(brand); nav.Controls.Add(sub); nav.Controls.Add(bar);
            foreach (var name in new[] { "Overview", "Performance", "Applications", "Recovery", "Diagnostics" })
            {
                var b = new Button
                {
                    Text = "   " + name, Width = Theme.S(200), Height = Theme.S(42), FlatStyle = FlatStyle.Flat, TextAlign = ContentAlignment.MiddleLeft,
                    ForeColor = Theme.Text, BackColor = Theme.Nav, Font = Theme.Bold, Margin = Theme.Pad(0), Cursor = Cursors.Hand,
                };
                b.FlatAppearance.BorderSize = 0;
                string page = name;
                b.Click += (s, e) => ShowPage(page);
                nav.Controls.Add(b);
                _nav[name] = b;
            }
            var footer = new Panel { Dock = DockStyle.Bottom, Height = Theme.S(28), BackColor = Theme.Nav, Padding = Theme.Pad(12, 5, 12, 0) };
            footer.Controls.Add(_status);

            BuildOverview(); BuildPerformance(); BuildApplications(); BuildRecovery(); BuildDiagnostics();
            Controls.Add(_content); Controls.Add(nav); Controls.Add(footer);

            _timer.Tick += (s, e) => { if (WindowState != FormWindowState.Minimized && (_page == "Overview" || _page == "Performance")) RefreshLive(); };
            ShowPage("Overview");
            if (!_smoke)
            {
                Load += (s, e) => { RefreshAll(); _timer.Start(); HashInBackground(); };
            }
        }

        public void CreateControlTree() { CreateControl(); }

        void ShowPage(string page)
        {
            _page = page;
            _content.Controls.Clear();
            _content.Controls.Add(_pages[page]);
            foreach (var kv in _nav) kv.Value.BackColor = kv.Key == page ? Theme.Card : Theme.Nav;
            if (page == "Applications") RefreshProfiles();
            if (page == "Recovery" && !_smoke) RefreshRecovery();
        }

        Panel Page(string name, string title, string intro)
        {
            var p = new FlowLayoutPanel { FlowDirection = FlowDirection.TopDown, WrapContents = false, AutoSize = true, BackColor = Theme.Back };
            p.Controls.Add(Theme.Label(title, Theme.Title));
            var i = Theme.Label(intro, null, Theme.Dim); i.Margin = Theme.Pad(0, 2, 0, 14);
            p.Controls.Add(i);
            _pages[name] = p;
            return p;
        }

        // ---- Overview ----------------------------------------------------------------------------------------------

        void BuildOverview()
        {
            var p = Page("Overview", "Overview", "The driver and the GPU now. Values update every 2 seconds.");
            var row = new FlowLayoutPanel { AutoSize = true, WrapContents = true, MaximumSize = Theme.Sz(930, 0) };
            _driver = new Card("Driver");
            foreach (var k in new[] { "Status", "Release", "GPU", "Kernel driver", "Start check", "Driver package", "Driver date", "Video memory", "Test signing", "Desktop composition", "Recovery" }) _driver.Row(k);
            _gpu = new Card("GPU now");
            foreach (var k in new[] { "Clock", "Voltage", "Temperature", "Load", "Clock control", "Clock ceiling", "Limited by" }) _gpu.Row(k);
            row.Controls.Add(_driver); row.Controls.Add(_gpu);
            p.Controls.Add(row);
            p.Controls.Add(Theme.Label("Installed components", Theme.CardTitle));
            _components = new ListView
            {
                View = View.Details, FullRowSelect = true, HeaderStyle = ColumnHeaderStyle.Nonclickable, Width = Theme.S(900), Height = Theme.S(210),
                BackColor = Theme.Card, ForeColor = Theme.Text, BorderStyle = BorderStyle.None, Margin = Theme.Pad(0, 6, 0, 6),
            };
            _components.Columns.Add("Component", Theme.S(190)); _components.Columns.Add("File", Theme.S(400)); _components.Columns.Add("Version", Theme.S(130)); _components.Columns.Add("SHA-256", Theme.S(160));
            p.Controls.Add(Theme.DarkList(_components));
            p.Controls.Add(Theme.Button("Refresh", (s, e) => RefreshAll()));
        }

        public void RefreshAll()
        {
            _inventory = Inventory.Read(false);
            FillComponents();
            RefreshLive();
            RefreshStoredDpm();
            RefreshProfiles();
            RefreshRecovery();
        }

        void FillComponents()
        {
            _components.Items.Clear();
            foreach (var c in _inventory.Components)
            {
                var item = new ListViewItem(new[] { c.Role, c.Path, c.Exists ? c.Version : "missing", c.Sha256.Length >= 8 ? c.Sha256.Substring(0, 8) : "" });
                if (!c.Exists) item.ForeColor = Theme.Warn;
                _components.Items.Add(item);
            }
        }

        void HashInBackground()
        {
            Task.Run(() => Inventory.Read(true)).ContinueWith(t =>
            {
                if (t.Status == TaskStatus.RanToCompletion && !IsDisposed) { _inventory = t.Result; FillComponents(); }
            }, TaskScheduler.FromCurrentSynchronizationContext());
        }

        void RefreshLive()
        {
            var dpm = Kmd.Dpm();
            var inv = _inventory ?? new InventoryState();
            var ts = Kmd.TestSigning();
            bool present = inv.AdapterFound || inv.DevicePresent;
            _driver.Set("GPU", present ? (inv.AdapterName.Length > 0 ? inv.AdapterName : "BC-250") : "No BC-250 found in this PC",
                present ? (Color?)null : Theme.Warn);
            _driver.Set("Driver package", inv.DriverVersion);
            _driver.Set("Driver date", inv.DriverDate);
            _driver.Set("Release", inv.ReleaseVersion.Length > 0 ? inv.ReleaseVersion : "not installed by the release installer");
            _driver.Set("Test signing", ts == null ? "unknown" : ts.Value ? "On" : "Off (test-signed drivers do not load)", ts == false ? (Color?)Theme.Warn : null);
            if (dpm.Value == null)
            {
                bool missing = dpm.DriverMissing || dpm.Status == unchecked((int)0xC0000135);
                _driver.Set("Status", missing ? "Driver not found. You can still create a bug report on the Diagnostics page." : dpm.Error, Theme.Warn);
                _driver.Set("Kernel driver", !missing ? "-" : !inv.DevicePresent ? "not loaded" :
                    "not loaded; device uses \"" + inv.DeviceService + "\", " + Inventory.DeviceProblemText(inv.DeviceProblem), Theme.Warn);
                foreach (var k in new[] { "Clock", "Voltage", "Temperature", "Load", "Clock control", "Clock ceiling", "Limited by" }) _gpu.Set(k, "-");
                foreach (var k in new[] { "Video memory", "Start check" }) _driver.Set(k, "-");
                _driver.Set("Desktop composition", KmdReply.CompositionLine(DwmForceCpu(), null, "-"));
                _perfNow.Set("Running mode", dpm.Error, Theme.Warn);
                foreach (var k in new[] { "Ceiling", "Clock", "Temperature", "Limited by", "Start decision" }) _perfNow.Set(k, "-");
                SetStatus(missing ? "Driver not found" : dpm.Error);
                return;
            }
            var d = dpm.Value;
            _driver.Set("Status", "Running", Theme.Good);
            _driver.Set("Kernel driver", KmdReply.VersionText(d.Version));
            // The release's logon task confirms a healthy start; three unconfirmed starts in a row put the GPU on
            // Microsoft Basic Display (the KMD's boot-loop guard).
            var health = Kmd.StartHealth();
            bool confirmed = health.Value != null && (health.Value.Flags & StartHealthState.Confirmed) != 0;
            _driver.Set("Start check", health.Value == null ? health.Error : confirmed ? "Confirmed for this start" :
                "Not confirmed yet (confirmed automatically about a minute after logon)", confirmed ? Theme.Good : Theme.Warn);
            var vram = Kmd.VideoMemory();
            _driver.Set("Video memory", vram.Value == null ? vram.Error :
                KmdReply.Bytes(vram.Value.LocalResident) + " used of " + KmdReply.Bytes(vram.Value.Dedicated != 0 ? vram.Value.Dedicated : vram.Value.LocalLimit));
            var interop = Kmd.Interop();
            _driver.Set("Desktop composition", KmdReply.CompositionLine(DwmForceCpu(), interop.Value, interop.Error));

            string temp = KmdReply.TemperatureText(d);
            Color? tempColor = d.Has(DpmState.FlagTemperature) && d.TemperatureMc >= 87000 ? Theme.Accent : d.Has(DpmState.FlagTemperature) && d.TemperatureMc >= 80000 ? Theme.Warn : (Color?)null;
            _gpu.Set("Clock", KmdReply.ClockText(d));
            _gpu.Set("Voltage", d.CurrentMv != 0 ? d.CurrentMv + " mV" : "not available");
            _gpu.Set("Temperature", temp, tempColor);
            _gpu.Set("Load", d.Has(DpmState.FlagHwBusy) ? (d.BusyAvgPermille / 10.0).ToString("0") + " %" : "not available");
            _gpu.Set("Clock control", KmdReply.ModeText(d.Mode));
            _gpu.Set("Clock ceiling", d.Mode == 1 ? d.MaxMHz + " MHz" : "-");
            _gpu.Set("Limited by", KmdReply.ThrottleText(d.Throttle), d.Throttle >= 1 && d.Throttle <= 3 ? (Color?)Theme.Warn : null);

            _perfNow.Set("Running mode", KmdReply.ModeText(d.Mode));
            _perfNow.Set("Ceiling", d.Mode == 1 ? d.MaxMHz + " MHz" : "-");
            _perfNow.Set("Clock", KmdReply.ClockText(d));
            _perfNow.Set("Temperature", temp, tempColor);
            _perfNow.Set("Limited by", KmdReply.ThrottleText(d.Throttle));
            _perfNow.Set("Start decision", KmdReply.ReasonText(d.Reason), d.Reason >= 2 ? (Color?)Theme.Warn : null);
            SetStatus("Driver " + KmdReply.VersionText(d.Version) + " - updated " + DateTime.Now.ToString("HH:mm:ss"));
        }

        void SetStatus(string text) { _status.Text = text; }

        static uint? DwmForceCpu()
        {
            try { return SettingsStore.ReadDword(KmdReply.DesktopRouterPath, "DwmForceCpu"); }
            catch (Exception) { return null; }
        }

        // ---- Performance -------------------------------------------------------------------------------------------
        // Every settings page follows one rule (owner, 2026-10-03): a box is checked only when its value is written,
        // nothing is written for an unchecked box, unchecking removes the value, and nothing is written before Apply.
        // A driver default is described in text ("driver default: ..."), never shown as a checked box.

        void BuildPerformance()
        {
            var p = Page("Performance", "Performance", "GPU clock control. The driver reads these settings when it starts: restart Windows after a change.");
            _perfNow = new Card("Now (from the driver)");
            foreach (var k in new[] { "Running mode", "Ceiling", "Clock", "Temperature", "Limited by", "Start decision" }) _perfNow.Row(k);
            p.Controls.Add(_perfNow);

            var settings = new Card("Settings for the next start");
            _dpmOn = Theme.Check("Automatic clock: the driver changes the clock with the GPU load (DpmMode 1)", Theme.Bold);
            settings.Add(_dpmOn);
            settings.Add(Theme.Hint("Driver default when unchecked: fixed clock, 1000 MHz at all times (lowest heat, lowest performance)."));
            var ceilingRow = new FlowLayoutPanel { AutoSize = true, WrapContents = false, Margin = Theme.Pad(0) };
            _ceilingOn = Theme.Check("Clock ceiling (DpmMaxMHz):", Theme.Bold);
            ceilingRow.Controls.Add(_ceilingOn);
            _ceiling = new CeilingPicker();
            ceilingRow.Controls.Add(_ceiling);
            settings.Add(ceilingRow);
            settings.Add(Theme.Hint("Driver default when unchecked: " + DpmSettings.DefaultMaxMHz + " MHz. Used with the automatic clock only."));
            _perfStored = Theme.Label("", null, Theme.Dim);
            settings.Add(_perfStored);
            _perfDirty = Theme.Label("", null, Theme.Dim);
            settings.Add(_perfDirty);
            var buttons = new FlowLayoutPanel { AutoSize = true, Margin = Theme.Pad(0, 6, 0, 0) };
            _perfApply = Theme.Button("Apply", (s, e) => ApplyDpm(), true);
            _perfRevert = Theme.Button("Revert", (s, e) => RefreshStoredDpm());
            buttons.Controls.Add(_perfApply); buttons.Controls.Add(_perfRevert);
            settings.Add(buttons);
            _perfResult = Theme.Label("", null, Theme.Dim);
            settings.Add(_perfResult);
            p.Controls.Add(settings);
            _dpmOn.CheckedChanged += (s, e) => UpdatePerfDirty();
            _ceilingOn.CheckedChanged += (s, e) => UpdatePerfDirty();
            _ceiling.Stepped += (s, e) => { _ceilingOn.Checked = true; UpdatePerfDirty(); };

            var thermal = new Card("Temperature protection (always on, not adjustable)");
            thermal.Add(Theme.Label("At 87 C the driver reduces the clock step by step. Below 82 C it lets the clock rise again."));
            thermal.Add(Theme.Label("At 90 C the driver holds the lowest clock until the GPU is cooler."));
            thermal.Add(Theme.Label("Without a temperature reading the driver holds the lowest clock."));
            thermal.Add(Theme.Label("The board firmware controls the fan (BIOS \"Fan Setting\"), not this driver.", null, Theme.Dim));
            thermal.Add(Theme.Label("Safety: if Windows stops while the clock is high, or a start with automatic clock does not finish, the next start uses the fixed clock and sets the mode back to fixed. Select automatic again after you find the cause.", null, Theme.Dim));
            p.Controls.Add(thermal);
        }

        void RefreshStoredDpm()
        {
            uint? lastReason = null;
            try
            {
                _storedMode = SettingsStore.ReadDword(DpmSettings.RegistryPath, "DpmMode");
                _storedMax = SettingsStore.ReadDword(DpmSettings.RegistryPath, "DpmMaxMHz");
                lastReason = SettingsStore.ReadDword(DpmSettings.RegistryPath, "DpmLastReason");
            }
            catch (Exception e) { _perfStored.Text = "Cannot read the settings: " + e.Message; return; }
            using (var key = Microsoft.Win32.Registry.LocalMachine.OpenSubKey(DpmSettings.RegistryPath)) _driverInstalled = key != null;
            _loadingPerf = true;
            _dpmOn.Checked = _storedMode == 1;
            _ceilingOn.Checked = _storedMax != null;
            int index = _storedMax != null ? Array.IndexOf(DpmSettings.CeilingChoices, _storedMax.Value) : -1;
            _ceiling.Index = index >= 0 ? index : Array.IndexOf(DpmSettings.CeilingChoices, DpmSettings.DefaultMaxMHz);
            _loadingPerf = false;
            if (!_driverInstalled) _perfStored.Text = "The bc250kmd driver is not installed: there are no settings to change.";
            else _perfStored.Text = "Stored now: " + DpmSettings.Describe(_storedMode, _storedMax) +
                (_storedMax != null && index < 0 ? " (not on the 100 MHz grid: the driver uses the fixed clock)" : "") +
                (lastReason.HasValue ? ". Last start: " + KmdReply.ReasonText(lastReason.Value) + "." : ".");
            UpdatePerfDirty();
        }

        List<RegWrite> PerfWrites()
        {
            return DpmSettings.PlanWrites(_storedMode, _storedMax, _dpmOn.Checked, _ceilingOn.Checked ? _ceiling.Value : (uint?)null);
        }

        void UpdatePerfDirty()
        {
            if (_loadingPerf) return;
            var writes = PerfWrites();
            _perfDirty.Text = writes.Count == 0 ? "No unsaved changes." : "Unsaved changes (Apply writes them): " + string.Join("; ", writes.Select(DescribeWrite));
            _perfDirty.ForeColor = writes.Count == 0 ? Theme.Dim : Theme.Warn;
            _perfApply.Enabled = _driverInstalled && writes.Count > 0;
            _perfRevert.Enabled = writes.Count > 0;
        }

        static string DescribeWrite(RegWrite w) { return w.Delete ? "remove " + w.Name : w.Name + " = " + w.Number; }

        void ApplyDpm()
        {
            DoAction("set-clocks", _dpmOn.Checked ? 1u : (uint?)null, _ceilingOn.Checked ? _ceiling.Value : (uint?)null, _perfResult);
        }

        // ---- Applications ------------------------------------------------------------------------------------------

        void BuildApplications()
        {
            var p = Page("Applications", "Applications", "D3D12 settings for one game or program, found by its file name (for example witcher3.exe). The driver reads them when the program starts.");
            var warn = Theme.Label("Change these settings only when the release notes or the developers tell you to. Wrong settings can make a game fail to start.", null, Theme.Warn);
            p.Controls.Add(warn);
            var row = new FlowLayoutPanel { AutoSize = true, WrapContents = false, Margin = Theme.Pad(0, 8, 0, 0) };
            var left = new FlowLayoutPanel { FlowDirection = FlowDirection.TopDown, AutoSize = true, WrapContents = false };
            _profiles = new ListView
            {
                View = View.Details, FullRowSelect = true, MultiSelect = false, HideSelection = false, Width = Theme.S(260), Height = Theme.S(380),
                BackColor = Theme.Card, ForeColor = Theme.Text, BorderStyle = BorderStyle.None, HeaderStyle = ColumnHeaderStyle.Nonclickable, Font = Theme.Body,
            };
            _profiles.Columns.Add("Application", Theme.S(110)); _profiles.Columns.Add("Switches", Theme.S(145));
            Theme.DarkList(_profiles);
            _profiles.SelectedIndexChanged += (s, e) => ShowProfile();
            left.Controls.Add(_profiles);
            var lb = new FlowLayoutPanel { AutoSize = true };
            lb.Controls.Add(Theme.Button("Add application...", (s, e) => AddProfile()));
            lb.Controls.Add(Theme.Button("Uncheck all", (s, e) => ClearProfile()));
            left.Controls.Add(lb);
            row.Controls.Add(left);

            var right = new Card("Switches");
            _noSelection = Theme.Label("Select an application, or add one.", null, Theme.Dim);
            right.Add(_noSelection);
            right.Add(Theme.Narrow(Theme.Label("Unchecked switches are not written; the driver's built-in behaviour applies. Unchecking every switch removes the application's settings.", null, Theme.Dim), 500));
            _switches = new FlowLayoutPanel { FlowDirection = FlowDirection.TopDown, AutoSize = true, WrapContents = false, Margin = Theme.Pad(0) };
            foreach (var sw in Profiles.Catalog)
            {
                var box = Theme.Check(sw.Title + "  (" + sw.Token + ")", Theme.Bold);
                box.CheckedChanged += (s, e) => ProfileBoxChanged();
                _switches.Controls.Add(box);
                _switches.Controls.Add(Theme.Hint(sw.Description, 20, 500));
                _switchBoxes[sw.Token] = box;
            }
            _otherSwitches = new FlowLayoutPanel { FlowDirection = FlowDirection.TopDown, AutoSize = true, WrapContents = false, Margin = Theme.Pad(0) };
            _switches.Controls.Add(_otherSwitches);
            right.Add(_switches);
            _profilesDirty = Theme.Narrow(Theme.Label("", null, Theme.Dim), 500);
            right.Add(_profilesDirty);
            var buttons = new FlowLayoutPanel { AutoSize = true, Margin = Theme.Pad(0, 6, 0, 0) };
            _profilesApply = Theme.Button("Apply", (s, e) => ApplyProfiles(), true);
            _profilesRevert = Theme.Button("Revert", (s, e) => { _edits.Clear(); RefreshProfiles(); });
            buttons.Controls.Add(_profilesApply); buttons.Controls.Add(_profilesRevert);
            right.Add(buttons);
            _profileResult = Theme.Narrow(Theme.Label("", null, Theme.Dim), 500);
            right.Add(_profileResult);
            row.Controls.Add(right);
            p.Controls.Add(row);
        }

        // Every name checked for an image now: the unsaved edit, else the stored value.
        List<string> CheckedNames(string image)
        {
            List<string> edit;
            if (_edits.TryGetValue(image, out edit)) return edit;
            string stored;
            var v = Profiles.Parse(_storedProfiles.TryGetValue(image, out stored) ? stored : "");
            return v.Known.Concat(v.Unknown).ToList();
        }

        string StoredOrNull(string image) { string v; return _storedProfiles.TryGetValue(image, out v) ? v : null; }

        void RefreshProfiles()
        {
            if (_profiles == null) return;
            try { _storedProfiles = SettingsStore.ReadProfiles(); }
            catch (Exception e) { _profileResult.Text = "Cannot read the profiles: " + e.Message; }
            foreach (var image in _edits.Keys.ToList())
                if (StoredOrNull(image) != null && Profiles.PlanWrite(image, StoredOrNull(image), _edits[image]).Kind == ProfileWriteKind.None) _edits.Remove(image);
            string selected = _shownImage;
            _loadingProfile = true;
            _profiles.Items.Clear();
            foreach (var image in _storedProfiles.Keys.Union(_edits.Keys, StringComparer.OrdinalIgnoreCase).OrderBy(i => i, StringComparer.OrdinalIgnoreCase))
            {
                var names = CheckedNames(image);
                string shown = names.Count == 0 ? (StoredOrNull(image) != null ? "(removed on Apply)" : "(nothing checked)") : string.Join(",", names);
                var item = _profiles.Items.Add(new ListViewItem(new[] { image, (_edits.ContainsKey(image) ? "* " : "") + shown }));
                if (image.Equals(selected, StringComparison.OrdinalIgnoreCase)) item.Selected = true;
            }
            _loadingProfile = false;
            ShowProfile();
        }

        string SelectedImage { get { return _profiles.SelectedItems.Count > 0 ? _profiles.SelectedItems[0].Text : null; } }

        void ShowProfile()
        {
            if (_loadingProfile) return;
            _shownImage = SelectedImage;
            _noSelection.Visible = _shownImage == null;
            _switches.Visible = _shownImage != null;
            _loadingProfile = true;
            var names = _shownImage == null ? new List<string>() : CheckedNames(_shownImage);
            foreach (var kv in _switchBoxes) kv.Value.Checked = names.Contains(kv.Key);
            // Names outside the catalog (a newer driver's, or a developer's): a check box each, so they can be unchecked too.
            _otherSwitches.Controls.Clear();
            _otherBoxes.Clear();
            var stored = Profiles.Parse(_shownImage == null ? "" : StoredOrNull(_shownImage) ?? "");
            foreach (var other in stored.Unknown.Union(names.Where(n => Profiles.Find(n) == null)))
            {
                var box = Theme.Check("Other value: " + other, Theme.Bold);
                box.Tag = other;
                box.Checked = names.Contains(other);
                box.CheckedChanged += (s, e) => ProfileBoxChanged();
                _otherSwitches.Controls.Add(box);
                _otherSwitches.Controls.Add(Theme.Hint("Not in this app's list. The driver ignores names it does not know.", 20, 500));
                _otherBoxes.Add(box);
            }
            _loadingProfile = false;
            UpdateProfilesDirty();
        }

        void ProfileBoxChanged()
        {
            if (_loadingProfile || _shownImage == null) return;
            var names = Profiles.Catalog.Where(c => _switchBoxes[c.Token].Checked).Select(c => c.Token)
                .Concat(_otherBoxes.Where(b => b.Checked).Select(b => (string)b.Tag)).ToList();
            if (Profiles.PlanWrite(_shownImage, StoredOrNull(_shownImage), names).Kind == ProfileWriteKind.None && StoredOrNull(_shownImage) != null) _edits.Remove(_shownImage);
            else _edits[_shownImage] = names;
            foreach (ListViewItem item in _profiles.Items)
                if (item.Text.Equals(_shownImage, StringComparison.OrdinalIgnoreCase))
                    item.SubItems[1].Text = (_edits.ContainsKey(_shownImage) ? "* " : "") + (names.Count == 0 ? (StoredOrNull(_shownImage) != null ? "(removed on Apply)" : "(nothing checked)") : string.Join(",", names));
            UpdateProfilesDirty();
        }

        List<ProfileWrite> ProfileWrites(out string error)
        {
            error = null;
            var writes = new List<ProfileWrite>();
            foreach (var kv in _edits)
                try { var w = Profiles.PlanWrite(kv.Key, StoredOrNull(kv.Key), kv.Value); if (w.Kind != ProfileWriteKind.None) writes.Add(w); }
                catch (ArgumentException e) { error = kv.Key + ": " + e.Message; }
            return writes;
        }

        void UpdateProfilesDirty()
        {
            string error;
            var writes = ProfileWrites(out error);
            _profilesDirty.Text = error != null ? "Cannot save: " + error :
                writes.Count == 0 ? "No unsaved changes." :
                "Unsaved changes (Apply writes them): " + string.Join("; ", writes.Select(w => w.Kind == ProfileWriteKind.Delete ? "remove " + w.Image : w.Image + " = " + w.Value));
            _profilesDirty.ForeColor = writes.Count == 0 && error == null ? Theme.Dim : Theme.Warn;
            _profilesApply.Enabled = writes.Count > 0 && error == null;
            _profilesRevert.Enabled = _edits.Count > 0;
        }

        void AddProfile()
        {
            using (var dialog = new OpenFileDialog { Title = "Select the game's program file", Filter = "Programs (*.exe)|*.exe", CheckFileExists = true })
            {
                if (dialog.ShowDialog(this) != DialogResult.OK) return;
                var image = Path.GetFileName(dialog.FileName);
                if (!Profiles.IsValidImage(image)) { _profileResult.Text = "This file name cannot be used: " + image; return; }
                if (!_edits.ContainsKey(image) && StoredOrNull(image) == null) _edits[image] = new List<string>();
                _shownImage = image;
                RefreshProfiles();
                foreach (ListViewItem item in _profiles.Items) item.Selected = item.Text.Equals(image, StringComparison.OrdinalIgnoreCase);
                _profileResult.Text = "Check the switches for " + image + ", then Apply. Nothing is saved before Apply.";
            }
        }

        void ClearProfile()
        {
            if (_shownImage == null) return;
            _loadingProfile = true;
            foreach (var b in _switchBoxes.Values.Concat(_otherBoxes)) b.Checked = false;
            _loadingProfile = false;
            ProfileBoxChanged();
        }

        void ApplyProfiles()
        {
            string error;
            var writes = ProfileWrites(out error);
            if (error != null || writes.Count == 0) return;
            var verb = new List<string> { "--apply-profiles" };
            foreach (var w in writes) { verb.Add(w.Image); verb.Add(w.Kind == ProfileWriteKind.Delete ? "" : w.Value); }
            var failure = Program.RunElevated(verb.ToArray());
            _profileResult.Text = failure ?? "Saved. Start the application again to use the new settings.";
            _profileResult.ForeColor = failure == null ? Theme.Good : Theme.Warn;
            if (failure == null) _edits.Clear();
            RefreshProfiles();
        }

        // ---- Recovery ----------------------------------------------------------------------------------------------

        void BuildRecovery()
        {
            var p = Page("Recovery", "Recovery", "The driver's start and recovery states, and safe ways to change them. Each action asks for administrator permission and saves the old values first.");
            var states = new Card("States");
            _states = new FlowLayoutPanel { FlowDirection = FlowDirection.TopDown, AutoSize = true, WrapContents = false, Margin = Theme.Pad(0) };
            states.Add(_states);
            p.Controls.Add(states);

            var actions = new Card("Actions");
            Func<string, string, FlowLayoutPanel> row = (label, action) =>
            {
                var r = new FlowLayoutPanel { AutoSize = true, WrapContents = false, Margin = Theme.Pad(0) };
                var b = Theme.Button(label, (s, e) => DoAction(action));
                _actionButtons.Add(b);
                r.Controls.Add(b);
                return r;
            };
            actions.Add(Theme.Label("GPU desktop path (takes effect at the next restart):", null, Theme.Dim));
            actions.Add(row("Reopen the GPU desktop path", "reopen-gpu-path"));
            actions.Add(Theme.Label("Desktop composition (restarts the desktop now; the GPU route is watched for 60 seconds and falls back to the CPU route on a crash):", null, Theme.Dim));
            var route = row("Desktop on the CPU route", "desktop-cpu");
            var gpu = Theme.Button("Desktop on the GPU route", (s, e) => DoAction("desktop-gpu"));
            _actionButtons.Add(gpu);
            route.Controls.Add(gpu);
            actions.Add(route);
            actions.Add(Theme.Label("Driver start:", null, Theme.Dim));
            actions.Add(row("Confirm this start", "confirm-start"));
            actions.Add(Theme.Label("Clock control after a fallback to the fixed clock (takes effect at the next restart):", null, Theme.Dim));
            var dpm = new FlowLayoutPanel { AutoSize = true, WrapContents = false, Margin = Theme.Pad(0) };
            var enable = Theme.Button("Re-enable automatic clocks", (s, e) => DoAction("enable-dpm", null, RecoveryCeiling()));
            _actionButtons.Add(enable);
            dpm.Controls.Add(enable);
            _recoveryCeilingOn = Theme.Check("Also set the clock ceiling:");
            _recoveryCeilingOn.Margin = Theme.Pad(0, 10, 6, 0);
            dpm.Controls.Add(_recoveryCeilingOn);
            _recoveryCeiling = new CeilingPicker();
            _recoveryCeiling.Margin = Theme.Pad(0, 2, 0, 0);
            _recoveryCeiling.Stepped += (s, e) => _recoveryCeilingOn.Checked = true;
            dpm.Controls.Add(_recoveryCeiling);
            actions.Add(dpm);
            actions.Add(Theme.Hint("Unchecked: the stored ceiling stays as it is (driver default " + DpmSettings.DefaultMaxMHz + " MHz when none is stored).", 0));
            actions.Add(Theme.Label("All of the above (takes effect at the next restart):", null, Theme.Dim));
            actions.Add(row("Reset driver settings to the release defaults", "reset-defaults"));
            var undo = row("Undo last action", "undo");
            _undoText = Theme.Label("", null, Theme.Dim);
            undo.Controls.Add(_undoText);
            actions.Add(undo);
            actions.Add(Theme.Label("Not offered here: the temperature limits, firmware, BIOS settings and test signing.", null, Theme.Dim));
            _recoveryResult = Theme.Label("", null, Theme.Dim);
            actions.Add(_recoveryResult);
            _recoveryLog = Theme.DarkScroll(new TextBox
            {
                Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.None, WordWrap = true, Width = Theme.S(880), Height = Theme.S(120),
                BackColor = Theme.Nav, ForeColor = Theme.Text, Font = Theme.MonoSmall, BorderStyle = BorderStyle.None, Visible = false,
            });
            actions.Add(_recoveryLog);
            p.Controls.Add(actions);
            p.Controls.Add(Theme.Button("Refresh", (s, e) => RefreshRecovery()));
        }

        uint? RecoveryCeiling() { return _recoveryCeilingOn.Checked ? _recoveryCeiling.Value : (uint?)null; }

        void RefreshRecovery()
        {
            RecoverySnapshot snapshot;
            List<BackupRecord> backups;
            try { snapshot = RecoveryProbe.Read(); backups = RecoveryProbe.Backups(); }
            catch (Exception e) { _driver.Set("Recovery", "Cannot read the states: " + e.Message, Theme.Warn); return; }
            _stateLines = Recovery.Describe(snapshot);
            _states.Controls.Clear();
            foreach (var line in _stateLines)
            {
                var block = new FlowLayoutPanel { FlowDirection = FlowDirection.TopDown, AutoSize = true, WrapContents = false, Margin = Theme.Pad(0, 0, 0, 10) };
                block.Controls.Add(Theme.Label(line.Topic, Theme.Bold));
                var text = Theme.Label(line.Text, null, line.Severity == "warn" ? Theme.Warn : line.Severity == "ok" ? Theme.Good : Theme.Text);
                text.MaximumSize = Theme.Sz(880, 0);
                block.Controls.Add(text);
                if (line.Action != null)
                {
                    string action = line.Action;
                    block.Controls.Add(Theme.Button("Recommended: " + line.ActionLabel, (s, e) =>
                    {
                        if (action == "restart") OfferRestart("Restart Windows now?");
                        else DoAction(action, null, action == "enable-dpm" ? RecoveryCeiling() : null);
                    }));
                }
                _states.Controls.Add(block);
            }
            var target = Recovery.UndoTarget(backups);
            _undoText.Text = target == null ? "Nothing to undo." : "Restores the values before \"" + target.Action + "\" of " + target.Utc + ".";
            int warn = _stateLines.Count(l => l.Severity == "warn");
            _driver.Set("Recovery", warn == 0 ? "No action needed" : warn + (warn == 1 ? " item needs" : " items need") + " attention: see the Recovery page",
                warn == 0 ? (Color?)null : Theme.Warn);
        }

        // One planned action: the plan from this window's reading, then a refusal or a confirmation of exactly what
        // changes, then the elevated helper (which plans again from its own reading), its log and the restart offer.
        void DoAction(string action, uint? mode = null, uint? ceiling = null, Label result = null)
        {
            result = result ?? _recoveryResult;
            RecoverySnapshot snapshot;
            try { snapshot = RecoveryProbe.Read(); }
            catch (Exception e) { result.Text = "Cannot read the driver's state: " + e.Message; result.ForeColor = Theme.Warn; return; }
            var plan = Recovery.Plan(action, snapshot, mode, ceiling, RecoveryProbe.Backups());
            if (plan.Refused && result == _perfResult) { result.Text = plan.Refusal; result.ForeColor = Theme.Warn; return; }
            if (plan.Refused)
            {
                result.Text = plan.Refusal; result.ForeColor = Theme.Warn;
                MessageBox.Show(this, plan.Refusal, plan.Title ?? Program.ProductName, MessageBoxButtons.OK, MessageBoxIcon.Information);
                return;
            }
            var text = new StringBuilder();
            text.AppendLine(plan.Change).AppendLine().AppendLine("Takes effect: " + plan.Effect + ".");
            foreach (var n in plan.Notes) text.AppendLine().AppendLine(n);
            text.AppendLine().AppendLine("Changes:");
            foreach (var w in plan.Writes) text.AppendLine("  " + w);
            if (plan.ConfirmStart) text.AppendLine("  the driver's start confirmation");
            text.AppendLine().AppendLine(plan.Undoable ? "The old values are saved first; \"Undo last action\" restores them." : "This action cannot be undone.");
            text.AppendLine().Append("Windows asks for administrator permission next. Continue?");
            if (MessageBox.Show(this, text.ToString(), plan.Title, MessageBoxButtons.OKCancel, MessageBoxIcon.Question) != DialogResult.OK) return;

            string runId = Guid.NewGuid().ToString("N").Substring(0, 12);
            var verb = new List<string> { "--action", action, "--run-id", runId };
            // set-clocks names both values: a number, or "unset" for an unchecked box (the value is removed).
            if (mode != null || action == "set-clocks") { verb.Add("--mode"); verb.Add(mode == null ? "unset" : mode.Value.ToString(System.Globalization.CultureInfo.InvariantCulture)); }
            if (ceiling != null || action == "set-clocks") { verb.Add("--ceiling"); verb.Add(ceiling == null ? "unset" : ceiling.Value.ToString(System.Globalization.CultureInfo.InvariantCulture)); }
            foreach (var b in _actionButtons) b.Enabled = false;
            _perfApply.Enabled = false;
            result.Text = plan.RestartDwm ? "Restarting the desktop" + (plan.WatchDwm ? " and watching it for 60 seconds" : "") + "..." : "Working...";
            result.ForeColor = Theme.Dim;
            Task.Run(() => Program.RunElevatedCode(verb.ToArray())).ContinueWith(t =>
            {
                foreach (var b in _actionButtons) b.Enabled = true;
                int code = t.Status == TaskStatus.RanToCompletion ? t.Result : RecoveryRunner.Failed;
                var lines = RecoveryProbe.LogLines(runId);
                var last = lines.LastOrDefault(l => l.Contains(" result: "));
                result.Text = code == Program.NotElevated ? "Administrator permission was not given. Nothing was changed." :
                    last != null ? last.Substring(last.IndexOf(" result: ", StringComparison.Ordinal) + 9) : "Finished with exit code " + code + ".";
                result.ForeColor = code == RecoveryRunner.Done ? Theme.Good : Theme.Warn;
                ShowLog(string.Join("\r\n", lines));
                RefreshAll();
                if (code == RecoveryRunner.Done && plan.OfferRestart) OfferRestart("The change takes effect at the next restart of Windows. Restart now?");
            }, TaskScheduler.FromCurrentSynchronizationContext());
        }

        // The log of the last action: hidden while empty, wrapped, and a scroll bar only when the text is taller than
        // the box.
        void ShowLog(string text)
        {
            _recoveryLog.Text = text;
            _recoveryLog.Visible = text.Length > 0;
            var room = new Size(_recoveryLog.Width - SystemInformation.VerticalScrollBarWidth - Theme.S(4), int.MaxValue);
            int need = TextRenderer.MeasureText(text, _recoveryLog.Font, room, TextFormatFlags.WordBreak | TextFormatFlags.TextBoxControl).Height;
            _recoveryLog.ScrollBars = need > _recoveryLog.Height ? ScrollBars.Vertical : ScrollBars.None;
        }

        void OfferRestart(string question)
        {
            if (MessageBox.Show(this, question + "\n\nSave your work in other programs first.", Program.ProductName, MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes) return;
            try { Process.Start(new ProcessStartInfo("shutdown.exe", "/r /t 0") { UseShellExecute = false, CreateNoWindow = true }); }
            catch (Exception e) { MessageBox.Show(this, "Windows did not restart: " + e.Message, Program.ProductName); }
        }

        // ---- Diagnostics -------------------------------------------------------------------------------------------

        void BuildDiagnostics()
        {
            var p = Page("Diagnostics", "Diagnostics", "Create a bug report for the developers. The report stays on this PC until you send it.");
            var card = new Card("Bug report");
            card.Add(Theme.Label("The report is a zip file on your Desktop. It contains:"));
            foreach (var line in new[]
            {
                "- the driver log and the driver state (clocks, temperature, settings)",
                "- the installed driver files with versions and checksums",
                "- DirectX diagnostics (dxdiag) and the D3D12 and Vulkan capability checks",
                "- Windows events of the last 24 hours from the display drivers",
            }) card.Add(Theme.Label(line, null, Theme.Dim));
            card.Add(Theme.Label("Your user name, computer name, MAC addresses and serial numbers are removed. You see the file list before the report is saved.", null, Theme.Dim));
            _withDxdiag = new CheckBox { Text = "Include dxdiag (takes up to 2 minutes)", Checked = true, AutoSize = true, ForeColor = Theme.Text };
            _withCaps = new CheckBox { Text = "Include the D3D12 and Vulkan capability checks", Checked = true, AutoSize = true, ForeColor = Theme.Text };
            card.Add(_withDxdiag); card.Add(_withCaps);
            _reportButton = Theme.Button("Create bug report", (s, e) => CreateReport(), true);
            card.Add(_reportButton);
            _reportStatus = Theme.Label("", null, Theme.Dim);
            card.Add(_reportStatus);
            p.Controls.Add(card);
        }

        void CreateReport()
        {
            _reportButton.Enabled = false;
            var report = new BugReport(Redactor.ForThisPc());
            bool dxdiag = _withDxdiag.Checked, caps = _withCaps.Checked;
            Action<string> progress = text => { try { BeginInvoke((Action)(() => _reportStatus.Text = text + "...")); } catch (InvalidOperationException) { } };
            Task.Run(() => report.Collect(dxdiag, caps, true, progress)).ContinueWith(t =>
            {
                _reportButton.Enabled = true;
                if (t.IsFaulted) { _reportStatus.Text = "The report failed: " + t.Exception.GetBaseException().Message; return; }
                var path = BugReport.DefaultPath(DateTime.Now);
                using (var preview = new ReportPreview(report, path))
                {
                    if (preview.ShowDialog(this) != DialogResult.OK) { _reportStatus.Text = "Cancelled. Nothing was saved."; return; }
                }
                try
                {
                    report.Write(path);
                    _reportStatus.Text = "Saved: " + path;
                    _reportStatus.ForeColor = Theme.Good;
                }
                catch (Exception e) { _reportStatus.Text = "Cannot save the report: " + e.Message; _reportStatus.ForeColor = Theme.Warn; }
            }, TaskScheduler.FromCurrentSynchronizationContext());
        }

        // ---- smoke ---------------------------------------------------------------------------------------------------

        // --smoke-render: every page drawn to a PNG without showing the window, each tall enough for all its content, and
        // the layout checked for sibling controls that overlap. Applications shows an unsaved demo profile; nothing is
        // written.
        public string RenderPages(string dir)
        {
            var w = new StringBuilder();
            _edits["demo.exe"] = new List<string> { "raytracing-tier", "deferred-replay", "x-future-switch" };
            _shownImage = "demo.exe";
            foreach (var page in _pages.Keys.ToList())
            {
                ShowPage(page);
                if (page == "Applications")
                    foreach (ListViewItem i in _profiles.Items) i.Selected = i.Text == "demo.exe";
                if (page == "Recovery") ShowLog("(render) The log of the last action appears here, wrapped to the width of the box; a scroll bar appears only when the text is taller than the box.");
                ClientSize = Theme.Sz(1180, 720);
                PerformLayout();
                CreateHandles(this);
                Snap(Path.Combine(dir, page + ".png"));
                // The last control of the page must lie inside the content panel's scroll range, and a page taller
                // than the panel needs its scroll bar. A form that is never shown does not scroll, so the check reads
                // the range, and <page>-full.png draws the whole page.
                var last = LastShown(_pages[page]);
                int bottom = _content.PointToClient(last.Parent.PointToScreen(last.Location)).Y + last.Height;
                int extent = _content.DisplayRectangle.Bottom + _content.Padding.Bottom;
                if (bottom > extent || bottom > _content.ClientSize.Height && !_content.VerticalScroll.Visible)
                    w.AppendLine(page + ": the last control " + Describe(last) + " ends at " + bottom + ", outside the scroll range " + extent +
                        " (panel " + _content.ClientSize.Height + ", scroll bar " + (_content.VerticalScroll.Visible ? "shown" : "hidden") + ")");
                var full = _pages[page];
                using (var bmp = new Bitmap(full.Width, full.Height))
                {
                    full.DrawToBitmap(bmp, new Rectangle(0, 0, full.Width, full.Height));
                    bmp.Save(Path.Combine(dir, page + "-full.png"), System.Drawing.Imaging.ImageFormat.Png);
                }
                foreach (var o in Overlaps(_pages[page])) w.AppendLine(page + ": " + o);
                int room = Theme.S(1180) - Theme.S(200) - _content.Padding.Horizontal;
                if (_pages[page].PreferredSize.Width > room) w.AppendLine(page + ": " + _pages[page].PreferredSize.Width + " px wide, the window has " + room);
            }
            _edits.Remove("demo.exe");
            ShowLog("");
            return w.ToString();
        }

        void Snap(string path)
        {
            using (var bmp = new Bitmap(Width, Height))
            {
                DrawToBitmap(bmp, new Rectangle(0, 0, Width, Height));
                bmp.Save(path, System.Drawing.Imaging.ImageFormat.Png);
            }
        }

        // The lowest control on the page that has no shown children of its own.
        static Control LastShown(Control c)
        {
            var kids = c.Controls.Cast<Control>().Where(HasVisibleFlag).ToList();
            if (kids.Count == 0) return c;
            return LastShown(kids.OrderBy(k => k.Bottom).Last());
        }

        // Control.Visible reads false for every control of a form that was never shown, so the check uses the
        // control's own visible flag (STATE_VISIBLE).
        static readonly System.Reflection.MethodInfo GetState = typeof(Control).GetMethod("GetState", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance);
        static bool HasVisibleFlag(Control c) { return (bool)GetState.Invoke(c, new object[] { 2 }); }

        // A form that is never shown creates no child windows by itself, and DrawToBitmap draws only windows.
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

        public string Describe()
        {
            var w = new StringBuilder();
            w.AppendLine(Program.ProductName + " " + Program.VersionText);
            foreach (var card in new[] { _driver, _gpu, _perfNow })
                foreach (var kv in card.Values) w.AppendLine(kv.Key + ": " + kv.Value.Text);
            w.AppendLine("stored dpm: " + _perfStored.Text);
            w.AppendLine("performance boxes: automatic " + _dpmOn.Checked + ", ceiling " + _ceilingOn.Checked + "; " + _perfDirty.Text);
            w.AppendLine("components: " + _components.Items.Count);
            foreach (ListViewItem i in _components.Items) w.AppendLine("  " + string.Join(" | ", i.SubItems.Cast<ListViewItem.ListViewSubItem>().Select(s => s.Text)));
            w.AppendLine("profiles: " + _profiles.Items.Count + "; " + _profilesDirty.Text);
            foreach (ListViewItem i in _profiles.Items) w.AppendLine("  " + i.Text + " | " + i.SubItems[1].Text);
            w.AppendLine("recovery: " + _stateLines.Count + " states");
            foreach (var l in _stateLines) w.AppendLine("  [" + l.Severity + "] " + l.Topic + ": " + l.Text + (l.Action != null ? " -> " + l.Action : ""));
            w.AppendLine("undo: " + _undoText.Text);
            w.AppendLine("status: " + _status.Text);
            return w.ToString();
        }

        protected override void OnFormClosed(FormClosedEventArgs e) { _timer.Stop(); base.OnFormClosed(e); }
    }

    // The file list of a report before anything is written, with a look at each file's text.
    sealed class ReportPreview : Form
    {
        public ReportPreview(BugReport report, string path)
        {
            Text = "Bug report - files"; BackColor = Theme.Back; ForeColor = Theme.Text; Font = Theme.Body;
            ClientSize = Theme.Sz(900, 600); StartPosition = FormStartPosition.CenterParent; MinimizeBox = false;
            var top = new Panel { Dock = DockStyle.Top, Height = Theme.S(54), Padding = Theme.Pad(12, 8, 12, 0) };
            top.Controls.Add(Theme.Label("These files will be saved in:\n" + path));
            var list = new ListView
            {
                Dock = DockStyle.Top, Height = Theme.S(200), View = View.Details, FullRowSelect = true, MultiSelect = false, HideSelection = false,
                BackColor = Theme.Card, ForeColor = Theme.Text, BorderStyle = BorderStyle.None,
            };
            Theme.DarkList(list);
            list.Columns.Add("File", Theme.S(170)); list.Columns.Add("Size", Theme.S(90)); list.Columns.Add("Contents", Theme.S(600));
            foreach (var e in report.Entries) list.Items.Add(new ListViewItem(new[] { e.Name, (e.Data.Length / 1024.0).ToString("0.0", System.Globalization.CultureInfo.InvariantCulture) + " KB", e.Description }) { Tag = e });
            var text = Theme.DarkScroll(new TextBox
            {
                Dock = DockStyle.Fill, Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Both, WordWrap = false,
                BackColor = Theme.Nav, ForeColor = Theme.Text, Font = Theme.Mono, BorderStyle = BorderStyle.None,
            });
            list.SelectedIndexChanged += (s, e) =>
            {
                if (list.SelectedItems.Count == 0) return;
                var entry = (ReportEntry)list.SelectedItems[0].Tag;
                var content = Encoding.UTF8.GetString(entry.Data);
                text.Text = content.Length > 200000 ? content.Substring(0, 200000) + "\r\n[... shortened in this view only ...]" : content.Replace("\r\n", "\n").Replace("\n", "\r\n");
            };
            var bottom = new FlowLayoutPanel { Dock = DockStyle.Bottom, Height = Theme.S(48), FlowDirection = FlowDirection.RightToLeft, Padding = Theme.Pad(8) };
            var save = Theme.Button("Save to Desktop", (s, e) => { DialogResult = DialogResult.OK; Close(); }, true);
            var cancel = Theme.Button("Cancel", (s, e) => { DialogResult = DialogResult.Cancel; Close(); });
            bottom.Controls.Add(save); bottom.Controls.Add(cancel);
            Controls.Add(text); Controls.Add(list); Controls.Add(top); Controls.Add(bottom);
            AcceptButton = save; CancelButton = cancel;
            if (list.Items.Count > 0) list.Items[0].Selected = true;
        }
    }
}
