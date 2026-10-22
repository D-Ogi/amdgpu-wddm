// The window's build gates, all without showing it (no window opens on a build machine).
//   Describe()        --smoke: every page's text, support view included.
//   RenderPages(...)  --smoke-render (G-RENDER, G-NOINT, G-A11Y): every page drawn to PNG at the default and the
//                     minimum window size; overlap, horizontal overflow, scroll range and page width checked; the text
//                     of every page outside the support view checked for internals; every interactive control checked
//                     for an accessible name; the guide panel's text present with and without the character; and
//                     G-SMALL, the rule: the right-hand panel is out of the window on a small
//                     screen (RenderOptions.WorkArea, so the render never reads the screen it runs on) and in it on a
//                     screen with room for it.
//   CheckLabels(...)  --smoke-labels: every page under the label layout rule of LabelLayout.cs (fact M898).
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
            if (!_smoke) throw new InvalidOperationException("fixtures require a smoke form");
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
                // BD-140: a browser's GPU process is in the list too; Home and Games do not offer it, and the Games
                // list draws the note that counts such entries.
                new RecentLaunch { Image = "msedge.exe", Path = @"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe", LastLaunchUtc = now.AddMinutes(-5), Apis = 3, Launches = 90 },
            };
            _upd = new UpdateCache { LastSuccessUtc = Recovery.Stamp(now.AddMinutes(-30)), LastAttemptUtc = Recovery.Stamp(now.AddMinutes(-30)), LastAttemptOutcome = "Available", CandidateTag = "v0.7.216.101-tester.29", CandidateVersion = "0.7.216.101-tester.29" };
            _drv = DriverCard.Decide(new DriverFacts { InstalledVersion = "0.7.216.100-tester.28", DriverDate = "10-3-2026" });
            // Exercise the long BD-138 attention text in every language, DPI and small-screen render gate.
            // Selection of this view from an exact witness is covered by StaleVerifyTests.
            _drv.Verification = VerifyKind.EarlierBootFailed;
            _drv.VerificationText = Strings.T("drv.verify.earlier-boot");
            _vram = new VideoMemoryState { Segments = 1, LocalResident = 3L << 30, Dedicated = 8L << 30, LocalLimit = 8L << 30 };
            // The board memory card as a BC-250 answers it: 8 GiB reserved and in use, 12 GiB the other choice,
            // nothing pending and no backup to restore. Without this a render on a PC without the board falls back
            // to the read-only video memory card, so the control the board really has is never drawn by a gate.
            // A board whose capabilities do not allow the setting keeps that fallback, which is what it must show.
            if (_uma == null && BoardCapabilities.Current.Allows(BoardCapabilities.Memory))
            {
                const uint inUseMiB = 8192;
                var block = new byte[28];
                Array.Copy(BitConverter.GetBytes(0x4c424124u), 0, block, 0, 4);       // the ABL block signature
                Array.Copy(BitConverter.GetBytes((ushort)inUseMiB), 0, block, 26, 2);
                uint sum = 0;
                for (int i = 6; i < block.Length; i++) sum += block[i];
                Array.Copy(BitConverter.GetBytes((ushort)sum), 0, block, 4, 2);
                _uma = new UmaState
                {
                    Supported = true, ProviderId = 1, AllowedFirstMiB = 8192, AllowedSecondMiB = 12288, NeedsRestart = 1,
                    ActiveValid = true, ReadValid = true, WriteAllowed = true, BackupAvailable = false,
                    ActiveBytes = (ulong)inUseMiB << 20, RequestedMiB = inUseMiB, ObservedBlock = block,
                };
            }
            // The graphics cores as a BC-250 answers them: the firmware's own stock set (CC
            // 0xFFF80000 per shader array, so pairs 1 to 3 of each group are fixed and 4 and 5 are free) and a
            // confirmed start of 30 of the 40 cores. Without this the map of pairs can only be drawn read-only,
            // and no render gate would ever measure a cell a person can click.
            if (_snap.Cu == null && BoardCapabilities.Current.Allows(BoardCapabilities.Gpu))
            {
                const uint mask = 0x84218;          // the documented default set of 30 cores (docs/design/cu-mode.md)
                _snap.Cu = new CuModeState
                {
                    Flags = CuModeState.FlagValid | CuModeState.FlagConfirmed,
                    Requested = CuMode.Full, Applied = CuMode.Full, ActiveCus = CuMode.ActiveCus(mask), DisableMask = mask,
                    Generation = _snap.Health != null ? _snap.Health.Generation : 0,
                    StockCc = new uint[] { 0xFFF80000, 0xFFF80000, 0xFFF80000, 0xFFF80000 },
                    StockSpi = new uint[] { 7, 7, 7, 7 },
                    ActiveWgps = CuMode.MapOf(mask, CuMode.DocumentedStockActive()),
                };
                if (_snap.P("CuMode") == null)
                {
                    _snap.Parameters["CuMode"] = CuMode.Full;
                    _snap.Parameters["CuDisableWgp"] = mask;
                    _snap.Parameters["CuModeConfirmed"] = CuMode.Encode(CuMode.Full, mask);
                    _snap.Parameters["CuModeChosen"] = CuMode.Encode(CuMode.Full, mask);
                }
            }
            // The "Now" card with every row filled: a recorded snapshot keeps its mode, reason and ceiling, and gets the
            // live readings it never recorded (37 % load, 62 C, the clock of its mode, 78 W from the SMU metrics table).
            if (_snap.Dpm == null) _snap.Dpm = new DpmState { Version = 0x000700D7, Mode = 1, Requested = 1, MaxMHz = DpmSettings.DefaultMaxMHz };
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
            // The Vulkan route for all games is the stored rollback (gdi, Compatible). MemoryOverflow stays in the fixture
            // although its row waits for the ICD (GraphicsSettings.AwaitingIcd): a computer can hold it from a hand
            // edit, and the pages must draw without it.
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
            if (_snap.Curve == null && BoardCapabilities.Current.Allows(BoardCapabilities.Gpu))
            {
                var line = Tuner.Table(); var floor = Tuner.Floors();
                var trial = Tuner.Preset("medium", line, floor);
                _snap.Curve = new CurveState
                {
                    Version = 0x000700D5, Flags = CurveState.FlagValid | CurveState.FlagGoverning | CurveState.FlagOnTrial | CurveState.FlagApplied | CurveState.FlagStored,
                    TrialMs = 120000, TrialRemainingMs = 87000, Serial = 4, Applied = 4, FirstMHz = Tuner.FirstMHz, StepMHz = Tuner.StepMHz, Points = (uint)Tuner.Points,
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
            // The ray tracing benchmark card at its widest: a copy of the game found, both passes measured, and a
            // history of three runs in which every reason a pass can have for no number appears once. The fixture
            // searches no disk of this PC (MainForm.Bench.cs ReadBenchmark says why).
            _bench = new InstallState { Folder = @"D:\Games\Quake II RTX", ExePath = @"D:\Games\Quake II RTX\q2rtx.exe", PakPath = @"D:\Games\Quake II RTX\baseq2\pak0.pak", Verdict = InstallVerdict.Ready, FromSteam = true };
            _benchHistory = new List<RunRecord>
            {
                new RunRecord { Utc = Recovery.Stamp(now.AddMinutes(-20)), Driver = "0.7.216.100-tester.28", Passes = new List<PassRecord>
                {
                    new PassRecord { Api = "pipeline", Outcome = PassOutcome.Done, Fps = 60.94, Frames = 1550, Seconds = 25.43, MaxTempC = 79, MeanClockMHz = 1948 },
                    new PassRecord { Api = "query", Outcome = PassOutcome.Done, Fps = 66.39, Frames = 1550, Seconds = 23.35, MaxTempC = 81, MeanClockMHz = 1951 },
                } },
                new RunRecord { Utc = Recovery.Stamp(now.AddDays(-1)), Driver = "0.7.216.100-tester.28", Passes = new List<PassRecord>
                {
                    new PassRecord { Api = "pipeline", Outcome = PassOutcome.TooHot, MaxTempC = 89, MeanClockMHz = 1700 },
                    new PassRecord { Api = "query", Outcome = PassOutcome.TooHot },
                } },
                new RunRecord { Utc = Recovery.Stamp(now.AddDays(-3)), Driver = "0.7.216.99-tester.27", Passes = new List<PassRecord>
                {
                    new PassRecord { Api = "pipeline", Outcome = PassOutcome.TimedOut, MaxTempC = 74 },
                    new PassRecord { Api = "query", Outcome = PassOutcome.NoResult, MaxTempC = 73 },
                } },
            };
            // No run of this session: the card then shows the newest saved run and the state line says so, which is
            // what a person sees who opens the application again (b23 lab round, F2). The card is as wide as with a
            // run of this session, because it shows the same run.
            _benchRun = null;
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
            public bool Nagi = true;        // the default of an installed application; --no-nagi turns it off
            // The working area of the screen the window is drawn for, in logical units. Empty: a screen with room for
            // the whole window, which is what every render assumed before the small-screen rule.
            public Size WorkArea;
        }

        public string RenderPages(RenderOptions o)
        {
            var w = new StringBuilder();
            WorkAreaForLayout = o.WorkArea;
            // A window never wider or taller than the screen it is drawn for: the window manager clamps a client size
            // larger than the desktop, and a render must draw what the person on that screen sees.
            var room = o.WorkArea.Width > 0 ? Theme.Sz(Math.Max(320, o.WorkArea.Width - 24), Math.Max(240, o.WorkArea.Height - 24)) : Size.Empty;
            foreach (var size in new[] { new { Name = "", Size = Theme.Sz(1240, 760) }, new { Name = "-min", Size = MinimumSize } })
            {
                var want = size.Size;
                if (room.Width > 0) want = new Size(Math.Min(want.Width, room.Width), Math.Min(want.Height, room.Height));
                ClientSize = want;
                BuildShell();
                // The rule: the right-hand panel is out of the window on a small screen, and in
                // it on a screen that has room for it. Both directions are findings, so neither can drift.
                bool sideInWindow = _side != null && HasVisibleFlag(_side);
                if (SmallScreen && sideInWindow) w.AppendLine("G-SMALL" + size.Name + ": the side panel shows on a small screen");
                if (!SmallScreen && ClientSize.Width >= Theme.S(1000) && !sideInWindow) w.AppendLine("G-SMALL" + size.Name + ": the side panel is missing on a screen that has room for it");
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
                    foreach (var p in Backdrops(this)) w.AppendLine(where + ": G-BACKDROP " + p);
                    if (_fixture && page == "graphics")
                    {
                        bool admitted = BoardCapabilities.Current.Allows(BoardCapabilities.Gpu | BoardCapabilities.Cpu);
                        if (_anchors.ContainsKey("graphics.tuning") != admitted)
                            w.AppendLine(where + ": board capability admission does not match the tuning controls");
                        if (!admitted && !Texts(built, false).Contains(Strings.T("board.unsupported")))
                            w.AppendLine(where + ": the unsupported board has no explanation");
                    }
                    if (_fixture && page == "system")
                        foreach (var anchor in new[] { "system.background", "system.updates", "system.drivers", "system.drive" })
                            if (!_anchors.ContainsKey(anchor)) w.AppendLine(where + ": Windows setting missing for this board: " + anchor);
                    // The map of the pairs of graphics cores is folded away until a person asks
                    // for it, so no run above draws its four rows of five check boxes. The page is measured again
                    // with the map open, by the same rules, and folded back before the next page.
                    if (page == "graphics" && !_cuMapOpen)
                    {
                        _cuMapOpen = true;
                        ShowPage(page, null, false);
                        PerformLayout();
                        CreateHandles(this);
                        string open = page + "-cumap" + size.Name;
                        Snap(Path.Combine(o.Directory, open + ".png"));
                        var mapPage = _content.Controls.Count > 0 ? _content.Controls[0] : null;
                        if (mapPage != null)
                        {
                            var end = LastShown(mapPage);
                            int endsAt = _content.PointToClient(end.Parent.PointToScreen(end.Location)).Y + end.Height;
                            int range = _content.DisplayRectangle.Bottom + _content.Padding.Bottom;
                            if (endsAt > range) w.AppendLine(open + ": the last control " + Label(end) + " ends at " + endsAt + ", outside the scroll range " + range);
                            var wide = LayoutRules.PageWidthFinding(open, mapPage.Width, PageColumnWidth, Theme.S(2));
                            if (wide != null) w.AppendLine(wide);
                        }
                        else w.AppendLine(open + ": empty page");
                        foreach (var p in Overlaps(this)) w.AppendLine(open + ": " + p);
                        foreach (var p in Overflows(this)) w.AppendLine(open + ": " + p);
                        foreach (var p in NoInternals(Texts(this, true))) w.AppendLine(open + ": G-NOINT " + p);
                        foreach (var p in Accessibility(this)) w.AppendLine(open + ": G-A11Y " + p);
                        foreach (var p in Backdrops(this)) w.AppendLine(open + ": G-BACKDROP " + p);
                        _cuMapOpen = false;
                        ShowPage(page, null, false);
                        PerformLayout();
                        CreateHandles(this);
                    }
                    if (_guide == null || string.IsNullOrEmpty(_guide.Text2)) w.AppendLine(where + ": the guide panel has no text");
                    if (_guide != null && _guide.ArtShown && !_prefs.ShowNagi) w.AppendLine(where + ": G-ART art shown with Show Nagi unchecked");
                    // The other direction, which no gate held before: with Show Nagi on, the art embedded and the
                    // side panel in the window, the guide panel must draw the picture of its own verdict. Every
                    // reason it could stay hidden (no resource, no verdict, a slot the layout dropped) is a finding.
                    if (_guide != null && _prefs.ShowNagi && _side != null && HasVisibleFlag(_side) && !_guide.ArtShown)
                        w.AppendLine(where + ": G-ART no art with Show Nagi on (verdict " + (_verdict != null ? _verdict.Expression : "none") +
                            ", resource " + (_verdict != null && NagiArt.Has(_verdict.Expression, Guide.ArtSize(Theme.Scale)) ? "present" : "absent") + ")");
                    if (_guide != null && _guide.Playing) w.AppendLine(where + ": G-PERF a frame sequence plays in a window that is not shown");
                    // "Show Nagi" turned off and on again on this page, with the verdict unchanged: the slot must
                    // hold her picture again. It runs where the side panel is in the window, so the minimum size
                    // (which has no side panel) is not measured and does not make this check pass by itself.
                    if (_guide != null && _side != null && HasVisibleFlag(_side) && _prefs.ShowNagi)
                    {
                        _prefs.ShowNagi = false; UpdateGuide();
                        if (_guide.ArtShown) w.AppendLine(where + ": G-ART the art stays in the panel after Show Nagi was turned off");
                        _prefs.ShowNagi = true; UpdateGuide();
                        if (!_guide.ArtShown) w.AppendLine(where + ": G-ART the art does not come back when Show Nagi is turned on again");
                    }
                }
            }
            if (!ThreeWayDialog.EscapeKeepsEditing()) w.AppendLine("G-A11Y: Escape in Save / Discard / Keep editing does not keep editing");
            // G-ORDER: the sizes the window gave itself in its constructor, for the screen it is drawn for, and the
            // order it really works in. The loop above sets the client size and builds after it; the live window
            // does the opposite - it takes its sizes and builds its page before it has a handle, and only then does
            // Windows clamp the window to the desktop and FitToScreens place it. Two things follow, and both were
            // wrong on a tester's 800x600 screen (GitHub #38): the smallest size the window allows must fit that
            // screen, or Windows cannot clamp the window at all and the page loses its right edge; and the page
            // must be built again for the column that is left after the clamp, which the Load handler does with
            // RebuildIfWidthChanged.
            if (o.WorkArea.Width > 0 && o.WorkArea.Height > 0)
            {
                var screen = Theme.Sz(o.WorkArea.Width, o.WorkArea.Height);
                if (MinimumSize.Width > screen.Width || MinimumSize.Height > screen.Height)
                    w.AppendLine("G-ORDER: the smallest size the window allows is " + MinimumSize.Width + "x" + MinimumSize.Height
                        + ", the screen it is drawn for has " + screen.Width + "x" + screen.Height);
                ClientSize = DesignSize;        // what the constructor asks for, before Windows clamps anything
                BuildShell();
                ShowPage("home", null, false);
                ClientSize = new Size(Math.Min(ClientSize.Width, room.Width), Math.Min(ClientSize.Height, room.Height));
                RebuildIfWidthChanged();
                ShowPage("home", null, false);
                PerformLayout();
                CreateHandles(this);
                var page = _content.Controls.Count > 0 ? _content.Controls[0] : null;
                if (page == null) w.AppendLine("G-ORDER: empty page after the window was clamped to the screen");
                else
                {
                    var cut = LayoutRules.PageWidthFinding("G-ORDER", page.Width, ColumnWidth, Theme.S(2));
                    if (cut != null) w.AppendLine(cut + " after the window was clamped to the screen");
                }
            }
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

        // --smoke-labels: the shell and every page, at the default window size, under the label layout rule
        // (LabelLayout.cs). The labels a timer changes (_live) are among the labels checked, and the tally counts them.
        internal void CheckLabels(LabelLayout.Tally t, string state)
        {
            ClientSize = Theme.Sz(1240, 760);
            BuildShell();
            foreach (var page in PageNames)
            {
                ShowPage(page, null, false);
                PerformLayout();
                LabelLayout.Check(this, "main window, " + state + ", " + page, t, new HashSet<Label>(_live.Values));
            }
        }

        // G-BACKDROP (BD-137): a control that paints itself leaves no pixel of its client rectangle unpainted.
        //
        // Control.WmPaint runs two layers for a control with ControlStyles.UserPaint: the background layer, and that
        // one only when ControlStyles.Opaque is not set, then OnPaint (.NET Framework reference source, Control.cs,
        // WmPaint and PaintWithErrorHandling). ButtonBase sets Opaque, so a button, option button or check box paints
        // its whole background itself. On the screen every control paints into a buffer that BufferedGraphicsManager
        // hands to the next control with the pixels of the last one still in it, so a control that does not clear
        // itself shows the control painted before it - which is what the setup window did in BD-137. A render into
        // one bitmap cannot show it: there the parent has already drawn the right pixels underneath. This gate paints
        // every control alone, over a colour no page uses, and reports whatever is left of it.
        //
        // The setup window's gate has a second half, which asks whether the colour a control clears itself in is the
        // colour the window paints under it. It has no counterpart here, and there is nothing to compare against: no
        // control of this application takes a background colour from an ancestor. Every container carries an opaque
        // BackColor of its own (CardPanel Theme.Card, GuidePanel Theme.GuideBack, the two charts Theme.Nav), and the
        // controls with Color.Transparent are stock ones, whose painting routes the transparent colour through
        // Control.PaintBackground and so through PaintTransparentBackground, which paints the parent chain itself
        // (probe of 2026-10-13: a stock RadioButton with a transparent BackColor inside a transparent layout panel
        // leaves no pixel of a sentinel background behind, although ButtonBase sets ControlStyles.Opaque). The half
        // above is what keeps the BD-137 shape - an owner-drawn control that fills a colour of its parent's instead -
        // out of this application.
        public List<string> Backdrops(Control root)
        {
            var problems = new List<string>();
            foreach (var c in ShownIn(root, false))
            {
                if (c.Width <= 0 || c.Height <= 0) continue;
                if (!Style(c, ControlStyles.UserPaint)) continue;
                if (c.GetType().Assembly != typeof(MainForm).Assembly && c.BackColor.A == 255) continue;
                int left;
                using (var bmp = new Bitmap(c.Width, c.Height, System.Drawing.Imaging.PixelFormat.Format32bppRgb))
                {
                    using (var g = Graphics.FromImage(bmp))
                    {
                        g.Clear(Sentinel);
                        var e = new PaintEventArgs(g, new Rectangle(0, 0, c.Width, c.Height));
                        if (!Style(c, ControlStyles.Opaque)) InvokePaintBackground(c, e);
                        InvokePaint(c, e);
                    }
                    left = Unpainted(bmp);
                }
                if (left > 0)
                    problems.Add(Label(c) + " " + c.Width + "x" + c.Height + " leaves " + left + " pixels of its background unpainted (parent " +
                        (c.Parent == null ? "none" : c.Parent.GetType().Name + " BackColor " + c.Parent.BackColor) + ")");
            }
            return problems;
        }

        // A colour no palette of this application carries.
        static readonly Color Sentinel = Color.FromArgb(0xFF, 0x00, 0xFF);

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

        static readonly System.Reflection.MethodInfo GetStyleMethod =
            typeof(Control).GetMethod("GetStyle", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance);
        static bool Style(Control c, ControlStyles s) { return (bool)GetStyleMethod.Invoke(c, new object[] { s }); }

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
        static bool HasVisibleFlag(Control c) { return Ui.HasVisibleFlag(c); }

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

        // ---- --smoke-scroll (the place a rebuild keeps) -------------------------------------------------------------
        //
        // A page is rebuilt under the person's hands whenever a card gets a new reading, and the place the person
        // scrolled to must survive that. It did not: a page built into a panel that was still scrolled came out with
        // a scroll range too large by the scroll offset, and the view then stood past the end of the new content
        // (b23 lab round, F1). The card of a run that has just started is shorter than the card of a finished one,
        // and the person who presses Start is at the end of the page, where that card is.
        //
        // The gate needs no window. The controls of a window that was never shown are not "created" in the sense of
        // WinForms, and the setter of AutoScrollPosition does nothing on a control that is not created, so the gate
        // creates the control tree the way Show() would (Control.CreateControl(true)). After that the panel scrolls
        // exactly as the window's panel does. What the focus does after a rebuild needs a real window, so the rule
        // of that is a pure function with its own host tests (PageFocus.After).
        public string ScrollKeeping()
        {
            var w = new StringBuilder();
            ShowPage("performance", null, false);
            CreateTree();

            int at = -_content.AutoScrollPosition.Y;
            if (at != 0) w.AppendLine("the page is not built at its top: " + at + " px");
            int end = PageEnd();
            if (end < 800)
            {
                w.AppendLine("the performance page of this fixture is only " + end + " px longer than the window, too short for this check");
                return w.ToString();
            }

            // In the middle of the page: a rebuild keeps the place and leaves the scroll range as it was.
            _content.AutoScrollPosition = new Point(0, 600);
            int before = -_content.AutoScrollPosition.Y, range = _content.DisplayRectangle.Height;
            if (before != 600) w.AppendLine("a scroll to 600 px gave " + before + " px");
            ShowPage("performance", null, false);
            int after = -_content.AutoScrollPosition.Y;
            if (after != before) w.AppendLine("a rebuild moved the page from " + before + " px to " + after + " px");
            if (_content.DisplayRectangle.Height != range)
                w.AppendLine("a rebuild changed the scroll range from " + range + " px to " + _content.DisplayRectangle.Height + " px");

            // Another page opens at its top, never where the page before it stood.
            _content.AutoScrollPosition = new Point(0, 600);
            ShowPage("driver", null, false);
            int other = -_content.AutoScrollPosition.Y;
            if (other != 0) w.AppendLine("another page opened " + other + " px down, where the page before it stood");
            ShowPage("performance", null, false);

            // At the end of the page, where the benchmark card is, with a rebuild that makes that card shorter: the
            // view belongs at the end of the new content, never past it and never back at the top.
            _content.AutoScrollPosition = new Point(0, end);
            int atEnd = -_content.AutoScrollPosition.Y;
            _benchRun = new BenchProgress
            {
                Stage = BenchProgress.Starting, Busy = true,
                Run = new RunRecord { Utc = Recovery.Stamp(DateTime.UtcNow), Driver = "0.7.216.100-tester.28" },
            };
            ShowPage("performance", null, false);
            int shorter = PageEnd(), now = -_content.AutoScrollPosition.Y;
            if (shorter >= atEnd)
                w.AppendLine("the card of a run that has just started is not shorter than the card of a finished one, so this check measures nothing");
            else if (now > shorter) w.AppendLine("after a rebuild that made the page shorter the view stands " + (now - shorter) + " px past its end");
            else if (now == 0) w.AppendLine("a rebuild that made the page shorter put the page back at its top");
            return w.ToString();
        }


        // ---- --smoke-scroll, part two: the card of a run that is going on keeps its place ---------------------------
        //
        // While a benchmark run goes on, "Start the test" and "Stop the test" must stay where they are. They did
        // not: the case fan card above gets the line "The fan is at full speed now, because the graphics are working
        // hard." as soon as the game loads the chip, and the whole benchmark card moved 23 px down with it, 9 to 18 s
        // into the run, under the pointer of a person who was aiming at Stop (b23 lab round 2, F7). This gate builds
        // every state of a run in the control tree and measures where the two buttons sit in the view.
        //
        // The measure is the place of a control in the page less the scroll offset, not the control bounds: a page
        // built without a window is laid out, but its controls are not moved by the panel the way a shown window
        // moves them, and the place in the page is what the person sees against the window.
        public string RunStateLayout()
        {
            var w = new StringBuilder();
            ShowPage("performance", null, false);
            CreateTree();
            Control card;
            if (!_anchors.TryGetValue("perf.benchmark", out card)) { w.AppendLine("the performance page has no benchmark card"); return w.ToString(); }

            // Where the person stands when they press Start: at the card, as the search leaves the page, with the
            // card as far up as it goes (a card taller than the view then has its top edge above it).
            int view = _content.ClientSize.Height;
            _content.AutoScrollPosition = new Point(0, ContentTop(card) + Math.Max(0, card.Height - view));
            int ready = ViewY(Strings.T("bench.start"));
            int above = CardTop();
            Check(w, "before the run", ready, ready);

            // Start: the page does not move, because the buttons are in reach already.
            var run = new RunRecord { Utc = Recovery.Stamp(DateTime.UtcNow), Driver = "0.7.216.100-tester.28" };
            _benchRun = new BenchProgress { Stage = BenchProgress.Starting, Busy = true, Run = run };
            ShowPage(_page, null, false);
            BringIntoView("perf.benchmark", "perf.benchmark.buttons");
            Check(w, "the run starts", ready, ViewY(Strings.T("bench.start")));

            _benchRun = new BenchProgress { Stage = BenchProgress.Running, Busy = true, Pass = "pipeline", ElapsedMs = 9000, Run = run };
            ShowPage(_page, null, false);
            Check(w, "the first way runs", ready, ViewY(Strings.T("bench.start")));

            // The fan card above gets its line while the game loads the chip. Everything under it moves, which is
            // what this gate is for; without that line it would measure nothing.
            if (_snap != null && _snap.Fan != null) _snap.Fan.Flags |= FanState.FlagBoost;
            ShowPage(_page, null, false);
            if (CardTop() == above)
                w.AppendLine("the card above the benchmark card did not change height at full fan speed, so this check measures nothing");
            Check(w, "the fan goes to full speed", ready, ViewY(Strings.T("bench.start")));

            run.Passes.Add(new PassRecord { Api = "pipeline", Outcome = PassOutcome.Done, Fps = 64.05, Frames = 631, Seconds = 9.85, MaxTempC = 72, MeanClockMHz = 1227 });
            _benchRun = new BenchProgress { Stage = BenchProgress.Reading, Busy = true, Pass = "pipeline", ElapsedMs = 18000, Run = run };
            ShowPage(_page, null, false);
            Check(w, "the first way has its numbers", ready, ViewY(Strings.T("bench.start")));

            _benchRun = new BenchProgress { Stage = BenchProgress.Running, Busy = true, Pass = "query", ElapsedMs = 9000, Run = run };
            ShowPage(_page, null, false);
            Check(w, "the second way runs", ready, ViewY(Strings.T("bench.start")));

            run.Passes.Add(new PassRecord { Api = "query", Outcome = PassOutcome.Done, Fps = 65.10, Frames = 631, Seconds = 9.69, MaxTempC = 74, MeanClockMHz = 1270 });
            _benchRun = new BenchProgress { Stage = BenchProgress.Finished, Run = run };
            if (_snap != null && _snap.Fan != null) _snap.Fan.Flags &= ~FanState.FlagBoost;
            ShowPage(_page, null, false);
            Check(w, "the run is finished", ready, ViewY(Strings.T("bench.start")));

            var stopped = new RunRecord { Utc = Recovery.Stamp(DateTime.UtcNow), Driver = "0.7.216.100-tester.28" };
            stopped.Passes.Add(new PassRecord { Api = "pipeline", Outcome = PassOutcome.Cancelled });
            stopped.Passes.Add(new PassRecord { Api = "query", Outcome = PassOutcome.Cancelled });
            _benchRun = new BenchProgress { Stage = BenchProgress.Finished, Run = stopped };
            ShowPage(_page, null, false);
            Check(w, "the person stopped the run", ready, ViewY(Strings.T("bench.start")));
            return w.ToString();
        }

        // The two buttons of the card in one state of a run: at the place they had before, and both inside the view.
        void Check(StringBuilder w, string state, int ready, int now)
        {
            if (now != ready) w.AppendLine("\"Start the test\" stands " + (now - ready) + " px from where it was when " + state + ": " + ready + " px against " + now);
            foreach (var id in new[] { "bench.start", "bench.cancel" })
            {
                var b = Named(Strings.T(id));
                if (b == null) { w.AppendLine("the card has no " + Strings.T(id) + " button when " + state); continue; }
                int top = ViewY(Strings.T(id));
                if (top < 0 || top + b.Height > _content.ClientSize.Height)
                    w.AppendLine("\"" + b.AccessibleName + "\" is outside the view when " + state + ": " + top + " px to " + (top + b.Height) + " px of " + _content.ClientSize.Height);
            }
        }

        // The benchmark card of the page as it stands now: every rebuild makes a new one, so the gate asks again.
        int CardTop()
        {
            Control card;
            return _anchors.TryGetValue("perf.benchmark", out card) ? ContentTop(card) : int.MinValue;
        }

        Control Named(string text)
        {
            string name = text.TrimEnd('.', '\u2026');
            return All(_content).FirstOrDefault(c => c is Button && c.AccessibleName == name);
        }

        // Where a control of the page stands in the view: its place in the page less the scroll offset.
        int ViewY(string text)
        {
            var c = Named(text);
            return c == null ? int.MinValue : ContentTop(c) + _content.AutoScrollPosition.Y;
        }

        // The control tree of a window that is never shown. The setter of AutoScrollPosition does nothing on a
        // control that is not created, so the gate creates the tree the way Show() would.
        void CreateTree()
        {
            PerformLayout();
            CreateHandles(this);
            var create = typeof(Control).GetMethod("CreateControl", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance,
                null, new[] { typeof(bool) }, null);
            foreach (var c in new[] { (Control)this }.Concat(All(this))) create.Invoke(c, new object[] { true });
            PerformLayout();
        }

        // How far the page can be scrolled: what its content has above the window.
        int PageEnd()
        {
            int content = _content.Controls.Count > 0 ? _content.Controls[0].Height + _content.Padding.Vertical : 0;
            return Math.Max(0, content - _content.ClientSize.Height);
        }
    }
}
