// The window's build gates, all without showing it (the owner's rule: no window on the development PC).
//   Describe()        --smoke: every page's text, support view included.
//   RenderPages(...)  --smoke-render (G-RENDER, G-NOINT, G-A11Y): every page drawn to PNG at the default and the
//                     minimum window size; overlap, horizontal overflow, scroll range and page width checked; the text
//                     of every page outside the support view checked for internals; every interactive control checked
//                     for an accessible name; the guide panel's text present with and without the character.
//   UseFixture(...)   a recorded snapshot and demo games instead of this PC's state, so that the cards with
//                     warnings, games and the CU section are drawn too. Nothing is written in any of them.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using System.Windows.Forms;

namespace AmdgpuWddmControl
{
    public sealed partial class MainForm
    {
        bool _fixture;

        public void UseFixture(RecoverySnapshot s)
        {
            _fixture = true;
            _snap = s ?? new RecoverySnapshot();
            if (_snap.GameProfiles == null) _snap.GameProfiles = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            // witcher3: one Invert group off, one opt-in group partial, one unknown name kept.
            // ascent: three of the four names of an Invert group, which draws that group partial.
            _snap.GameProfiles["witcher3.exe"] = "raytracing-tier-off,present-noprimary,x-future-switch";
            _snap.GameProfiles["ascent.exe"] = "recording-bind-off,retire-handoff-off,deferred-replay-off";
            if (_snap.DefaultApplications == null) _snap.DefaultApplications = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            _snap.DefaultApplications["witcher3.exe"] = "raytracing-tier-off";
            var now = DateTime.UtcNow;
            _recent = new List<RecentLaunch>
            {
                new RecentLaunch { Image = "witcher3.exe", Path = @"D:\Games\The Witcher 3\bin\x64_dx12\witcher3.exe", LastLaunchUtc = now.AddHours(-2), Apis = 1, Launches = 12 },
                new RecentLaunch { Image = "ROTTR.exe", Path = @"D:\Games\Rise of the Tomb Raider\ROTTR.exe", LastLaunchUtc = now.AddDays(-1), Apis = 1, Launches = 3 },
                new RecentLaunch { Image = "factorio.exe", Path = @"D:\Games\Factorio\bin\x64\factorio.exe", LastLaunchUtc = now.AddDays(-3), Apis = 2, Launches = 40 },
            };
            _upd = new UpdateCache { LastSuccessUtc = Recovery.Stamp(now.AddMinutes(-30)), LastAttemptUtc = Recovery.Stamp(now.AddMinutes(-30)), LastAttemptOutcome = "Available", CandidateTag = "v1.0.1.0-tester.12", CandidateVersion = "1.0.1.0-tester.12" };
            _drv = DriverCard.Decide(new DriverFacts { InstalledVersion = "1.0.0.0-tester.11", DriverDate = "10-3-2026" });
            _vram = new VideoMemoryState { Segments = 1, LocalResident = 3L << 30, Dedicated = 8L << 30, LocalLimit = 8L << 30 };
            // The "Now" card with every row filled: a recorded snapshot keeps its mode, reason and ceiling, and gets the
            // live readings it never recorded (37 % load, 62 C, the clock of its mode, 78 W from the SMU metrics table).
            if (_snap.Dpm == null) _snap.Dpm = new DpmState { Version = 0x000700D7, Mode = 1, Requested = 1, MaxMHz = 1500 };
            {
                var d = _snap.Dpm;
                const uint live = DpmState.FlagTemperature | DpmState.FlagClock | DpmState.FlagHwBusy | DpmState.FlagPower;
                if ((d.Flags & live) == 0)
                {
                    uint mhz = d.Mode == 1 ? 1500u : 1000u;
                    d.Flags |= DpmState.FlagRunning | live;
                    d.AbiVersion = 3; d.MetricsState = DpmState.MetricsOk; d.MetricsAgeMs = 400;
                    d.BusyPermille = 360; d.BusyAvgPermille = 370; d.TemperatureMc = 62000;
                    d.CurrentMHz = mhz; d.ObservedMHz = mhz; d.CurrentMv = d.Mode == 1 ? 919u : 820u;
                    d.SocketPowerMw = 78000; d.SocketPowerAvgMw = 77400; d.GfxPowerMw = 48000; d.SocPowerMw = 21000;
                    d.GfxMv = d.CurrentMv; d.SocMv = 900;
                }
            }
            _game = "witcher3.exe";
            _gameEdits["witcher3.exe"] = new Dictionary<string, bool> { { "cpu", true } };
            // The graphics settings at their widest: a frame rate limit and the compatibility route for all games, an
            // overlay value outside the contract (drawn as not valid), witcher3 with values of its own (one not
            // valid), a changed shader model, and one edit in each scope. A fixture with keys of its own keeps them.
            // The two Vulkan values stay in the fixture although their rows wait for the ICD (GraphicsSettings.AwaitingIcd):
            // a computer can hold them from a hand edit, and the pages must draw without them.
            if (_snap.GfxKeys == null)
            {
                _snap.GfxKeys = new List<GfxKey>();
                var all = new GfxKey { Path = GraphicsSettings.GraphicsPath, SubKeys = 1 };
                all.Values["FrameRateLimit"] = GfxValue.Dword(60); all.Values["PerformanceOverlay"] = GfxValue.Dword(7);
                var vk = new GfxKey { Path = GraphicsSettings.VulkanPath };
                vk.Values["WsiRoute"] = GfxValue.Str("gdi");
                var w3 = new GfxKey { Path = GraphicsSettings.AppPath(GraphicsSettings.GraphicsPath, "witcher3.exe") };
                w3.Values["VSync"] = GfxValue.Dword(1); w3.Values["Anisotropy"] = GfxValue.Dword(7);
                var w3vk = new GfxKey { Path = GraphicsSettings.AppPath(GraphicsSettings.VulkanPath, "witcher3.exe") };
                w3vk.Values["MemoryOverflow"] = GfxValue.Str("strict");
                _snap.GfxKeys.AddRange(new[] { all, vk, w3, w3vk });
            }
            _gfxGlobalEdits["MaxFrameLatency"] = "1";
            _gfxGameEdits["witcher3.exe"] = new Dictionary<string, string> { { "FrameRateLimit", "144" } };
            _smEdits["witcher3.exe"] = "6.7";
            var monitor = DisplayInfo.Current().FirstOrDefault();
            if (monitor != null) _displayEdits[monitor.Device] = new DisplayEdit { Width = monitor.Width, Height = monitor.Height, RefreshHz = monitor.RefreshHz, Scaling = "center" };
            _ceilEdited = true; _ceilEdit = 1800;
            // The fan card at its widest: the driver runs the standard curve (stored), the person has dragged a curve of
            // six points on the chart and applied a choice before (so Undo shows), and the ring marks 61.5 C at 72 % (the standard curve there).
            // The render gates see every row the card can have short of the eight-point maximum.
            if (_snap.Fan == null)
            {
                _snap.Fan = new FanState
                {
                    Version = 0x000700D5, Flags = FanState.FlagEnabled | FanState.FlagControlling | FanState.FlagStored, Mode = FanState.ModeCurve,
                    State = FanState.StateCurve, Profile = FanState.ProfileStandard, Points = 5, Rpm = 1180, Generation = 5,
                    StoredMode = FanState.ModeCurve, StoredProfile = FanState.ProfileStandard, GuardMc = 61500, TargetPct = 72, AppliedPct = 72,
                    CurveC = new uint[] { 40, 60, 70, 80, 85, 0, 0, 0 }, CurvePct = new uint[] { 50, 70, 82, 95, 100, 0, 0, 0 },
                };
                _fanEditC = new uint[] { 35, 50, 62, 70, 78, 84 };
                _fanEditPct = new uint[] { 30, 42, 58, 70, 88, 100 };
                _fanChoice = "custom";
                _fanPoint = 2;
                _fanUndo = new FanChoiceRecord { Choice = "quiet" };
            }
            // The tuning cards open and at their widest: a curve test running over a saved curve (every row differs from
            // the one in force), and processor tuning on with one readback and saved settings. A fixture that brings
            // its own readings (test/snapshot-tuner.json) keeps them.
            _tuningOpen = true;
            if (_snap.Curve == null)
            {
                var line = Tuner.Table(); var floor = Tuner.Floors();
                var trial = Tuner.Preset("medium", line, floor);
                _snap.Curve = new CurveState
                {
                    Version = 0x000700D5, Flags = CurveState.FlagValid | CurveState.FlagGoverning | CurveState.FlagOnTrial | CurveState.FlagApplied | CurveState.FlagStored,
                    TrialMs = 120000, TrialRemainingMs = 87000, Serial = 4, Applied = 4, FirstMHz = Tuner.FirstMHz, StepMHz = Tuner.StepMHz, Points = Tuner.Points,
                    Candidate = trial, Active = (uint[])trial.Clone(), Stored = Tuner.Preset("mild", line, floor), Default = line, Floor = floor,
                    Level = 5, LevelMHz = 1500, LevelMv = trial[5], CeilingMHz = 1500, Mode = 1, TemperatureMc = 64000, Sets = 2, Keeps = 1, Generation = 5,
                };
            }
            if (_snap.Cpu == null)
            {
                _snap.Parameters["CpuTune"] = 1;
                _snap.Cpu = new CpuState
                {
                    Version = 0x000700D5, Flags = CpuState.FlagValid | CpuState.FlagTuneOn | CpuState.FlagQueue3Proven | CpuState.FlagStored | CpuState.FlagTempValid,
                    AppliedMaxMHz = 3300, AppliedUvSteps = 4, AppliedTempC = 90, StoredMaxMHz = 3300, StoredUvSteps = 4, StoredTempC = 90,
                    BaselineMaxMHz = 3500, BaselineTempC = 95, VoltageMv = 1012, CapC = 90, TemperatureMc = 61000,
                    CoreMHz = new uint[] { 3290, 3290, 3280, 3290, 3290, 3280, 0, 0 }, PstateMHz = new uint[] { 3500, 2800, 1600, 0, 0, 0, 0, 0 },
                    Cores = 6, Threads = 12, CoreMask = CpuTuning.MaskStock, Reads = 3, Writes = 2, Generation = 5,
                };
            }
            _fanEditC = new uint[] { 35, 50, 60, 70, 80, 85 };
            _fanEditPct = new uint[] { 30, 40, 55, 70, 90, 100 };
            _fanChoice = "custom";
            ComputeStatus();
            ShowPage(_page, null, false);
        }

        // ---- --smoke ------------------------------------------------------------------------------------------------

        public string Describe()
        {
            var w = new StringBuilder();
            w.AppendLine(Program.ProductName + " " + Program.VersionText);
            w.AppendLine("Status: " + _status.Title);
            foreach (var i in _status.Items) w.AppendLine("  [" + i.Severity + "] " + i.Text + (i.Action != null ? " -> " + i.Action : ""));
            w.AppendLine("Guide: " + (_verdict == null ? "none" : _verdict.Cause + " (" + _verdict.Expression + ")"));
            string back = _page;
            foreach (var page in PageNames)
            {
                ShowPage(page, null, false);
                CreateHandles(this);
                w.AppendLine("[" + page + "]");
                foreach (var t in Texts(_content, false)) w.AppendLine("  " + t.Replace("\r", " ").Replace("\n", " "));
            }
            ShowPage(back, null, false);
            return w.ToString();
        }

        // ---- --smoke-render ------------------------------------------------------------------------------------------

        public sealed class RenderOptions
        {
            public string Directory, SwitchTo;
            public bool Nagi;
        }

        public string RenderPages(RenderOptions o)
        {
            var w = new StringBuilder();
            foreach (var size in new[] { new { Name = "", Size = Theme.Sz(1240, 760) }, new { Name = "-min", Size = MinimumSize } })
            {
                ClientSize = size.Size;
                BuildShell();
                foreach (var page in PageNames)
                {
                    ShowPage(page, null, false);
                    PerformLayout();
                    CreateHandles(this);
                    Snap(Path.Combine(o.Directory, page + size.Name + ".png"));
                    string where = page + size.Name;
                    var built = _content.Controls.Count > 0 ? _content.Controls[0] : null;
                    if (built == null) { w.AppendLine(where + ": empty page"); continue; }
                    if (size.Name == "")
                        using (var bmp = new Bitmap(Math.Max(1, built.Width), Math.Max(1, built.Height)))
                        {
                            built.DrawToBitmap(bmp, new Rectangle(0, 0, built.Width, built.Height));
                            bmp.Save(Path.Combine(o.Directory, page + "-full.png"), System.Drawing.Imaging.ImageFormat.Png);
                        }
                    var last = LastShown(built);
                    int bottom = _content.PointToClient(last.Parent.PointToScreen(last.Location)).Y + last.Height;
                    int extent = _content.DisplayRectangle.Bottom + _content.Padding.Bottom;
                    if (bottom > extent) w.AppendLine(where + ": the last control " + Label(last) + " ends at " + bottom + ", outside the scroll range " + extent);
                    // PageColumnWidth, not ColumnWidth: the window manager may have clamped ClientSize since the page
                    // was built (LayoutRules.PageWidthFinding says why).
                    var tooWide = LayoutRules.PageWidthFinding(where, built.Width, PageColumnWidth, Theme.S(2));
                    if (tooWide != null) w.AppendLine(tooWide);
                    foreach (var p in Overlaps(this)) w.AppendLine(where + ": " + p);
                    foreach (var p in Overflows(this)) w.AppendLine(where + ": " + p);
                    foreach (var p in NoInternals(Texts(this, true))) w.AppendLine(where + ": G-NOINT " + p);
                    foreach (var p in Accessibility(this)) w.AppendLine(where + ": G-A11Y " + p);
                    if (_guide == null || string.IsNullOrEmpty(_guide.Text2)) w.AppendLine(where + ": the guide panel has no text");
                    if (_guide != null && _guide.ArtShown && !_prefs.ShowNagi) w.AppendLine(where + ": G-ART art shown with Show Nagi unchecked");
                    if (_guide != null && _guide.Playing) w.AppendLine(where + ": G-PERF a frame sequence plays in a window that is not shown");
                }
            }
            if (!ThreeWayDialog.EscapeKeepsEditing()) w.AppendLine("G-A11Y: Escape in Save / Discard / Keep editing does not keep editing");
            if (o.SwitchTo != null)
            {
                SwitchLanguageForTest(o.SwitchTo);
                ShowPage("home", null, false);
                CreateHandles(this);
                Snap(Path.Combine(o.Directory, "home-switched-" + o.SwitchTo + ".png"));
                foreach (var kv in _navButtons)
                    if (kv.Value.Text != Strings.In(o.SwitchTo, "nav." + kv.Key)) w.AppendLine("language switch: the " + kv.Key + " button still reads " + kv.Value.Text);
                foreach (var p in Overlaps(this)) w.AppendLine("home after the switch: " + p);
            }
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

        // ---- the checks --------------------------------------------------------------------------------------------

        // Control.Visible reads false for every control of a form that was never shown, so the checks use the control's
        // own visible flag (STATE_VISIBLE).
        static readonly System.Reflection.MethodInfo GetState = typeof(Control).GetMethod("GetState", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance);
        static bool HasVisibleFlag(Control c) { return (bool)GetState.Invoke(c, new object[] { 2 }); }

        static void CreateHandles(Control c)
        {
            if (!c.IsHandleCreated) { var h = c.Handle; }
            foreach (Control k in c.Controls) CreateHandles(k);
        }

        static IEnumerable<Control> ShownIn(Control root, bool skipSupport)
        {
            foreach (Control c in root.Controls)
            {
                if (!HasVisibleFlag(c)) continue;
                if (skipSupport && "support".Equals(c.Tag)) continue;
                yield return c;
                foreach (var k in ShownIn(c, skipSupport)) yield return k;
            }
        }

        public static List<string> Texts(Control root, bool skipSupport)
        {
            var list = new List<string>();
            foreach (var c in ShownIn(root, skipSupport))
            {
                if (c is TextBox || c is ListBox) continue;
                if (!string.IsNullOrEmpty(c.Text)) list.Add(c.Text);
                if (!string.IsNullOrEmpty(c.AccessibleName) && c.AccessibleName != c.Text) list.Add(c.AccessibleName);
            }
            return list;
        }

        static Control LastShown(Control c)
        {
            var kids = c.Controls.Cast<Control>().Where(HasVisibleFlag).ToList();
            if (kids.Count == 0) return c;
            return LastShown(kids.OrderBy(k => k.Bottom).Last());
        }

        static string Label(Control c) { return c.GetType().Name + (string.IsNullOrEmpty(c.Text) ? "" : " \"" + (c.Text.Length > 40 ? c.Text.Substring(0, 40) : c.Text) + "\""); }

        static IEnumerable<string> Overlaps(Control parent)
        {
            var kids = parent.Controls.Cast<Control>().Where(HasVisibleFlag).Where(k => !(k is ListBox)).ToList();
            for (int i = 0; i < kids.Count; i++)
                for (int j = i + 1; j < kids.Count; j++)
                {
                    // Docked panels of the shell share edges by design; only an intersection with area counts.
                    var a = kids[i].Bounds; a.Intersect(kids[j].Bounds);
                    if (a.Width > 0 && a.Height > 0)
                        yield return Label(kids[i]) + " " + kids[i].Bounds + " overlaps " + Label(kids[j]) + " " + kids[j].Bounds;
                }
            foreach (var k in kids) foreach (var o in Overlaps(k)) yield return o;
        }

        // A control wider than the room its parent gives it is cut on the right (labels wrap; buttons do not).
        static IEnumerable<string> Overflows(Control root)
        {
            foreach (var c in ShownIn(root, false))
            {
                var parent = c.Parent;
                if (parent == null || parent is Form || parent is ScrollableControl && ((ScrollableControl)parent).AutoScroll) continue;
                int room = parent.ClientSize.Width - parent.Padding.Right;
                if (c.Right > room + 1) yield return Label(c) + " ends at " + c.Right + ", its parent " + Label(parent) + " has " + room;
            }
        }

        static IEnumerable<string> NoInternals(IEnumerable<string> texts) { return PlainWords.Findings(texts); }

        // G-A11Y: every interactive control has a name for screen readers and is reachable with Tab; the character
        // image is decorative.
        static IEnumerable<string> Accessibility(Control root)
        {
            foreach (var c in ShownIn(root, false))
            {
                bool interactive = c is ButtonBase || c is TextBox || c is ListBox || c is ComboBox;
                if (interactive && string.IsNullOrWhiteSpace(c.AccessibleName)) yield return Label(c) + " has no accessible name";
                if (interactive && !c.TabStop && !(c is RadioButton)) yield return Label(c) + " cannot be reached with Tab";
                if (c is PictureBox && c.AccessibleRole != AccessibleRole.None) yield return "the character image is not marked decorative";
            }
        }

        // ---- --smoke-perf (G-PERF) ---------------------------------------------------------------------------------

        public bool ReadOnlyProbe;
    }
}
