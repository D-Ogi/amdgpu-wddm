// GUI phase 1 host tests: app preferences (R4), the guide verdict (G-ART), recent launches (A4), search (WU-009,
// WU-071), window recovery (WU-036), cache inventory, sensor rows, game groups (G-PROF), restart hints (WU-060), the
// Home status card, the plain dialogs (G-PLAN: every write has a sentence), the poll policy (G-PERF), the plain-words
// check over the string tables (G-NOINT), the help content (WU-063, WU-064) and the source scan (G-SRC).
using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using AmdgpuWddmControl;

static partial class UnitTests
{
    static void GuiTests(string root)
    {
        Strings.Language = "en";
        Preferences();
        GuideVerdicts();
        RecentLaunchRecords();
        Search();
        WindowFit();
        Caches();
        SensorRows();
        GroupsAndOrigins();
        RestartHints();
        StatusCards();
        PlainDialogs();
        Polling();
        PlainWordsInTables();
        HelpContent();
        SourceScan(root);
    }

    // R4: the default is "value absent"; turning a choice on and off again leaves nothing behind.
    static void Preferences()
    {
        var store = new MemoryPrefStore();
        var p = new AppPrefs(store);
        Check(!p.ShowNagi && !p.ShowTipsAutomatically && !p.ReduceAnimations && !p.ShowSupportOptions && !p.GettingStartedDismissed, "R4: off by default");
        Check(p.UpdateCheckAtStart && p.RecordRecentLaunches, "R4: D2 and D3 are on while absent");
        Equal(0, store.Writes, "R4: reading writes nothing");
        p.ShowNagi = false; p.UpdateCheckAtStart = true; p.RecordRecentLaunches = true; p.HiddenGames = new HashSet<string>();
        Equal(0, store.Writes, "R4: setting a default writes nothing");
        p.ShowNagi = true; p.ShowNagi = false;
        p.UpdateCheckAtStart = false; p.UpdateCheckAtStart = true;
        p.RecordRecentLaunches = false;
        Equal(0u, (uint)store.Values[RecentLaunches.SwitchName], "D3: off is written as 0 (read by the UMDs)");
        p.RecordRecentLaunches = true;
        p.HiddenGames = new HashSet<string> { "a.exe" }; Check(p.HiddenGames.Contains("A.EXE"), "hidden games compare without case");
        p.HiddenGames = new HashSet<string>();
        p.LastSeenRelease = "1.0"; p.LastSeenRelease = null;
        Equal(0, store.Values.Count, "R4: on then off leaves no value");
        Check(store.Writes > 0, "R4: the changes were written");
        Equal(@"Software\amdgpu-wddm\Control", RegistryPrefStore.Path, "preferences live under HKCU Software\\amdgpu-wddm\\Control");
    }

    // G-ART: lowest rank wins, the listed order inside a rank, every pair of simultaneous causes.
    static void GuideVerdicts()
    {
        var all = Enum.GetValues(typeof(GuideCause)).Cast<GuideCause>().ToList();
        Equal(GuideCause.AllGood, Guide.Choose(null).Cause, "G-ART: nothing active is all good");
        foreach (var a in all)
            foreach (var b in all)
            {
                var v = Guide.Choose(new[] { a, b });
                var want = Guide.Rank(a) != Guide.Rank(b) ? (Guide.Rank(a) < Guide.Rank(b) ? a : b) : (a <= b ? a : b);
                if (v.Cause != want) Check(false, "G-ART: " + a + " + " + b + " gives " + v.Cause);
            }
        Check(true, "G-ART: every pair of causes decided");
        var expect = new Dictionary<int, string> { { 1, "03-surprised" }, { 2, "06-warning" }, { 3, "07-hopeful" }, { 4, "05-thinking" }, { 5, "04-wink" }, { 7, "02-curious" }, { 8, "09-thumbs-up" } };
        foreach (var c in all)
        {
            string e;
            if (expect.TryGetValue(Guide.Rank(c), out e)) Equal(e, Guide.Expression(c), "G-ART: expression of " + c);
            Check(Strings.Has(Guide.Choose(new[] { c }).TextId), "G-ART: text of " + c + " exists");
            Check(Guide.ArtFile.ContainsKey(Guide.Expression(c)), "G-ART: art mapped for " + c);
        }
        Equal("01-wave", Guide.Expression(GuideCause.Welcome), "welcome waves");
        Equal("08-giggle", Guide.Expression(GuideCause.About), "about giggles");
        Equal("02-curious", Guide.Expression(GuideCause.PageTip), "page tip is curious");
        Equal("cu-unknown-after-40", Guide.Id(GuideCause.CuUnknownAfter40), "kebab id");
        Equal(GuideCause.PendingRestart, Guide.Choose(new[] { GuideCause.AllGood, GuideCause.UpdateAvailable, GuideCause.PendingRestart, GuideCause.Welcome }).Cause, "rank 3 beats 6, 7 and 8");
        // Animation: never with ranks 1-3, reduced animations, Windows animations off, hidden window or no frames.
        Check(Guide.MayAnimate(true, false, true, 4, true, 12), "animates when everything allows it");
        Check(Guide.MayAnimate(true, false, null, 8, true, 12), "unknown Windows setting does not block");
        for (int r = 1; r <= 3; r++) Check(!Guide.MayAnimate(true, false, true, r, true, 12), "G-ART: no animation at rank " + r);
        Check(!Guide.MayAnimate(true, true, true, 8, true, 12), "G-ART: reduce animations");
        Check(!Guide.MayAnimate(true, false, false, 8, true, 12), "G-ART: Windows animations off");
        Check(!Guide.MayAnimate(true, false, true, 8, false, 12), "G-PERF: no frames while hidden or minimised");
        Check(!Guide.MayAnimate(false, false, true, 8, true, 12), "G-ART: Show Nagi unchecked");
        Check(!Guide.MayAnimate(true, false, true, 8, true, 1), "static fallback without a frame set");
        Equal(0, Guide.UsableFrames(new[] { true, false, true }), "a gap in the frames falls back to the static image");
        Equal(0, Guide.UsableFrames(Enumerable.Repeat(true, Guide.MaxFrames + 1).ToList()), "too many frames fall back");
        Equal(5, Guide.UsableFrames(Enumerable.Repeat(true, 5).ToList()), "a complete short set plays");
        Check(!Guide.ShowArt(false, true) && !Guide.ShowArt(true, false) && Guide.ShowArt(true, true), "G-ART: art only when checked and present");
        Check(!Guide.ShowUnaskedTip(false) && Guide.ShowUnaskedTip(true), "unasked tips only when checked");
        Equal("nagi.06-warning@128.png", Guide.Resource("06-warning", Guide.ArtSize(1.25f)), "resource name at 120 DPI");
        Equal(256, Guide.ArtSize(2f), "@256 at 200 %");
        Equal("nagi.01-wave-f03@256.png", Guide.FrameResource("01-wave", 3, 256), "frame resource name");
    }

    static Dictionary<string, object> Launch(string path, DateTime utc, string api = "D3D12", uint n = 1)
    {
        return new Dictionary<string, object> { { "Path", path }, { "Image", Path.GetFileName(path) }, { "LastLaunchUtc", (ulong)utc.ToFileTimeUtc() }, { "Api", api }, { "Launches", n } };
    }

    // A4: key derivation, strict parsing, newest first, prune keeps 50, clear under the mutex.
    static void RecentLaunchRecords()
    {
        Equal(RecentLaunches.KeyOf(@"C:\Games\A.exe"), RecentLaunches.KeyOf(@"\\?\c:\games\a.EXE"), "A4: \\\\?\\ prefix and case do not change the key");
        Equal(RecentLaunches.KeyOf(@"\\server\share\a.exe"), RecentLaunches.KeyOf(@"\\?\UNC\server\share\a.exe"), "A4: UNC prefix");
        Check(Regex.IsMatch(RecentLaunches.KeyOf("x"), "^[0-9a-f]{32}$"), "A4: 32 lower-case hex digits");
        var store = new MemoryRecentStore();
        var t0 = new DateTime(2026, 10, 1, 12, 0, 0, DateTimeKind.Utc);
        string p1 = @"D:\Games\Witcher 3\bin\witcher3.exe", p2 = @"E:\Other\witcher3.exe", p3 = @"D:\日本語\ゲーム.exe";
        store.Data[RecentLaunches.KeyOf(p1)] = Launch(p1, t0);
        store.Data[RecentLaunches.KeyOf(p2)] = Launch(p2, t0.AddHours(1));
        store.Data[RecentLaunches.KeyOf(p3)] = Launch(p3, t0.AddHours(2), "Vulkan");
        var wrongKey = Launch(p1, t0); store.Data["00000000000000000000000000000000"] = wrongKey;
        var badImage = Launch(p1, t0); badImage["Image"] = "other.exe"; store.Data[RecentLaunches.KeyOf(p1 + "x")] = badImage;
        var badApi = Launch(p2 + "y", t0); badApi["Api"] = "d3d9"; store.Data[RecentLaunches.KeyOf(p2 + "y")] = badApi;
        var badType = Launch(p2 + "z", t0); badType["Launches"] = 3; store.Data[RecentLaunches.KeyOf(p2 + "z")] = badType;
        var list = RecentLaunches.Read(store);
        Equal(3, list.Count, "A4: invalid entries are skipped");
        Equal(p3, list[0].Path, "A4: newest first (Unicode path kept)");
        Equal(2, list.Count(r => r.Image == "witcher3.exe"), "A4: duplicate base names stay two entries");
        for (int i = 0; i < 60; i++) { var p = @"C:\G\g" + i + ".exe"; store.Data[RecentLaunches.KeyOf(p)] = Launch(p, t0.AddMinutes(-i)); }
        store.Busy = true;
        Equal(-1, RecentLaunches.Prune(store), "A4: prune does nothing while the mutex is held");
        Equal(ClearResult.Busy, RecentLaunches.Clear(store), "A4: clear reports busy");
        Equal(0, store.Deleted, "A4: nothing deleted while busy");
        store.Busy = false;
        Equal(13, RecentLaunches.Prune(store), "A4: prune keeps the 50 newest valid entries");
        Equal(50, RecentLaunches.Read(store).Count, "A4: 50 left");
        Equal(ClearResult.Cleared, RecentLaunches.Clear(store), "A4: clear");
        Equal(0, store.Data.Count, "A4: clear removes every subkey, invalid ones too");
    }

    // WU-009, WU-071: names and common terms in every language, width and kana folding, Coming-later rows.
    static void Search()
    {
        Func<string, string, string> first = (q, lang) => { var h = SettingsSearch.Find(q, lang); return h.Count == 0 ? null : h[0].Entry.Id; };
        Equal("later.vsync", first("vsync", "en"), "VSync leads to its Coming-later row");
        Check(SettingsSearch.Find("vsync", "en")[0].Entry.ComingLater, "the VSync row is marked coming later");
        Equal("later.fps", first("FPS limit", "en"), "FPS limit leads to its Coming-later row");
        Equal("later.vsync", first("ＶＳＹＮＣ", "ja"), "full-width Latin folds to half width");
        Equal(SettingsSearch.Normalize("カタカナ"), SettingsSearch.Normalize("かたかな"), "hiragana folds to katakana");
        Equal(SettingsSearch.Normalize("ｶﾀｶﾅ"), SettingsSearch.Normalize("カタカナ"), "half-width katakana folds");
        Equal(SettingsSearch.Normalize("clock-auto"), SettingsSearch.Normalize("Clock Auto"), "spaces, hyphens and case are ignored");
        Check(SettingsSearch.Find("", "en").Count == 0, "an empty query finds nothing");
        foreach (var lang in Strings.Languages)
            foreach (var e in SettingsSearch.Index)
            {
                var name = Strings.In(lang, "search." + e.Id);
                var hits = SettingsSearch.Find(name, lang);
                if (!hits.Any(h => h.Entry.Id == e.Id)) Check(false, "WU-009: " + lang + " name of " + e.Id + " finds it");
            }
        Check(true, "WU-009: every entry is found by its own name in every language");
        Equal("graphics.cores", first(Strings.In("ja", "search.graphics.cores"), "ja"), "a Japanese name finds the CU setting");
        Equal("graphics.cores", first(Strings.In("ko", "search.graphics.cores"), "ko"), "a Korean name finds the CU setting");
        Equal("graphics.cores", first("graphics cores", "ko"), "the English name works in every language");
        foreach (var e in SettingsSearch.Index)
            Check(Array.IndexOf(new[] { "home", "games", "graphics", "display", "performance", "driver", "settings", "help" }, e.Page) >= 0, "search entry " + e.Id + " names a page");
    }

    // WU-036: a window outside every screen comes back fully visible; one that is visible stays.
    static void WindowFit()
    {
        var one = new List<Rectangle> { new Rectangle(0, 0, 1920, 1040) };
        var two = new List<Rectangle> { new Rectangle(0, 0, 1920, 1040), new Rectangle(1920, 0, 2560, 1400) };
        var min = new Size(900, 560);
        var w = new Rectangle(100, 100, 1240, 760);
        Equal(w, DisplayInfo.Fit(w, one, min, 30), "WU-036: a visible window stays");
        var lost = DisplayInfo.Fit(new Rectangle(3000, 200, 1240, 760), one, min, 30);
        Check(one[0].Contains(lost), "WU-036: a window on a removed monitor comes back: " + lost);
        var big = DisplayInfo.Fit(new Rectangle(-6000, 0, 4000, 3000), one, min, 30);
        Check(one[0].Contains(big) && big.Width >= min.Width, "WU-036: a lost window larger than the screen shrinks: " + big);
        var kept = new Rectangle(2200, 100, 1240, 760);
        Equal(kept, DisplayInfo.Fit(kept, two, min, 30), "WU-036: a window on the second monitor stays");
        var title = DisplayInfo.Fit(new Rectangle(100, -500, 1240, 760), one, min, 30);
        Check(title.Top >= 0, "WU-036: the title bar comes back on a screen");
        var none = DisplayInfo.Fit(w, new List<Rectangle>(), min, 30);
        Equal(w, none, "WU-036: no screen information: nothing moves");
    }

    // F-CACHE: the D3D12 engine cache counted from a directory; the other caches honestly unknown.
    static void Caches()
    {
        var dir = Path.Combine(Path.GetTempPath(), "amdgpu-wddm-cache-test-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        try
        {
            var rows = CacheInventory.Read(dir);
            Equal(CacheState.Empty, rows[0].State, "cache: an empty directory is empty");
            File.WriteAllBytes(Path.Combine(dir, "vkd3d-proton.witcher3.cache"), new byte[3000]);
            File.WriteAllBytes(Path.Combine(dir, "vkd3d-proton.witcher3.cache.driver"), new byte[1000]);
            File.WriteAllBytes(Path.Combine(dir, "vkd3d-proton.ascent.cache"), new byte[2000]);
            rows = CacheInventory.Read(dir);
            Equal(CacheState.Present, rows[0].State, "cache: present");
            Equal(6000L, rows[0].Bytes, "cache: bytes of every file");
            Equal(2, rows[0].Programs, "cache: one program per .cache file");
            Check(rows[0].Text.Contains("2"), "cache: the text names the program count");
            Equal(CacheState.Empty, CacheInventory.Read(Path.Combine(dir, "missing"))[0].State, "cache: a missing directory is empty");
            Check(rows.Any(r => r.Id == "d3d11" && r.State == CacheState.Unknown) && rows.Any(r => r.Id == "vulkan" && r.State == CacheState.Unknown), "cache: DXVK and Mesa caches are unknown, not empty");
            Check(rows.Any(r => r.Id == "windows" && r.State == CacheState.Windows), "cache: the Windows cache is a pointer");
            Equal("1.5 GB", CacheInventory.Size(3L << 29), "cache size text");
        }
        finally { Directory.Delete(dir, true); }
    }

    // WU-038: values only with their flag; power and fan honestly "No reading".
    static void SensorRows()
    {
        var none = Sensors.Rows(null, null);
        Check(none.All(r => r.NoReading && r.Value == Strings.T("perf.no-reading")), "sensors: no driver, no readings");
        Equal(string.Join(",", Sensors.Ids), string.Join(",", none.Select(r => r.Id)), "sensors: fixed rows");
        var d = new DpmState { Flags = DpmState.FlagTemperature | DpmState.FlagHwBusy | DpmState.FlagClock, TemperatureMc = 88000, BusyAvgPermille = 734, ObservedMHz = 1500, CurrentMHz = 1400 };
        var rows = Sensors.Rows(d, new VideoMemoryState { LocalResident = 3L << 30, Dedicated = 8L << 30 });
        Equal("73 %", rows.First(r => r.Id == "load").Value, "sensors: load from the average");
        Equal("hot", rows.First(r => r.Id == "temperature").Level, "sensors: 88 C is hot");
        Equal("1500 MHz", rows.First(r => r.Id == "clock").Value, "sensors: the observed clock when flagged");
        Check(rows.First(r => r.Id == "memory").Value.Contains("3.0") && rows.First(r => r.Id == "memory").Value.Contains("8.0"), "sensors: memory in GB");
        Check(rows.First(r => r.Id == "power").NoReading && rows.First(r => r.Id == "fan").NoReading, "sensors: power and fan have no reading");
        d.Flags = DpmState.FlagTemperature; d.TemperatureMc = 81000;
        rows = Sensors.Rows(d, null);
        Equal("warn", rows.First(r => r.Id == "temperature").Level, "sensors: 81 C warns");
        Check(rows.First(r => r.Id == "load").NoReading, "sensors: no busy flag, no load");
        Equal("1400 MHz", rows.First(r => r.Id == "clock").Value, "sensors: the current clock without the observed flag");
    }

    // G-PROF: partial groups and unknown names survive an edit; an edit that changes nothing writes nothing.
    static void GroupsAndOrigins()
    {
        var rt = GameGroups.Find("rt"); var present = GameGroups.Find("present"); var cpu = GameGroups.Find("cpu");
        string stored = "raytracing-tier,present-noprimary,x-future,release-two-phase-off";
        Equal(GroupState.Partial, GameGroups.State(present, stored), "G-PROF: one of two present switches is partial");
        Equal(GroupState.On, GameGroups.State(rt, stored), "G-PROF: rt on");
        Equal(ProfileWriteKind.None, GameGroups.Plan("g.exe", stored, new Dictionary<string, bool>()).Kind, "G-PROF: no change, no write");
        var w = GameGroups.Plan("g.exe", stored, new Dictionary<string, bool> { { "cpu", true } });
        Equal(ProfileWriteKind.Set, w.Kind, "G-PROF: a group change writes");
        Check(w.Value.Contains("present-noprimary") && !w.Value.Contains("present-cached"), "G-PROF: the untouched partial group keeps its exact tokens");
        Check(w.Value.Contains("x-future") && w.Value.Contains("release-two-phase-off"), "G-PROF: unknown and diagnostic names are kept");
        Check(w.Value.Contains("recording-bind") && w.Value.Contains("retire-handoff") && w.Value.Contains("deferred-replay"), "G-PROF: the changed group fully on");
        w = GameGroups.Plan("g.exe", "raytracing-tier", new Dictionary<string, bool> { { "rt", false } });
        Equal(ProfileWriteKind.Delete, w.Kind, "G-PROF: no names left removes the key (settings rule)");
        Equal(ProfileWriteKind.None, GameGroups.Plan("g.exe", "present-cached,present-noprimary", new Dictionary<string, bool> { { "present", true } }).Kind, "G-PROF: the same set in another order is no change");
        Throws<ArgumentException>(() => GameGroups.Apply("", new Dictionary<string, bool> { { "nope", true } }), "G-PROF: unknown group refused");
        // WU-015 origins.
        Equal(ValueOrigin.DriverDefault, GameGroups.Origin(rt, null, "raytracing-tier"), "origin: no key is the driver default");
        Equal(ValueOrigin.Recommended, GameGroups.Origin(rt, "raytracing-tier", "raytracing-tier"), "origin: as the installer recommends");
        Equal(ValueOrigin.ThisGame, GameGroups.Origin(cpu, "deferred-replay,recording-bind,retire-handoff", "raytracing-tier"), "origin: set for this game");
        Equal(ValueOrigin.DriverDefault, GameGroups.Origin(cpu, "raytracing-tier", "raytracing-tier"), "origin: off is the driver default");
        Check(GameGroups.Hidden(stored).SequenceEqual(new[] { "release-two-phase-off", "x-future" }), "G-PROF: the support-only names: " + string.Join(",", GameGroups.Hidden(stored)));
        foreach (var g in GameGroups.All) foreach (var t in g.Tokens) Check(Profiles.Find(t) != null, "group token " + t + " is a catalog switch");
        Check(Profiles.Catalog.All(c => GameGroups.All.Count(g => g.Tokens.Contains(c.Token)) == 1), "every catalog switch is in exactly one group");
        foreach (var g in GameGroups.All) Check(Strings.Has("game.group." + g.Id) && Strings.Has("game.group." + g.Id + ".cost") && Strings.Has("help.setting." + g.Id), "group " + g.Id + " has its texts");
    }

    // WU-060: hints are lines in the confirmation; nothing skips the confirmation.
    static void RestartHints()
    {
        var h = new ActivityHints(); h.RunningGames.Add("witcher3.exe"); h.Busy = BusyState.Fullscreen;
        var d = Hints.Decide(h);
        Check(d.Lines.Any(l => l.Contains("witcher3.exe")) && d.Lines.Count >= 2, "WU-060: a running game and the full-screen state are named");
        Check(d.NeedsConfirmation && d.OfferLater, "WU-060: a hint never authorizes; Later is offered");
        var clear = Hints.Decide(new ActivityHints { Busy = BusyState.Free });
        Check(clear.NeedsConfirmation && clear.OfferLater, "WU-060: a clear state still needs the confirmation");
        Check(Hints.Decide(new ActivityHints()).NeedsConfirmation, "WU-060: unknown state needs the confirmation");
    }

    // WU-007, WU-052, B5: the Home status card from snapshots.
    static void StatusCards()
    {
        var none = HomeStatus.Compute(new StatusInputs { Snapshot = new RecoverySnapshot() });
        Equal("problem", none.Severity, "Home: not installed is a problem");
        Check(none.Items[0].Text == Strings.T("status.not-installed"), "Home: not installed text");
        var s = WithDefaults();
        var ok = HomeStatus.Compute(new StatusInputs { Snapshot = s });
        Check(ok.Items.All(i => i.Severity != "problem"), "Home: a running driver has no problem item: " + string.Join(" / ", ok.Items.Select(i => i.Text)));
        s = WithDefaults(); s.Parameters["DpmMode"] = 1; s.Dpm.Mode = 0;
        var pending = HomeStatus.Compute(new StatusInputs { Snapshot = s });
        Check(pending.Pending.Contains(Strings.T("status.pending.clock")), "B5: a stored clock mode that differs from the running one waits for the restart");
        Check(pending.Items.Any(i => i.Cause == GuideCause.PendingRestart && i.Action == "restart"), "Home: pending restart offers the restart");
        var later = HomeStatus.Compute(new StatusInputs { Snapshot = s, LaterRestart = true });
        Check(later.Items.Any(i => i.Text.StartsWith(Strings.T("status.pending-later", "").Split(':')[0], StringComparison.Ordinal)), "Home: Later keeps it pending");
        s = WithDefaults(); s.Parameters["InteropClosedReason"] = 2;
        Check(HomeStatus.Compute(new StatusInputs { Snapshot = s }).Items.Any(i => i.Action == "reopen-gpu-path"), "Home: a closed GPU desktop path offers to reopen it");
        var upd = HomeStatus.Compute(new StatusInputs { Snapshot = WithDefaults(), UpdateAvailable = true, WorkInProgress = true });
        Check(upd.Items.Any(i => i.Action == "page:driver") && upd.Items.Any(i => i.Cause == GuideCause.WorkInProgress), "Home: update and work items");
        foreach (var c in new[] { none, ok, pending, upd })
            foreach (var i in c.Items)
            {
                Check(!i.Text.StartsWith("[", StringComparison.Ordinal), "Home: text exists for " + i.Text);
                if (i.Action != null) Check(!i.ActionLabel.StartsWith("[", StringComparison.Ordinal), "Home: label exists for " + i.Action);
            }
        Check(PlainWords.Findings(new[] { none, ok, pending, upd }.SelectMany(c => c.Items.Select(i => i.Text + " " + i.Next + " " + i.ActionLabel))).Count() == 0, "G-NOINT: the status card");
    }

    // G-PLAN / WU-053: every write of every plan has a plain sentence; the dialog is in plain words.
    static void PlainDialogs()
    {
        int plans = 0;
        var fixtures = new List<RecoverySnapshot> { WithDefaults() };
        var f = WithDefaults(); f.Parameters["InteropClosedReason"] = 2; f.Parameters["DpmMode"] = 0; f.Parameters["CuMode"] = 40; fixtures.Add(f);
        var more = new Dictionary<string, Recovery.PlanArgs>
        {
            { "cu-mode", new Recovery.PlanArgs { Cu = 40 } }, { "reset-defaults", new Recovery.PlanArgs { Games = "reset" } },
            { "game-profile", new Recovery.PlanArgs { Image = "witcher3.exe", Value = "raytracing-tier,deferred-replay" } },
        };
        foreach (var s in fixtures)
            foreach (var a in Recovery.Actions)
            {
                Recovery.PlanArgs m; more.TryGetValue(a, out m);
                var p = Recovery.Plan(a, s, a == "set-clocks" ? 1u : (uint?)null, a == "set-clocks" || a == "enable-dpm" ? 1800u : (uint?)null, new List<BackupRecord>(), false, m);
                if (p.Refused) { Check(!PlainPlan.Refusal(p).StartsWith("[", StringComparison.Ordinal), "WU-061: a refusal of " + a + " has a plain text"); continue; }
                plans++;
                foreach (var w in p.Writes) Check(PlainPlan.LineId(w) != null, "G-PLAN: " + a + " write " + w.Name + " has a plain sentence");
                var d = PlainPlan.Describe(p);
                Check(!d.Title.StartsWith("[", StringComparison.Ordinal) && d.Changes.Count > 0, "WU-053: " + a + " dialog has a title and changes");
                Check(d.Restart == p.OfferRestart, "WU-053: " + a + " says whether a restart is needed");
                foreach (var t in new[] { d.Title }.Concat(d.Changes).Concat(d.Notes))
                {
                    Check(!t.Contains("[plan."), "WU-053: " + a + " text exists: " + t);
                    foreach (var x in PlainWords.Findings(new[] { t })) Check(false, "G-NOINT: " + a + " dialog: " + x);
                }
            }
        Check(plans >= 8, "G-PLAN: plans described: " + plans);
        var game = PlainPlan.Describe(Recovery.Plan("game-profile", WithDefaults(), more: new Recovery.PlanArgs { Image = "witcher3.exe", Value = "" }));
        Check(game.Notes.Concat(game.Changes).Any(t => t.Contains("witcher3.exe")), "WU-054: the game dialog names the file-name scope");
    }

    // G-PERF: the poll runs only while visible, not minimised, on a page with live values.
    static void Polling()
    {
        Check(Sensors.PollWanted(true, false, "home") && Sensors.PollWanted(true, false, "performance"), "G-PERF: polls on Home and Performance");
        Check(!Sensors.PollWanted(false, false, "home"), "G-PERF: no poll while hidden");
        Check(!Sensors.PollWanted(true, true, "performance"), "G-PERF: no poll while minimised");
        Check(!Sensors.PollWanted(true, false, "games"), "G-PERF: no poll on pages without live values");
    }

    // G-NOINT over every table (search terms exempt: they are not shown), JA/KO Latin terms included.
    static void PlainWordsInTables()
    {
        int n = 0;
        foreach (var lang in Strings.Languages)
            foreach (var e in Strings.Tables[lang].Entries.Values)
            {
                if (e.Id.EndsWith(".terms", StringComparison.Ordinal)) continue;
                n++;
                foreach (var x in PlainWords.Findings(new[] { e.Text })) Check(false, "G-NOINT: " + lang + " " + e.Id + ": " + x);
                if (e.Text.Contains("\u2014")) Check(false, "style: em dash in " + lang + " " + e.Id);
            }
        Check(n > 1000, "G-NOINT: strings checked: " + n);
        Check(PlainWords.Findings(new[] { "The KMD fence 0x1F timed out" }).Any(), "G-NOINT: the check finds internals");
        Check(!PlainWords.Findings(new[] { "amdgpu-wddm Control 1.0.0.0-tester.11" }).Any(), "G-NOINT: the product name and a release version are fine");
    }

    // WU-063, WU-064: four symptom guides with steps, an explanation for every searchable setting and every page.
    static void HelpContent()
    {
        Equal(4, HelpGuides.Ids.Length, "WU-063: four symptom guides");
        foreach (var g in HelpGuides.Ids.Concat(HelpGuides.RecoveryIds).Distinct())
        {
            Check(Strings.Has("help.guide." + g), "WU-063: guide " + g + " has a title");
            Check(HelpGuides.Steps(g).Count >= 3, "WU-063: guide " + g + " has steps");
        }
        foreach (var e in SettingsSearch.Index)
            Check(Strings.Has("help.setting." + e.Id.Substring(e.Id.IndexOf('.') + 1)), "WU-064: " + e.Id + " has an explanation");
        foreach (var page in new[] { "home", "games", "graphics", "display", "performance", "driver", "settings", "help" })
            Check(Strings.Has("nav." + page) && Strings.Has("page." + page + ".title") && Strings.Has("page." + page + ".intro") && Strings.Has("help.page." + page), "WU-008: page " + page + " has its texts");
        Check(Strings.T("ui.later.reason").Contains("cannot") && !Regex.IsMatch(Strings.T("ui.later.reason"), @"(?i)phase|ph \d"), "WU-029: the reason is phase-free");
    }

    // G-SRC: what the code may not do, by a scan of the sources.
    static void SourceScan(string root)
    {
        var dir = Path.Combine(root, @"tools\win\amdgpu_wddm_control\src");
        var files = Directory.GetFiles(dir, "*.cs").ToDictionary(Path.GetFileName, File.ReadAllText);
        string[] window = files.Keys.Where(k => k.StartsWith("MainForm", StringComparison.Ordinal) || k == "Dialogs.cs" || k == "GuidePanel.cs" || k == "RecoveryView.cs" || k == "Ui.cs" || k == "Hints.cs" || k == "DisplayInfo.cs").ToArray();
        Check(window.Length >= 8, "G-SRC: window files found: " + window.Length);
        Func<string, string[], IEnumerable<string>> where = (pattern, except) => files.Where(kv => (except == null || !except.Contains(kv.Key)) && Regex.IsMatch(Regex.Replace(kv.Value, @"//[^\n]*", ""), pattern)).Select(kv => kv.Key);
        Equal("", string.Join(",", where(@"\b(WebRequest|HttpWebRequest|HttpClient|WebClient|TcpClient|UdpClient|Socket|ServicePointManager)\b", new[] { "UpdateCheck.cs" })), "G-SRC: network code only in UpdateCheck.cs");
        Equal("", string.Join(",", where(@"\b(ChangeDisplaySettings\w*|SetDisplayConfig)\b", null)), "G-SRC: no display-mode writes before phase 4");
        Equal("", string.Join(",", where(@"\b(TerminateProcess|taskkill)\b|\.Kill\(", new[] { "RecoveryActions.cs", "BugReport.cs" })), "G-SRC: no process kill outside the helper's accepted BD-060 escape and the report's own timed-out child");
        Equal("", string.Join(",", where(@"\b(SetupDiCallClassInstaller|DICS_DISABLE|DIF_PROPERTYCHANGE|CM_Disable_DevNode|pnputil|devcon)\b", null)), "G-SRC: no PnP disable or enable");
        Equal("", string.Join(",", where(@"\b(EWX_FORCE\w*|Restart-Computer|shutdown(\.exe)?\s+/r)\b", null)), "G-SRC: no forced restart");
        Equal("", string.Join(",", where(@"(?i)\b(oauth|access_token|Authorization)\b", null)), "G-SRC: no sign-in or token code");
        foreach (var f in window)
        {
            var text = Regex.Replace(files[f], @"//[^\n]*", "");
            foreach (Match m in Regex.Matches(text, @"\bKmd\.(\w+)\(([^)]*)\)"))
                Check(new[] { "Dpm", "Interop", "StartHealth", "CuMode", "VideoMemory" }.Contains(m.Groups[1].Value) && m.Groups[2].Value.Trim().Length == 0,
                    "G-SRC: " + f + " calls only Level-One reads: " + m.Value);
            Check(!Regex.IsMatch(text, @"Registry\.LocalMachine[^;]*(SetValue|DeleteValue|CreateSubKey|DeleteSubKey)|OpenSubKey\([^)]*,\s*true\)"), "G-SRC: " + f + " writes no HKLM value itself");
            Check(!text.Contains("Process.GetProcessesByName(\"dwm\")") && !Regex.IsMatch(text, @"(?i)""dwm(\.exe)?""\s*\)\s*\.\s*Kill"), "G-SRC: " + f + " does not touch DWM");
        }
        foreach (var kv in files)
            foreach (var line in kv.Value.Split('\n'))
                if (line.Contains("\"CuModePending\"") && Regex.IsMatch(line, @"SetDword|SetValue|Dword\(|DeleteValue|Remove\(")) Check(false, "G-SRC: " + kv.Key + " writes or deletes CuModePending: " + line.Trim());
        Check(!Regex.IsMatch(files["MainForm.cs"] + files["MainForm.Actions.cs"], @"restart-device|RestartDevice"), "G-SRC: no device restart from the window");
    }
}
