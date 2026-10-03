// The window: a navigation bar on the left and four pages (Overview, Performance, Applications, Diagnostics).
// Live values come from the KMD's software snapshots every 2 s, and only while Overview or Performance is shown and
// the window is not minimized. Settings pages write the registry through Program.RunElevated.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace AmdgpuWddmControl
{
    static class Theme
    {
        public static readonly Color Back = Color.FromArgb(24, 24, 27), Nav = Color.FromArgb(16, 16, 18), Card = Color.FromArgb(36, 36, 40),
            Text = Color.FromArgb(232, 232, 236), Dim = Color.FromArgb(160, 160, 170), Accent = Color.FromArgb(237, 28, 36),
            Good = Color.FromArgb(90, 200, 120), Warn = Color.FromArgb(240, 180, 60), Line = Color.FromArgb(60, 60, 66);
        public static readonly Font Body = new Font("Segoe UI", 9.5f), Bold = new Font("Segoe UI Semibold", 9.5f),
            Title = new Font("Segoe UI Semibold", 15f), CardTitle = new Font("Segoe UI Semibold", 11f), Brand = new Font("Segoe UI Semibold", 13f);

        public static Button Button(string text, EventHandler click, bool primary = false)
        {
            var b = new Button
            {
                Text = text, AutoSize = true, FlatStyle = FlatStyle.Flat, ForeColor = Text, Font = Bold,
                BackColor = primary ? Accent : Card, Padding = new Padding(10, 4, 10, 4), Margin = new Padding(0, 6, 10, 6), Cursor = Cursors.Hand,
            };
            b.FlatAppearance.BorderColor = primary ? Accent : Line;
            b.Click += click;
            return b;
        }

        public static Label Label(string text, Font font = null, Color? color = null)
        {
            return new Label { Text = text, AutoSize = true, Font = font ?? Body, ForeColor = color ?? Text, Margin = new Padding(0, 3, 12, 3), MaximumSize = new Size(860, 0) };
        }
    }

    // A titled block with key/value rows; values are updated by key.
    sealed class Card : Panel
    {
        readonly TableLayoutPanel _rows;
        public readonly Dictionary<string, Label> Values = new Dictionary<string, Label>();

        public Card(string title)
        {
            BackColor = Theme.Card; AutoSize = true; AutoSizeMode = AutoSizeMode.GrowAndShrink; Padding = new Padding(14, 10, 14, 12);
            Margin = new Padding(0, 0, 14, 14);
            var stack = new FlowLayoutPanel { FlowDirection = FlowDirection.TopDown, AutoSize = true, WrapContents = false, Dock = DockStyle.Fill };
            stack.Controls.Add(Theme.Label(title, Theme.CardTitle));
            _rows = new TableLayoutPanel { ColumnCount = 2, AutoSize = true, Margin = new Padding(0, 6, 0, 0) };
            _rows.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 190));
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
        readonly Panel _content = new Panel { Dock = DockStyle.Fill, BackColor = Theme.Back, Padding = new Padding(24, 18, 24, 18), AutoScroll = true };
        readonly Dictionary<string, Panel> _pages = new Dictionary<string, Panel>();
        readonly Dictionary<string, Button> _nav = new Dictionary<string, Button>();
        readonly Label _status = Theme.Label("", null, Theme.Dim);
        readonly Timer _timer = new Timer { Interval = 2000 };
        string _page;

        Card _driver, _gpu, _perfNow;
        ListView _components;
        RadioButton _dpmOn, _dpmOff;
        ComboBox _ceiling;
        Label _perfStored, _perfResult;
        ListView _profiles;
        FlowLayoutPanel _switches;
        Label _unknown, _profileResult, _reportStatus;
        readonly Dictionary<string, CheckBox> _switchBoxes = new Dictionary<string, CheckBox>();
        List<string> _unknownTokens = new List<string>();
        CheckBox _withDxdiag, _withCaps;
        Button _reportButton, _perfApply;
        InventoryState _inventory;

        public MainForm() : this(false) { }

        public MainForm(bool smoke)
        {
            _smoke = smoke;
            Text = Program.ProductName;
            BackColor = Theme.Back; ForeColor = Theme.Text; Font = Theme.Body;
            ClientSize = new Size(1080, 720); MinimumSize = new Size(820, 560); StartPosition = FormStartPosition.CenterScreen;
            try { Icon = Icon.ExtractAssociatedIcon(System.Reflection.Assembly.GetExecutingAssembly().Location); } catch (Exception) { }

            var nav = new FlowLayoutPanel { Dock = DockStyle.Left, Width = 200, BackColor = Theme.Nav, FlowDirection = FlowDirection.TopDown, WrapContents = false, Padding = new Padding(0, 16, 0, 0) };
            var brand = Theme.Label("amdgpu-wddm", Theme.Brand); brand.Margin = new Padding(18, 0, 0, 0);
            var sub = Theme.Label("ASRock BC-250", null, Theme.Dim); sub.Margin = new Padding(18, 0, 0, 18);
            var bar = new Panel { Width = 200, Height = 3, BackColor = Theme.Accent, Margin = new Padding(0, 0, 0, 14) };
            nav.Controls.Add(brand); nav.Controls.Add(sub); nav.Controls.Add(bar);
            foreach (var name in new[] { "Overview", "Performance", "Applications", "Diagnostics" })
            {
                var b = new Button
                {
                    Text = "   " + name, Width = 200, Height = 42, FlatStyle = FlatStyle.Flat, TextAlign = ContentAlignment.MiddleLeft,
                    ForeColor = Theme.Text, BackColor = Theme.Nav, Font = Theme.Bold, Margin = new Padding(0), Cursor = Cursors.Hand,
                };
                b.FlatAppearance.BorderSize = 0;
                string page = name;
                b.Click += (s, e) => ShowPage(page);
                nav.Controls.Add(b);
                _nav[name] = b;
            }
            var footer = new Panel { Dock = DockStyle.Bottom, Height = 28, BackColor = Theme.Nav, Padding = new Padding(12, 5, 12, 0) };
            footer.Controls.Add(_status);

            BuildOverview(); BuildPerformance(); BuildApplications(); BuildDiagnostics();
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
        }

        Panel Page(string name, string title, string intro)
        {
            var p = new FlowLayoutPanel { FlowDirection = FlowDirection.TopDown, WrapContents = false, AutoSize = true, BackColor = Theme.Back };
            p.Controls.Add(Theme.Label(title, Theme.Title));
            var i = Theme.Label(intro, null, Theme.Dim); i.Margin = new Padding(0, 2, 0, 14);
            p.Controls.Add(i);
            _pages[name] = p;
            return p;
        }

        // ---- Overview ----------------------------------------------------------------------------------------------

        void BuildOverview()
        {
            var p = Page("Overview", "Overview", "The driver and the GPU now. Values update every 2 seconds.");
            var row = new FlowLayoutPanel { AutoSize = true, WrapContents = true, MaximumSize = new Size(1000, 0) };
            _driver = new Card("Driver");
            foreach (var k in new[] { "Status", "Release", "GPU", "Kernel driver", "Start check", "Driver package", "Driver date", "Video memory", "Test signing", "Desktop composition" }) _driver.Row(k);
            _gpu = new Card("GPU now");
            foreach (var k in new[] { "Clock", "Voltage", "Temperature", "Load", "Clock control", "Clock ceiling", "Limited by" }) _gpu.Row(k);
            row.Controls.Add(_driver); row.Controls.Add(_gpu);
            p.Controls.Add(row);
            p.Controls.Add(Theme.Label("Installed components", Theme.CardTitle));
            _components = new ListView
            {
                View = View.Details, FullRowSelect = true, HeaderStyle = ColumnHeaderStyle.Nonclickable, Width = 960, Height = 210,
                BackColor = Theme.Card, ForeColor = Theme.Text, BorderStyle = BorderStyle.None, Margin = new Padding(0, 6, 0, 6),
            };
            _components.Columns.Add("Component", 200); _components.Columns.Add("File", 430); _components.Columns.Add("Version", 140); _components.Columns.Add("SHA-256", 170);
            p.Controls.Add(_components);
            p.Controls.Add(Theme.Button("Refresh", (s, e) => RefreshAll()));
        }

        public void RefreshAll()
        {
            _inventory = Inventory.Read(false);
            FillComponents();
            RefreshLive();
            RefreshStoredDpm();
            RefreshProfiles();
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

        void BuildPerformance()
        {
            var p = Page("Performance", "Performance", "GPU clock control. The driver reads these settings when it starts: restart Windows after a change.");
            _perfNow = new Card("Now (from the driver)");
            foreach (var k in new[] { "Running mode", "Ceiling", "Clock", "Temperature", "Limited by", "Start decision" }) _perfNow.Row(k);
            p.Controls.Add(_perfNow);

            var settings = new Card("Settings for the next start");
            _dpmOn = new RadioButton { Text = "Automatic clock (recommended): the driver changes the clock with the GPU load", AutoSize = true, ForeColor = Theme.Text, Margin = new Padding(0, 4, 0, 2) };
            _dpmOff = new RadioButton { Text = "Fixed clock: 1000 MHz at all times (lowest heat, lowest performance)", AutoSize = true, ForeColor = Theme.Text, Margin = new Padding(0, 2, 0, 8) };
            settings.Add(_dpmOn); settings.Add(_dpmOff);
            var ceilingRow = new FlowLayoutPanel { AutoSize = true, Margin = new Padding(0) };
            ceilingRow.Controls.Add(Theme.Label("Clock ceiling (automatic mode):"));
            _ceiling = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 120, BackColor = Theme.Card, ForeColor = Theme.Text, FlatStyle = FlatStyle.Flat };
            foreach (var mhz in DpmSettings.CeilingChoices) _ceiling.Items.Add(mhz + " MHz" + (mhz == DpmSettings.DefaultMaxMHz ? " (default)" : ""));
            ceilingRow.Controls.Add(_ceiling);
            settings.Add(ceilingRow);
            _dpmOn.CheckedChanged += (s, e) => _ceiling.Enabled = _dpmOn.Checked;
            _perfStored = Theme.Label("", null, Theme.Dim);
            settings.Add(_perfStored);
            var buttons = new FlowLayoutPanel { AutoSize = true, Margin = new Padding(0, 6, 0, 0) };
            _perfApply = Theme.Button("Apply", (s, e) => ApplyDpm(), true);
            buttons.Controls.Add(_perfApply);
            buttons.Controls.Add(Theme.Button("Reset to defaults", (s, e) => { _dpmOn.Checked = true; _ceiling.SelectedIndex = Array.IndexOf(DpmSettings.CeilingChoices, DpmSettings.DefaultMaxMHz); }));
            settings.Add(buttons);
            _perfResult = Theme.Label("", null, Theme.Dim);
            settings.Add(_perfResult);
            p.Controls.Add(settings);

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
            uint? mode = null, max = null, lastReason = null;
            try
            {
                mode = SettingsStore.ReadDword(DpmSettings.RegistryPath, "DpmMode");
                max = SettingsStore.ReadDword(DpmSettings.RegistryPath, "DpmMaxMHz");
                lastReason = SettingsStore.ReadDword(DpmSettings.RegistryPath, "DpmLastReason");
            }
            catch (Exception e) { _perfStored.Text = "Cannot read the settings: " + e.Message; return; }
            bool installed;
            using (var key = Microsoft.Win32.Registry.LocalMachine.OpenSubKey(DpmSettings.RegistryPath)) installed = key != null;
            _dpmOn.Checked = mode == 1; _dpmOff.Checked = mode != 1;
            int index = Array.IndexOf(DpmSettings.CeilingChoices, max ?? DpmSettings.DefaultMaxMHz);
            _ceiling.SelectedIndex = index >= 0 ? index : Array.IndexOf(DpmSettings.CeilingChoices, DpmSettings.DefaultMaxMHz);
            _ceiling.Enabled = _dpmOn.Checked;
            _perfApply.Enabled = installed;
            if (!installed) { _perfStored.Text = "The bc250kmd driver is not installed: there are no settings to change."; return; }
            _perfStored.Text = "Stored now: " + DpmSettings.Describe(mode, max) +
                (lastReason.HasValue ? ". Last start: " + KmdReply.ReasonText(lastReason.Value) + "." : ".");
        }

        void ApplyDpm()
        {
            uint mode = _dpmOn.Checked ? 1u : 0u;
            uint max = DpmSettings.CeilingChoices[Math.Max(0, _ceiling.SelectedIndex)];
            if (mode == 1 && max > DpmSettings.DefaultMaxMHz &&
                MessageBox.Show(this, "A ceiling above " + DpmSettings.DefaultMaxMHz + " MHz makes the GPU hotter and uses more power. Make sure the case has good air flow.\n\nApply " + max + " MHz?",
                    Program.ProductName, MessageBoxButtons.OKCancel, MessageBoxIcon.Warning) != DialogResult.OK) return;
            var error = Program.RunElevated("--write-dpm", mode.ToString(), max.ToString());
            _perfResult.Text = error ?? "Saved. Restart Windows to use the new setting.";
            _perfResult.ForeColor = error == null ? Theme.Good : Theme.Warn;
            RefreshStoredDpm();
        }

        // ---- Applications ------------------------------------------------------------------------------------------

        void BuildApplications()
        {
            var p = Page("Applications", "Applications", "D3D12 settings for one game or program, found by its file name (for example witcher3.exe). The driver reads them when the program starts.");
            var warn = Theme.Label("Change these settings only when the release notes or the developers tell you to. Wrong settings can make a game fail to start.", null, Theme.Warn);
            p.Controls.Add(warn);
            var row = new FlowLayoutPanel { AutoSize = true, WrapContents = false, Margin = new Padding(0, 8, 0, 0) };
            var left = new FlowLayoutPanel { FlowDirection = FlowDirection.TopDown, AutoSize = true, WrapContents = false };
            _profiles = new ListView
            {
                View = View.Details, FullRowSelect = true, MultiSelect = false, HideSelection = false, Width = 330, Height = 380,
                BackColor = Theme.Card, ForeColor = Theme.Text, BorderStyle = BorderStyle.None, HeaderStyle = ColumnHeaderStyle.Nonclickable,
            };
            _profiles.Columns.Add("Application", 150); _profiles.Columns.Add("Switches", 175);
            _profiles.SelectedIndexChanged += (s, e) => ShowProfile();
            left.Controls.Add(_profiles);
            var lb = new FlowLayoutPanel { AutoSize = true };
            lb.Controls.Add(Theme.Button("Add application...", (s, e) => AddProfile()));
            lb.Controls.Add(Theme.Button("Remove", (s, e) => RemoveProfile()));
            left.Controls.Add(lb);
            row.Controls.Add(left);

            var right = new Card("Switches");
            _switches = new FlowLayoutPanel { FlowDirection = FlowDirection.TopDown, AutoSize = true, WrapContents = false, Margin = new Padding(0) };
            foreach (var sw in Profiles.Catalog)
            {
                var box = new CheckBox { Text = sw.Title + "  (" + sw.Token + ")", AutoSize = true, ForeColor = Theme.Text, Font = Theme.Bold, Margin = new Padding(0, 6, 0, 0) };
                var desc = Theme.Label(sw.Description, null, Theme.Dim); desc.MaximumSize = new Size(560, 0); desc.Margin = new Padding(18, 0, 0, 2);
                _switches.Controls.Add(box); _switches.Controls.Add(desc);
                _switchBoxes[sw.Token] = box;
            }
            right.Add(_switches);
            _unknown = Theme.Label("", null, Theme.Dim); _unknown.MaximumSize = new Size(560, 0);
            right.Add(_unknown);
            right.Add(Theme.Button("Save", (s, e) => SaveProfile(), true));
            _profileResult = Theme.Label("", null, Theme.Dim);
            right.Add(_profileResult);
            row.Controls.Add(right);
            p.Controls.Add(row);
            SetSwitchesEnabled(false);
        }

        void SetSwitchesEnabled(bool on) { foreach (var b in _switchBoxes.Values) b.Enabled = on; }

        void RefreshProfiles()
        {
            if (_profiles == null) return;
            string selected = _profiles.SelectedItems.Count > 0 ? _profiles.SelectedItems[0].Text : null;
            _profiles.Items.Clear();
            try
            {
                foreach (var kv in SettingsStore.ReadProfiles())
                {
                    var item = _profiles.Items.Add(new ListViewItem(new[] { kv.Key, kv.Value.Length == 0 ? "(none)" : kv.Value }));
                    if (kv.Key.Equals(selected, StringComparison.OrdinalIgnoreCase)) item.Selected = true;
                }
            }
            catch (Exception e) { _profileResult.Text = "Cannot read the profiles: " + e.Message; }
            if (_profiles.SelectedItems.Count == 0) ShowProfile();
        }

        string SelectedImage { get { return _profiles.SelectedItems.Count > 0 ? _profiles.SelectedItems[0].Text : null; } }

        void ShowProfile()
        {
            var image = SelectedImage;
            string value = "";
            if (image != null) { string v; if (SettingsStore.ReadProfiles().TryGetValue(image, out v)) value = v; }
            var parsed = Profiles.Parse(value);
            foreach (var kv in _switchBoxes) kv.Value.Checked = parsed.Known.Contains(kv.Key);
            _unknownTokens = parsed.Unknown;
            _unknown.Text = parsed.Unknown.Count == 0 ? "" : "Other values (kept as they are, not editable here): " + string.Join(", ", parsed.Unknown);
            SetSwitchesEnabled(image != null);
            _profileResult.Text = image == null ? "Select an application, or add one." : "";
        }

        void AddProfile()
        {
            using (var dialog = new OpenFileDialog { Title = "Select the game's program file", Filter = "Programs (*.exe)|*.exe", CheckFileExists = true })
            {
                if (dialog.ShowDialog(this) != DialogResult.OK) return;
                var image = Path.GetFileName(dialog.FileName);
                if (!Profiles.IsValidImage(image)) { _profileResult.Text = "This file name cannot be used: " + image; return; }
                var error = Program.RunElevated("--write-profile", image, "");
                _profileResult.Text = error ?? "Added " + image + ". Select switches and click Save.";
                RefreshProfiles();
                foreach (ListViewItem item in _profiles.Items) item.Selected = item.Text.Equals(image, StringComparison.OrdinalIgnoreCase);
            }
        }

        void RemoveProfile()
        {
            var image = SelectedImage;
            if (image == null) return;
            if (MessageBox.Show(this, "Remove all settings for " + image + "?", Program.ProductName, MessageBoxButtons.OKCancel) != DialogResult.OK) return;
            var error = Program.RunElevated("--remove-profile", image);
            _profileResult.Text = error ?? "Removed " + image + ".";
            RefreshProfiles();
        }

        void SaveProfile()
        {
            var image = SelectedImage;
            if (image == null) return;
            string value;
            try { value = Profiles.Compose(_switchBoxes.Where(kv => kv.Value.Checked).Select(kv => kv.Key), _unknownTokens); }
            catch (ArgumentException e) { _profileResult.Text = e.Message; _profileResult.ForeColor = Theme.Warn; return; }
            var error = Program.RunElevated("--write-profile", image, value);
            _profileResult.Text = error ?? "Saved. Start " + image + " again to use the new settings.";
            _profileResult.ForeColor = error == null ? Theme.Good : Theme.Warn;
            RefreshProfiles();
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

        public string Describe()
        {
            var w = new StringBuilder();
            w.AppendLine(Program.ProductName + " " + Program.VersionText);
            foreach (var card in new[] { _driver, _gpu, _perfNow })
                foreach (var kv in card.Values) w.AppendLine(kv.Key + ": " + kv.Value.Text);
            w.AppendLine("stored dpm: " + _perfStored.Text);
            w.AppendLine("components: " + _components.Items.Count);
            foreach (ListViewItem i in _components.Items) w.AppendLine("  " + string.Join(" | ", i.SubItems.Cast<ListViewItem.ListViewSubItem>().Select(s => s.Text)));
            w.AppendLine("profiles: " + _profiles.Items.Count);
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
            ClientSize = new Size(900, 600); StartPosition = FormStartPosition.CenterParent; MinimizeBox = false;
            var top = new Panel { Dock = DockStyle.Top, Height = 54, Padding = new Padding(12, 8, 12, 0) };
            top.Controls.Add(Theme.Label("These files will be saved in:\n" + path));
            var list = new ListView
            {
                Dock = DockStyle.Top, Height = 200, View = View.Details, FullRowSelect = true, MultiSelect = false, HideSelection = false,
                BackColor = Theme.Card, ForeColor = Theme.Text, BorderStyle = BorderStyle.None,
            };
            list.Columns.Add("File", 170); list.Columns.Add("Size", 90); list.Columns.Add("Contents", 600);
            foreach (var e in report.Entries) list.Items.Add(new ListViewItem(new[] { e.Name, (e.Data.Length / 1024.0).ToString("0.0", System.Globalization.CultureInfo.InvariantCulture) + " KB", e.Description }) { Tag = e });
            var text = new TextBox
            {
                Dock = DockStyle.Fill, Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Both, WordWrap = false,
                BackColor = Theme.Nav, ForeColor = Theme.Text, Font = new Font("Consolas", 9f), BorderStyle = BorderStyle.None,
            };
            list.SelectedIndexChanged += (s, e) =>
            {
                if (list.SelectedItems.Count == 0) return;
                var entry = (ReportEntry)list.SelectedItems[0].Tag;
                var content = Encoding.UTF8.GetString(entry.Data);
                text.Text = content.Length > 200000 ? content.Substring(0, 200000) + "\r\n[... shortened in this view only ...]" : content.Replace("\r\n", "\n").Replace("\n", "\r\n");
            };
            var bottom = new FlowLayoutPanel { Dock = DockStyle.Bottom, Height = 48, FlowDirection = FlowDirection.RightToLeft, Padding = new Padding(8) };
            var save = Theme.Button("Save to Desktop", (s, e) => { DialogResult = DialogResult.OK; Close(); }, true);
            var cancel = Theme.Button("Cancel", (s, e) => { DialogResult = DialogResult.Cancel; Close(); });
            bottom.Controls.Add(save); bottom.Controls.Add(cancel);
            Controls.Add(text); Controls.Add(list); Controls.Add(top); Controls.Add(bottom);
            AcceptButton = save; CancelButton = cancel;
            if (list.Items.Count > 0) list.Items[0].Selected = true;
        }
    }
}
