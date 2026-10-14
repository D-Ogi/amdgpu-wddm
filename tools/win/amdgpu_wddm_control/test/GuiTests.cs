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
        LaunchKindRules();
        Search();
        WindowFit();
        SmallScreens();
        PageWidthRule(root);
        RebuiltPageFocus(root);
        RebuiltPagePlace(root);
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
        Check(!p.ShowTipsAutomatically && !p.ReduceAnimations && !p.ShowSupportOptions && !p.GettingStartedDismissed, "R4: off by default");
        Check(p.UpdateCheckAtStart && p.RecordRecentLaunches, "R4: D2 and D3 are on while absent");
        Check(p.ShowNagi, "R4: the guide character is on while absent (owner, 2026-10-11)");
        Equal(0, store.Writes, "R4: reading writes nothing");
        p.ShowNagi = true; p.UpdateCheckAtStart = true; p.RecordRecentLaunches = true; p.HiddenGames = new HashSet<string>();
        Equal(0, store.Writes, "R4: setting a default writes nothing");
        p.ShowNagi = false;
        Equal(0u, (uint)store.Values["ShowNagi"], "R4: the guide character turned off is written as 0");
        Check(!p.ShowNagi, "R4: 0 reads as off");
        p.ShowNagi = true;
        p.UpdateCheckAtStart = false; p.UpdateCheckAtStart = true;
        p.RecordRecentLaunches = false;
        Equal(0u, (uint)store.Values[RecentLaunches.SwitchName], "D3: off is written as 0 (read by the shells)");
        p.RecordRecentLaunches = true;
        Equal(1u, (uint)store.Values[RecentLaunches.SwitchName], "D3: on again is written as 1");
        Equal(RecentSwitch.On, p.RecentLaunchesSwitch, "D3: 1 is on");
        foreach (var other in new object[] { 2u, 0xFFFFFFFFu, "1", new[] { "1" }, PrefValue.Unusable })
        {
            store.Values[RecentLaunches.SwitchName] = other;
            Equal(RecentSwitch.Invalid, p.RecentLaunchesSwitch, "D3: a value other than DWORD 0 or 1 is not valid (" + other + ")");
            Check(!p.RecordRecentLaunches, "D3: a value that is not valid counts as off (" + other + ")");
        }
        int before = store.Writes;
        p.RecordRecentLaunches = false;
        Check(store.Writes == before + 1 && (uint)store.Values[RecentLaunches.SwitchName] == 0u, "D3: turning off over a value that is not valid writes 0");
        p.RecordRecentLaunches = true;
        store.Values.Remove(RecentLaunches.SwitchName);
        p.HiddenGames = new HashSet<string> { "a.exe" }; Check(p.HiddenGames.Contains("A.EXE"), "hidden games compare without case");
        p.HiddenGames = new HashSet<string>();
        p.ShownGames = new HashSet<string> { "msedge.exe" };
        Check(p.ShownGames.Contains("MSEDGE.EXE") && store.Values["ShownGames"] is string[], "BD-140: entries shown anyway are a list of names (REG_MULTI_SZ) and compare without case");
        p.ShownGames = new HashSet<string>();
        p.LastSeenRelease = "1.0"; p.LastSeenRelease = null;
        p.NameTipRelease = "1.0"; Equal("1.0", p.NameTipRelease, "the name tip records the release it was shown for"); p.NameTipRelease = null;
        Equal(0, store.Values.Count, "R4: on then off leaves no value");
        Check(store.Writes > 0, "R4: the changes were written");
        Equal(@"Software\amdgpu-wddm\Control", RegistryPrefStore.Path, "preferences live under HKCU Software\\amdgpu-wddm\\Control");
    }

    // G-ART: lowest rank wins, the listed order inside a rank, every pair of simultaneous causes.
    static void GuideVerdicts()
    {
        var all = Enum.GetValues(typeof(GuideCause)).Cast<GuideCause>().ToList();
        Check(Guide.Choose(null) == null && Guide.Choose(new GuideCause[0]) == null, "G-ART: nothing active is no verdict, never an implicit all good (497.4)");
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
        Check(Guide.Rank(GuideCause.Installing) == 4 && Guide.Order(GuideCause.Installing) == 1 && Guide.Order(GuideCause.Repairing) == 2 && Guide.Order(GuideCause.CreatingReport) == 3,
            "G-ART: rank 4 is installing, repair, support report, in that order");
        Check(Guide.Order(GuideCause.DesktopReplaced) == 6 && Guide.Order(GuideCause.PageTip) == 3 && Guide.Order(GuideCause.AllGood) == 1, "G-ART: places inside a rank");
        // The name tip: once per installed release, never over a problem, an attention item or a pending restart.
        Check(Guide.NameTipDue("0.7.216.100-tester.30", null) && Guide.NameTipDue("0.7.216.100-tester.30", "0.7.216.100-tester.29v2"),
            "name tip: due on the first start after an install and after an update");
        Check(!Guide.NameTipDue("0.7.216.100-tester.30", "0.7.216.100-TESTER.30") && !Guide.NameTipDue(null, null) && !Guide.NameTipDue("", "x"),
            "name tip: not again for the same release, and never without an installed release");
        foreach (var c in all)
        {
            var v = Guide.Choose(new[] { c });
            Check(Guide.ShowNameTip(true, v) == (Guide.Rank(c) > 3) && !Guide.ShowNameTip(false, v), "name tip: over " + c + " only when its rank is above 3, and only while it shows");
        }
        Check(!Guide.ShowNameTip(true, null), "name tip: no verdict, no tip (the panel says the status is unreadable)");
        Equal(15, Guide.NameTipSeconds, "name tip: about 15 seconds");

        // Guide.Evaluate: the whole decision as a pure function.
        var on = new GuideContext { ShowNagi = true, ArtAvailable = true, FramesFound = 12, FramesComplete = true, WindowsAnimations = true };
        var none = Guide.Evaluate(new GuideCause[0], on);
        Check(none.Verdict == null && none.Figure == null && !none.Animate && none.MaxAnimationSeconds == 0, "G-ART: no status read, no verdict and no art (497.4)");
        var good = Guide.Evaluate(new[] { GuideCause.AllGood }, on);
        Check(good.Verdict.Cause == GuideCause.AllGood && good.Figure == "09-thumbs-up" && good.Animate && good.MaxAnimationSeconds == 2, "G-ART: an explicit all good may play once");
        var hidden = Guide.Evaluate(new[] { GuideCause.Repairing }, new GuideContext { ShowNagi = true, ArtAvailable = true, FramesFound = 12, FramesComplete = true, WindowVisible = false });
        Check(hidden.Figure == "05-thinking" && !hidden.PaintNow && !hidden.Animate, "G-PERF: hidden keeps the figure, paints nothing and plays nothing");
        Check(!Guide.Evaluate(new[] { GuideCause.AllGood }, new GuideContext { ShowNagi = true, ArtAvailable = true, FramesFound = 12, FramesComplete = true, Trigger = GuideTrigger.None }).Animate,
            "G-ART: no playback without a verdict change or the panel opening");
        Check(!Guide.Evaluate(new[] { GuideCause.AllGood }, new GuideContext { ShowNagi = true, ArtAvailable = true, FramesFound = Guide.MaxFrames + 1, FramesComplete = true }).Animate &&
            !Guide.Evaluate(new[] { GuideCause.AllGood }, new GuideContext { ShowNagi = true, ArtAvailable = true, FramesFound = 12, FramesComplete = false }).Animate,
            "G-ART: an oversized or failed frame set falls back to the static figure");
        var off = Guide.Evaluate(new[] { GuideCause.UpdateAvailable }, new GuideContext { ArtAvailable = true, FramesFound = 12, FramesComplete = true, ShowTipsAutomatically = true });
        Check(off.Verdict.Cause == GuideCause.UpdateAvailable && off.Figure == null && !off.Animate && off.AutomaticTips, "G-ART: Show Nagi off keeps the verdict, no art; tips are independent");
        Check(Guide.Evaluate(new[] { GuideCause.AllGood }, new GuideContext { ShowNagi = true, ArtAvailable = false }).Figure == null, "G-ART: missing art is the text-only panel");
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

    static string Entry(DateTime utc, uint pid, uint starts, uint apis, string path)
    {
        return utc.ToFileTimeUtc().ToString("X16") + "\t" + pid + "\t" + starts + "\t" + apis + "\t" + path;
    }

    static byte[] ListBytes(params string[] entries)
    {
        var text = RecentLaunches.Header + string.Concat(entries.Select(e => e + "\n")) + "end " + entries.Length + "\n";
        return new System.Text.UTF8Encoding(false).GetBytes(text);
    }

    static RecentListState StateOf(byte[] data) { return RecentLaunches.Parse(data).State; }

    // A4 on stream C's file contract (docs/design/recent-launches.md v1): strict whole-list parse in file order, up to
    // 64, the API bit, path identity without case, Clear under the lock with its bounded retry.
    static void RecentLaunchRecords()
    {
        var t0 = new DateTime(2026, 10, 1, 12, 0, 0, DateTimeKind.Utc);
        string p1 = @"D:\Games\Witcher 3\bin\witcher3.exe", p2 = @"E:\Other\witcher3.exe", p3 = @"D:\日本語\ゲーム.exe", p4 = @"\\server\share\🎮\play.exe";
        var good = ListBytes(Entry(t0, 10, 3, 3, p1), Entry(t0.AddHours(1), 11, 1, 2, p2), Entry(t0.AddHours(-1), 12, 4294967295, 0, p3), Entry(t0, 13, 1, 4, p4));
        var list = RecentLaunches.Parse(good);
        Equal(RecentListState.Valid, list.State, "A4: a valid list");
        Equal(4, list.Entries.Count, "A4: every entry");
        Check(list.Entries.Select(r => r.Path).SequenceEqual(new[] { p1, p2, p3, p4 }), "A4: the file's order is kept (launch order, not the clock); Unicode paths kept");
        Check(list.Entries[0].D3D12 && !list.Entries[1].D3D12 && !list.Entries[2].D3D12 && !list.Entries[3].D3D12, "A4: bit 1 is D3D12, everything else D3D11");
        Equal(t0, list.Entries[0].LastLaunchUtc, "A4: the start field is the FILETIME of the launch");
        Check(list.Entries[0].Pid == 10 && list.Entries[0].Launches == 3 && list.Entries[2].Launches == 4294967295u, "A4: pid and starts");
        Equal("ゲーム.exe", list.Entries[2].Image, "A4: the file name comes from the path");
        Equal(2, list.Entries.Count(r => r.Image == "witcher3.exe"), "A4: two folders with one file name stay two entries");
        int partial = 0;
        for (int n = 0; n < good.Length; n++) { var cut = new byte[n]; Array.Copy(good, cut, n); var r = RecentLaunches.Parse(cut); if (r.State == RecentListState.Valid || r.Entries.Count > 0) partial++; }
        Equal(0, partial, "A4: no truncation of a valid list is read as a list");
        var enc = new System.Text.UTF8Encoding(false);
        Func<string, RecentListState> text = s => StateOf(enc.GetBytes(s));
        var h = RecentLaunches.Header;
        Equal(RecentListState.Valid, text(h + "end 0\n"), "A4: an empty list");
        Equal(0, RecentLaunches.Parse(enc.GetBytes(h + "end 0\n")).Entries.Count, "A4: an empty list has no entries");
        Equal(RecentListState.Invalid, text(h + Entry(t0, 1, 1, 1, p1) + "\nend 2\n"), "A4: a count that does not match");
        Equal(RecentListState.Invalid, text(h + "end 0\nx"), "A4: bytes after the end line");
        Equal(RecentListState.Invalid, text(h + "end 0"), "A4: an end line without its line feed");
        Equal(RecentListState.Invalid, text(h + Entry(t0, 1, 1, 1, p1).ToLowerInvariant() + "\nend 1\n"), "A4: lower-case hexadecimal start");
        Equal(RecentListState.Invalid, text(h + "01DC0000000000\t1\t1\t1\tC:\\a.exe\nend 1\n"), "A4: a start field shorter than 16 digits");
        Equal(RecentListState.Invalid, text(h + "0000000000000001\t-1\t1\t1\tC:\\a.exe\nend 1\n"), "A4: a sign in a number field");
        Equal(RecentListState.Invalid, text(h + "0000000000000001\t4294967296\t1\t1\tC:\\a.exe\nend 1\n"), "A4: a pid above 32 bits");
        Equal(RecentListState.Invalid, text(h + "0000000000000001\t1\t1\t1\t\nend 1\n"), "A4: an empty path");
        Equal(RecentListState.Invalid, text(h + "0000000000000001\t1\t1\t1\tC:\\a\r.exe\nend 1\n"), "A4: a control character in the path");
        Equal(RecentListState.Invalid, text(h + "0000000000000001\t1\t1\t1\tC:\\a\tb.exe\nend 1\n"), "A4: a sixth field");
        Equal(RecentListState.Invalid, text(h + "0000000000000001\t1\t1\tC:\\a.exe\nend 1\n"), "A4: four fields");
        Equal(RecentListState.Invalid, text("\uFEFF" + h + "end 0\n"), "A4: a byte order mark");
        Equal(RecentListState.Invalid, text(h.Replace("\n", "\r\n") + "end 0\n"), "A4: CR LF line ends");
        Equal(RecentListState.OtherVersion, text("amdgpu-wddm recent-launches 2\nend 0\n"), "A4: another version is not read");
        Equal(RecentListState.Invalid, text("amdgpu-wddm recent-launches\nend 0\n"), "A4: a damaged header");
        Equal(RecentListState.Invalid, StateOf(new byte[] { 0x61, 0xFF, 0x0A }), "A4: bytes that are not UTF-8");
        Equal(RecentListState.Invalid, StateOf(ListBytes(Enumerable.Range(0, 65).Select(i => Entry(t0, (uint)i, 1, 1, @"C:\G\g" + i + ".exe")).ToArray())), "A4: 65 entries are not a list");
        Equal(64, RecentLaunches.Parse(ListBytes(Enumerable.Range(0, 64).Select(i => Entry(t0, (uint)i, 1, 1, @"C:\G\g" + i + ".exe")).ToArray())).Entries.Count, "A4: 64 entries are shown");
        Equal(RecentListState.Invalid, StateOf(new byte[RecentLaunches.MaxBytes + 1]), "A4: more than 8 MiB");
        Equal(RecentListState.Missing, RecentLaunches.Read(new MemoryRecentFiles()).State, "A4: a missing file is an empty list");
        Equal(RecentListState.Invalid, RecentLaunches.Read(new MemoryRecentFiles { List = good, Unreadable = true }).State, "A4: a list that cannot be read shows nothing");
        Check(RecentLaunches.SamePath(@"C:\Games\A.exe", @"c:\games\a.EXE") && !RecentLaunches.SamePath(@"C:\A\g.exe", @"C:\B\g.exe"), "A4: paths compare ordinally without case");

        var files = new MemoryRecentFiles { List = good, BusyAttempts = int.MaxValue };
        int slept = 0;
        Equal(ClearResult.Busy, RecentLaunches.Clear(files, ms => slept += ms), "A4: clear reports busy when the lock is not taken");
        Check(files.List != null && files.Deletes == 0, "A4: nothing deleted while busy");
        Equal(RecentLaunches.ClearWaitMs, slept, "A4: clear retries every 20 ms for 2 s");
        Equal(RecentLaunches.ClearWaitMs / RecentLaunches.ClearRetryMs + 1, files.Attempts, "A4: one attempt per retry");
        files = new MemoryRecentFiles { List = good, BusyAttempts = 3 };
        Equal(ClearResult.Cleared, RecentLaunches.Clear(files, ms => { }), "A4: clear once the lock is free");
        Check(files.List == null && files.Deletes == 1 && !files.Locked, "A4: the list is deleted under the lock and the lock released");

        var dir = Path.Combine(Path.GetTempPath(), "amdgpu-wddm-recent-test-" + Guid.NewGuid().ToString("N"));
        try
        {
            var local = new LocalRecentFiles(dir);
            Equal(RecentListState.Missing, RecentLaunches.Read(local).State, "A4: no directory is an empty list");
            Directory.CreateDirectory(dir);
            File.WriteAllBytes(Path.Combine(dir, RecentLaunches.ListName), good);
            File.WriteAllBytes(Path.Combine(dir, RecentLaunches.TempName), good);
            Equal(4, RecentLaunches.Read(local).Entries.Count, "A4: the file is read");
            using (var writer = new FileStream(Path.Combine(dir, RecentLaunches.LockName), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.ReadWrite | FileShare.Delete))
            {
                writer.Lock(0, 1);
                Equal(ClearResult.Busy, RecentLaunches.Clear(new LocalRecentFiles(dir), ms => { }), "A4: a writer holding byte 0 keeps the list");
                Check(File.Exists(Path.Combine(dir, RecentLaunches.ListName)), "A4: the list is still there");
                writer.Unlock(0, 1);
            }
            using (new FileStream(Path.Combine(dir, RecentLaunches.ListName), FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                Equal(ClearResult.Cleared, RecentLaunches.Clear(new LocalRecentFiles(dir), ms => { }), "A4: clear while a reader has the list open");
            Check(!File.Exists(Path.Combine(dir, RecentLaunches.ListName)) && !File.Exists(Path.Combine(dir, RecentLaunches.TempName)), "A4: list and temporary file deleted");
            Check(File.Exists(Path.Combine(dir, RecentLaunches.LockName)), "A4: the lock file is never deleted");
        }
        finally { try { Directory.Delete(dir, true); } catch (Exception) { } }
    }

    // BD-140: what a recorded launch is, from its path alone. Browsers, web views, parts of Windows and game stores are
    // not offered as games; a game in any folder stays a game; Hide and Show keep working on both kinds.
    static void LaunchKindRules()
    {
        const string win = @"C:\Windows";
        Equal(LaunchKind.Browser, LaunchKinds.Of(@"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe", win), "BD-140: Edge is a browser");
        Equal(LaunchKind.Browser, LaunchKinds.Of(@"C:\Program Files (x86)\Microsoft\EdgeWebView\Application\129.0.2792.89\msedgewebview2.exe", win),
            "BD-140: the WebView2 host (Widgets, Teams, the new Outlook) is a browser");
        Equal(LaunchKind.Browser, LaunchKinds.Of(@"D:\Tools\Portable\MSEDGE.EXE", win), "BD-140: the file name decides, in any folder and any case");
        Equal(LaunchKind.Windows, LaunchKinds.Of(@"C:\Program Files\WindowsApps\Microsoft.ScreenSketch_11.2409.25.0_x64__8wekyb3d8bbwe\SnippingTool\SnippingTool.exe", win),
            "BD-140: the Snipping Tool is part of Windows (by its package name)");
        Equal(LaunchKind.Windows, LaunchKinds.Of(@"E:\WindowsApps\Microsoft.Paint_11.2410.28.0_x64__8wekyb3d8bbwe\PaintApp\mspaint.exe", win),
            "BD-140: a Windows package moved to another drive is still part of Windows");
        Equal(LaunchKind.Windows, LaunchKinds.Of(@"C:\Windows\SystemApps\MicrosoftWindows.Client.CBS_cw5n1h2txyewy\SearchHost.exe", win), "BD-140: anything below the Windows folder");
        Equal(LaunchKind.Windows, LaunchKinds.Of(@"c:\windows\system32\dwm.exe", win + @"\"), "BD-140: the Windows folder compares without case and trailing separator");
        Equal(LaunchKind.Game, LaunchKinds.Of(@"C:\Windows2\game.exe", win), "BD-140: a folder whose name only starts like the Windows folder is not below it");
        Equal(LaunchKind.Game, LaunchKinds.Of(@"D:\SteamLibrary\steamapps\common\The Witcher 3\bin\x64_dx12\witcher3.exe", win), "BD-140: a game in a Steam library is a game");
        Equal(LaunchKind.Game, LaunchKinds.Of(@"C:\Games\Factorio\bin\x64\factorio.exe", win), @"BD-140: a game in C:\Games is a game");
        Equal(LaunchKind.Game, LaunchKinds.Of(@"\\nas\games\Quake II RTX\q2rtx.exe", win), "BD-140: a game on a network share is a game");
        Equal(LaunchKind.Game, LaunchKinds.Of(@"C:\Program Files\WindowsApps\Microsoft.MicrosoftSolitaireCollection_4.21.0.0_x64__8wekyb3d8bbwe\Solitaire.exe", win),
            "BD-140: a Microsoft game package this list does not name stays a game (the publisher does not decide)");
        Equal(LaunchKind.Game, LaunchKinds.Of(@"C:\XboxGames\Forza Horizon 5\Content\ForzaHorizon5.exe", win), "BD-140: an Xbox app game is a game");
        Equal(LaunchKind.Launcher, LaunchKinds.Of(@"C:\Program Files (x86)\Steam\steam.exe", win), "BD-140: the Steam client starts games, it is not one");
        Equal(LaunchKind.Browser, LaunchKinds.Of(@"C:\Program Files (x86)\Steam\bin\cef\cef.win7x64\steamwebhelper.exe", win), "BD-140: Steam's web pages are a browser");
        Equal(LaunchKind.OwnTool, LaunchKinds.Of(@"C:\Program Files\amdgpu-wddm\amdgpu_wddm_control.exe", win), "BD-140: this project's own programs are not games");
        Equal(LaunchKind.Game, LaunchKinds.Of("", win), "BD-140: no path, no verdict: a game");
        Equal(LaunchKind.Game, LaunchKinds.Of(@"C:\Games\x.exe", null), "BD-140: no Windows folder known, the other rules still apply");
        Equal(LaunchKind.Browser, LaunchKinds.Of(new RecentLaunch { Image = "msedge.exe", Path = @"C:\Program Files\Microsoft\Edge\Application\msedge.exe" }, win), "BD-140: a recorded launch by its path");
        Equal("Microsoft.ScreenSketch", LaunchKinds.PackageName(@"C:\Program Files\WindowsApps\Microsoft.ScreenSketch_11.2409.25.0_x64__8wekyb3d8bbwe\SnippingTool\SnippingTool.exe"), "BD-140: package name");
        Equal(null, LaunchKinds.PackageName(@"D:\Games\WindowsApps\MyGame\game.exe"), "BD-140: a folder that is not a package folder has no package name");

        var hidden = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var shown = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        Check(LaunchKinds.Hidden("msedge.exe", LaunchKind.Browser, hidden, shown) && !LaunchKinds.Hidden("witcher3.exe", LaunchKind.Game, hidden, shown),
            "BD-140: a browser is hidden by itself, a game is not");
        LaunchKinds.Toggle("msedge.exe", LaunchKind.Browser, true, hidden, shown);
        Check(!LaunchKinds.Hidden("MSEDGE.EXE", LaunchKind.Browser, hidden, shown) && shown.Contains("msedge.exe") && hidden.Count == 0, "BD-140: Show puts a browser among the games");
        LaunchKinds.Toggle("msedge.exe", LaunchKind.Browser, false, hidden, shown);
        Check(LaunchKinds.Hidden("msedge.exe", LaunchKind.Browser, hidden, shown) && shown.Count == 0 && hidden.Count == 0, "BD-140: Hide takes it out again and leaves no name behind");
        LaunchKinds.Toggle("witcher3.exe", LaunchKind.Game, false, hidden, shown);
        Check(LaunchKinds.Hidden("witcher3.exe", LaunchKind.Game, hidden, shown) && hidden.Contains("witcher3.exe") && shown.Count == 0, "BD-140: Hide on a game still hides it (WU-013)");
        LaunchKinds.Toggle("witcher3.exe", LaunchKind.Game, true, hidden, shown);
        Check(!LaunchKinds.Hidden("witcher3.exe", LaunchKind.Game, hidden, shown) && hidden.Count == 0 && shown.Count == 0, "BD-140: Show on a hidden game brings it back");
        hidden.Add("msedge.exe"); shown.Add("msedge.exe");
        Check(LaunchKinds.Hidden("msedge.exe", LaunchKind.Browser, hidden, shown), "BD-140: the person's Hide wins over an older Show");
        foreach (LaunchKind k in Enum.GetValues(typeof(LaunchKind)))
        {
            var id = LaunchKinds.TextId(k);
            Check(k == LaunchKind.Game ? id == null : Strings.Has(id), "BD-140: the line of " + k + " is in the string table");
        }
    }

    // WU-009, WU-071: names and common terms in every language, width and kana folding, Coming-later rows.
    static void Search()
    {
        Func<string, string, string> first = (q, lang) => { var h = SettingsSearch.Find(q, lang); return h.Count == 0 ? null : h[0].Entry.Id; };
        Equal("graphics.vsync", first("vsync", "en"), "VSync leads to its setting for all games");
        Check(!SettingsSearch.Find("vsync", "en")[0].Entry.ComingLater, "the VSync row is a setting now, not coming later");
        Equal("graphics.fps", first("FPS limit", "en"), "FPS limit leads to its setting");
        Equal("graphics.vsync", first("ＶＳＹＮＣ", "ja"), "full-width Latin folds to half width");
        Equal("later.hdr", first("HDR", "en"), "HDR leads to its Coming-later row");
        Check(SettingsSearch.Find("HDR", "en")[0].Entry.ComingLater, "the HDR row is marked coming later");
        Equal("games.sm", first("shader model", "en"), "the shader model is a game setting");
        Equal("display.scaling", first("GPU scaling", "en"), "GPU scaling is on the Display page");
        Equal("display.resolution", first("refresh rate", "en"), "the refresh rate is the resolution setting");
        Check(!SettingsSearch.Index.Any(x => x.Id.StartsWith("graphics.vk-", StringComparison.Ordinal)), "no search entry for a Vulkan setting that waits for its ICD");
        foreach (var e in SettingsSearch.Index.Where(x => x.Id.StartsWith("later.", StringComparison.Ordinal)))
            Check(!new[] { "vsync", "fps", "af", "scaling", "mode", "fps-counter" }.Contains(e.Id.Substring(6)), "no Coming-later row for a setting that exists: " + e.Id);
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
            Check(Array.IndexOf(new[] { "home", "games", "graphics", "display", "performance", "driver", "system", "settings", "help" }, e.Page) >= 0, "search entry " + e.Id + " names a page");
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

    // The owner's rule of 2026-10-12: on a 1024x768 or 800x600 working area, and on every smaller one, the guide
    // panel with the character and the whole right-hand panel it sits in do not show (GitHub #38: on the tester's
    // 800x600 screen the panel covered the text of the page). The screen comes in through the rule's arguments, so
    // these cases never read the screen this test runs on; the window's own side of it is a render finding
    // (--smoke-render G-SMALL, at the same five working areas).
    static void SmallScreens()
    {
        foreach (var c in new[] {
                new { W = 800, H = 600, Small = true },
                new { W = 1024, H = 768, Small = true },
                new { W = 1280, H = 720, Small = true },      // 720 high: under the 768 of the rule
                new { W = 1366, H = 768, Small = true },       // exactly 768 high
                new { W = 1920, H = 1080, Small = false },
                new { W = 640, H = 480, Small = true },
                new { W = 1024, H = 1200, Small = true },      // exactly 1024 wide
                new { W = 1025, H = 769, Small = false } })    // one pixel over the rule in both directions
            Check(LayoutRules.SmallScreen.Is(c.W, c.H) == c.Small,
                "small screen: " + c.W + "x" + c.H + " is " + (c.Small ? "small" : "not small"));
        Check(!LayoutRules.SmallScreen.Is(0, 0) && !LayoutRules.SmallScreen.Is(-1, 600) && !LayoutRules.SmallScreen.Is(800, 0),
            "small screen: a working area that is not known is not small, so the panel stays where it was");
        // Logical units, not device pixels: the same monitor is a small screen at a large text size.
        Check(!LayoutRules.SmallScreen.IsDevice(1920, 1032, 1f), "small screen: 1920x1032 at 100 % is not small");
        Check(LayoutRules.SmallScreen.IsDevice(1920, 1032, 1.5f), "small screen: 1920x1032 at 150 % is 1280x688 logical, which is small");
        Check(LayoutRules.SmallScreen.IsDevice(1600, 1200, 2f), "small screen: 1600x1200 at 200 % is 800x600 logical, which is small");
        Check(LayoutRules.SmallScreen.IsDevice(2048, 1536, 2f), "small screen: 2048x1536 at 200 % is 1024x768 logical, which is small");
        Check(!LayoutRules.SmallScreen.IsDevice(2100, 1600, 2f), "small screen: 2100x1600 at 200 % is 1050x800 logical, which is not small");
        Check(!LayoutRules.SmallScreen.IsDevice(1920, 1080, 0f), "small screen: a scale that is not known decides nothing");
        Equal(1024, LayoutRules.SmallScreen.MaxWidth, "small screen: the width of the rule the owner gave");
        Equal(768, LayoutRules.SmallScreen.MaxHeight, "small screen: the height of the rule the owner gave");
    }

    // G-RENDER: the page width rule of --smoke-render. A page is measured against the column it was built at, so that
    // a real overflow is still a finding and a client size clamped after the build is not (b24 lab round 3: on a
    // 1024x768 session-0 desktop the 1240 px client became 1028 px, the live column 449 px, and the correctly built
    // 661 px home page was reported as too wide).
    static void PageWidthRule(string root)
    {
        // The real overflow: a page wider than its own build column, slack included, always fails.
        var overflow = LayoutRules.PageWidthFinding("home", 700, 661, 2);
        Check(overflow != null, "G-RENDER: a page wider than its own build column fails");
        Check(overflow != null && overflow.Contains("home") && overflow.Contains("700") && overflow.Contains("661"),
            "G-RENDER: the finding names the page, its width and its build column: " + overflow);
        Check(LayoutRules.PageWidthFinding("graphics", 2000, 1339, 10) != null, "G-RENDER: a page far wider than its build column fails");
        Check(LayoutRules.PageWidthFinding("home", 664, 661, 2) != null, "G-RENDER: one px past the slack fails");
        // A page that fits passes, at the slack and below it.
        Equal(null, LayoutRules.PageWidthFinding("home", 661, 661, 2), "G-RENDER: a page as wide as its build column passes");
        Equal(null, LayoutRules.PageWidthFinding("home", 663, 661, 2), "G-RENDER: the slack of a fractional DPI scale passes");
        Equal(null, LayoutRules.PageWidthFinding("home", 400, 661, 2), "G-RENDER: a narrower page passes");
        // The regression itself: the page was built at 661 and the live column shrank to 449. The live column is not
        // an input of the rule any more, so a correctly built page gives no finding.
        Equal(null, LayoutRules.PageWidthFinding("home", 661, 661, 2), "G-RENDER: a client size clamped after the build is no finding");
        Check(LayoutRules.PageWidthFinding("home", 661, 449, 2) != null, "G-RENDER: the rule reads the build column, so 661 over 449 is still a finding");
        Throws<ArgumentOutOfRangeException>(() => LayoutRules.PageWidthFinding("home", 700, 661, -1), "G-RENDER: negative slack is refused");
        Throws<ArgumentNullException>(() => LayoutRules.PageWidthFinding(null, 700, 661, 2), "G-RENDER: a finding without a place is refused");
        // The window code asks the form for the build column, not for the width of this moment.
        var smoke = Regex.Replace(File.ReadAllText(Path.Combine(root, @"tools\win\amdgpu_wddm_control\src\MainForm.Smoke.cs")), @"//[^\n]*", "");
        var widthCall = smoke.Split('\n').FirstOrDefault(l => l.Contains("LayoutRules.PageWidthFinding"));
        Check(widthCall != null && widthCall.Contains("PageColumnWidth"),
            "G-RENDER: the render gate measures the page against PageColumnWidth: " + widthCall);
        Check(!Regex.IsMatch(smoke, @"built\.Width\s*>\s*ColumnWidth"), "G-RENDER: the render gate no longer measures the page against the live column width");
    }

    // F1 of the b23 lab round: where the keyboard focus goes when a card rebuilds itself under the person's hands.
    // The page below is the benchmark card as it stands while a run goes on: "Start the test" is disabled, so it
    // cannot take the focus back, and the focus must stay on that card instead of leaving for the navigation bar.
    static void RebuiltPageFocus(string root)
    {
        const string card = "perf.benchmark";
        var running = new List<FocusCandidate>
        {
            new FocusCandidate("Button|Home", null, true, true),                      // the navigation bar
            new FocusCandidate("Label|Ray tracing benchmark", card, false, false),
            new FocusCandidate("Button|Start the test", card, false, false),           // disabled while the run goes on
            new FocusCandidate("Button|Stop the test", card, true, true),
            new FocusCandidate("Button|Choose another folder...", card, false, false),
            new FocusCandidate("CardPanel|", card, true, false),                       // a panel: focusable, never a tab stop
        };
        Equal(3, PageFocus.After("Button|Start the test", card, running),
            "F1: the focus of a disabled Start goes to Stop the test, the next control of its own card");
        Equal(3, PageFocus.After("Button|Stop the test", card, running), "F1: a control that is still there takes the focus back");
        Equal(0, PageFocus.After("Button|Home", null, running), "F1: a control outside every card takes its own focus back");
        Equal(-1, PageFocus.After("Button|Start the test", null, running), "F1: without a card there is nothing to fall back to");
        Equal(-1, PageFocus.After(null, card, running), "F1: a page that had no focus asks for none");
        Equal(-1, PageFocus.After("Button|Start the test", "perf.fan", running), "F1: the fallback stays inside the card that had the focus");
        var gone = new List<FocusCandidate> { new FocusCandidate("Button|Home", null, true, true), new FocusCandidate("Label|Only text", card, false, false) };
        Equal(-1, PageFocus.After("Button|Start the test", card, gone), "F1: a card with nothing to focus hands the focus to nobody");
        Throws<ArgumentNullException>(() => PageFocus.After("Button|Home", null, null), "F1: a rule without a page is refused");

        // The window puts the focus back before it puts the place back: a control that takes the focus is scrolled
        // into view, which would move a view that had just been put back (the page then moved by its own offset).
        var form = Regex.Replace(File.ReadAllText(Path.Combine(root, @"tools\win\amdgpu_wddm_control\src\MainForm.cs")), @"//[^\n]*", "");
        int focusAt = form.IndexOf("KeepFocus(focusName", StringComparison.Ordinal);
        int scrollAt = form.IndexOf("PagePlace.Keep(scroll, cards, Cards())", StringComparison.Ordinal);
        Check(focusAt > 0 && scrollAt > 0 && focusAt < scrollAt, "F1: ShowPage puts the focus back before the place");
        Check(form.Contains("AutoScrollPosition = Point.Empty"), "F1: ShowPage builds the page with the panel at its top");
    }

    // F7: where a rebuilt page stands, and where it goes when a card must come into reach. The numbers are the ones
    // the lab measured: the case fan card above the benchmark card is 1287 px tall and gets a line of 23 px while
    // the game loads the chip, and the benchmark card sits at 1659 px in the page with its buttons at 1847 px.
    static void RebuiltPagePlace(string root)
    {
        var before = new List<CardBox>
        {
            new CardBox("perf.sensors", 70, 290),
            new CardBox("perf.fan", 372, 1287),
            new CardBox("perf.fan.chart", 800, 190),
            new CardBox("perf.benchmark", 1659, 514),
            new CardBox("perf.benchmark.buttons", 1847, 39),
        };
        var grown = new List<CardBox>
        {
            new CardBox("perf.sensors", 70, 290),
            new CardBox("perf.fan", 372, 1310),
            new CardBox("perf.fan.chart", 823, 190),
            new CardBox("perf.benchmark", 1682, 514),
            new CardBox("perf.benchmark.buttons", 1870, 39),
        };
        Equal(1682, PagePlace.Keep(1659, before, grown), "F7: a card that grows above the one in the view moves the page with it");
        Equal(1700, PagePlace.Keep(1677, before, grown), "F7: the card keeps its distance from the top of the view");
        Equal(1659, PagePlace.Keep(1659, before, before), "F7: a rebuild that changes no height keeps the offset");
        Equal(0, PagePlace.Keep(0, before, grown), "F7: a page at its top stays at its top");
        Equal(-200, PagePlace.Keep(-200, before, grown), "F7: an offset that is no offset is handed back as it is");
        // The outermost card wins: a page marks sections inside cards as well, and the chart inside the fan card
        // would keep a place of its own (it moved by 23 px, the fan card did not).
        Equal(850, PagePlace.Keep(850, before, grown), "F7: the card at the top of the view is the outermost one");
        // The top edge of the view in the gap between two cards: the first card below it keeps the place.
        var gap = new List<CardBox> { new CardBox("a", 0, 100), new CardBox("b", 200, 100) };
        Equal(180, PagePlace.Keep(150, gap, new List<CardBox> { new CardBox("a", 0, 100), new CardBox("b", 230, 100) }),
            "F7: in a gap between cards the card below keeps the place");
        Equal(0, PagePlace.Keep(150, gap, new List<CardBox> { new CardBox("a", 0, 100), new CardBox("b", 30, 100) }),
            "F7: the page never goes above its own top");
        Equal(150, PagePlace.Keep(150, gap, new List<CardBox> { new CardBox("a", 0, 100) }),
            "F7: a card that is gone from the rebuilt page leaves the offset alone");
        Throws<ArgumentNullException>(() => PagePlace.Keep(100, null, grown), "F7: a rule without the page before it is refused");
        Throws<ArgumentNullException>(() => PagePlace.Keep(100, before, null), "F7: a rule without the rebuilt page is refused");

        // Bringing the buttons of the card into reach: a page that shows them already does not move at all, which
        // is why pressing Start moves nothing under the pointer.
        Equal(1659, PagePlace.Bring(1659, 700, 1659, 1847, 39), "F7: the page stays where it is while the buttons are in the view");
        Equal(1682, PagePlace.Bring(1682, 700, 1659, 1870, 39), "F7: a card whose top edge is above the view stays there while its buttons are in reach");
        Equal(1659, PagePlace.Bring(0, 700, 1659, 1847, 39), "F7: a page that shows neither goes to the top of the card");
        Equal(1659, PagePlace.Bring(3000, 700, 1659, 1847, 39), "F7: the card comes back from below as well");
        Equal(1686, PagePlace.Bring(0, 200, 1659, 1847, 39), "F7: a view too short for the whole card shows the buttons");
        Equal(0, PagePlace.Bring(400, 700, -50, 10, 39), "F7: a card above the top of the page brings the page to its top");
        Throws<ArgumentOutOfRangeException>(() => PagePlace.Bring(0, 0, 100, 200, 39), "F7: a view with no height is refused");
        Throws<ArgumentOutOfRangeException>(() => PagePlace.Bring(0, 700, 100, 200, -1), "F7: a row shorter than nothing is refused");

        // The window asks the rule, and the card of a run that starts brings its own buttons into reach.
        var form = Regex.Replace(File.ReadAllText(Path.Combine(root, @"tools\win\amdgpu_wddm_control\src\MainForm.cs")), @"//[^\n]*", "");
        Check(form.Contains("PagePlace.Keep(scroll, cards, Cards())"), "F7: ShowPage puts the rebuilt page where the card the person reads is");
        Check(form.Contains("PagePlace.Bring("), "F7: a card is brought into reach by arithmetic the gates can measure");
        var bench = Regex.Replace(File.ReadAllText(Path.Combine(root, @"tools\win\amdgpu_wddm_control\src\MainForm.Bench.cs")), @"//[^\n]*", "");
        Check(bench.Contains("Mark(\"perf.benchmark.buttons\"") && bench.Contains("BringIntoView(\"perf.benchmark\", \"perf.benchmark.buttons\")"),
            "F7: Start brings the buttons of the card into reach, not the card alone");
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

    // WU-038: values only with their flag; power and fan honestly "No reading" without a fresh value.
    // Unit A's hardware monitor as E01 read it: one fan on channel 1 at 1589 rpm, duty read-back 961 permille.
    static HwmonState Fan(uint flags, uint rpm, uint dutyPermille)
    {
        var h = new HwmonState { Flags = flags, BasePort = 0x0A20, FanPresentMask = 0x1F, DutyPresentMask = 0x1F };
        h.Rpm[1] = rpm;
        for (int i = 0; i < 5; i++) h.DutyPermille[i] = dutyPermille;
        h.TemperatureSource[0] = HwmonState.SourceApu; h.TemperatureMc[0] = 83000;
        return h;
    }
    const uint FanLive = HwmonState.FlagValid | HwmonState.FlagFresh;

    static void SensorRows()
    {
        var none = Sensors.Rows(null, null, null);
        Check(none.All(r => r.NoReading && r.Value == Strings.T("perf.no-reading")), "sensors: no driver, no readings");
        Equal(string.Join(",", Sensors.Ids), string.Join(",", none.Select(r => r.Id)), "sensors: fixed rows");
        var d = new DpmState { Flags = DpmState.FlagTemperature | DpmState.FlagHwBusy | DpmState.FlagClock, TemperatureMc = 88000, BusyAvgPermille = 734, ObservedMHz = 1500, CurrentMHz = 1400 };
        var rows = Sensors.Rows(d, new VideoMemoryState { LocalResident = 3L << 30, Dedicated = 8L << 30 },
            Fan(FanLive | HwmonState.FlagDutyProven, 1589, 961));
        Equal("73 %", rows.First(r => r.Id == "load").Value, "sensors: load from the average");
        Equal("hot", rows.First(r => r.Id == "temperature").Level, "sensors: 88 C is hot");
        Equal("1500 MHz", rows.First(r => r.Id == "clock").Value, "sensors: the observed clock when flagged");
        Check(rows.First(r => r.Id == "memory").Value.Contains("3.0") && rows.First(r => r.Id == "memory").Value.Contains("8.0"), "sensors: memory in GB");
        Check(rows.First(r => r.Id == "power").NoReading, "sensors: no power flag, no power");
        Equal(Strings.T("perf.fan.value", 1589u, 96u), rows.First(r => r.Id == "fan").Value, "sensors: fan speed and proved duty");
        Check(!rows.First(r => r.Id == "fan").NoReading && rows.First(r => r.Id == "fan").Level == null, "sensors: a turning fan is plain");
        // Power (KMD 0.7.215, RUN_DPM ABI 3): only with the flag the driver sets for a table at most 3 s old.
        d.SocketPowerMw = 78400; d.Flags |= DpmState.FlagPower;
        rows = Sensors.Rows(d, null, null);
        Equal("78 W", rows.First(r => r.Id == "power").Value, "sensors: the package power from a fresh SMU table");
        Check(!rows.First(r => r.Id == "power").NoReading && rows.First(r => r.Id == "power").Level == null, "sensors: power is plain");
        d.Flags &= ~DpmState.FlagPower;
        Check(Sensors.Rows(d, null, null).First(r => r.Id == "power").NoReading, "sensors: a stale table is no reading, not the last value");
        d.Flags = DpmState.FlagTemperature | DpmState.FlagHwBusy | DpmState.FlagClock;
        d.Flags = DpmState.FlagTemperature; d.TemperatureMc = 81000;
        rows = Sensors.Rows(d, null, null);
        Equal("warn", rows.First(r => r.Id == "temperature").Level, "sensors: 81 C warns");
        Check(rows.First(r => r.Id == "load").NoReading, "sensors: no busy flag, no load");
        Equal("1400 MHz", rows.First(r => r.Id == "clock").Value, "sensors: the current clock without the observed flag");

        // The fan row, state by state. Anything short of "online and fresh" is "No reading", never a last-known
        // number: the owner reads this row to decide whether the fan turns right now.
        Func<HwmonState, SensorRow> fanRow = f => Sensors.Rows(d, null, f).First(r => r.Id == "fan");
        Check(fanRow(Fan(HwmonState.FlagGated, 0, 0)).NoReading, "sensors: the gate is closed by every install");
        Check(fanRow(Fan(HwmonState.FlagValid | HwmonState.FlagDutyProven, 1589, 961)).NoReading, "sensors: stale is not a reading");
        Check(fanRow(Fan(0, 0, 0)).NoReading, "sensors: an offline reader has no reading");
        // No lab trial has proved the duty read-back yet, so the speed is shown alone. At E01 the chip said 96 %
        // while the fan turned at about half of this board's measured full speed, which is why it is withheld.
        Equal(Strings.T("perf.fan.rpm-only", 1589u), fanRow(Fan(FanLive, 1589, 961)).Value, "sensors: unproven duty is withheld");
        // A duty output runs and nothing turns.
        SensorRow stopped = fanRow(Fan(FanLive | HwmonState.FlagDutyProven | HwmonState.FlagStopped, 0, 961));
        Equal(Strings.T("perf.fan.stopped"), stopped.Value, "sensors: a stopped fan says so");
        Equal("hot", stopped.Level, "sensors: a stopped fan is hot");
        Check(!stopped.NoReading, "sensors: a stopped fan is a reading, and a bad one");
    }

    // G-PROF: partial groups and unknown names survive an edit; an edit that changes nothing writes nothing.
    static void GroupsAndOrigins()
    {
        var rt = GameGroups.Find("rt"); var present = GameGroups.Find("present"); var cpu = GameGroups.Find("cpu");
        // The rt and cpu groups are Invert groups: the box is checked while the value names none of their off
        // switches, so "rt off" is the value that names raytracing-tier-off.
        string stored = "raytracing-tier-off,present-noprimary,x-future,release-two-phase-off";
        Equal(GroupState.Partial, GameGroups.State(present, stored), "G-PROF: one of two present switches is partial");
        Equal(GroupState.Off, GameGroups.State(rt, stored), "G-PROF: rt off, the value names its off switch");
        Equal(GroupState.On, GameGroups.State(rt, "present-cached"), "G-PROF: rt on without its off switch");
        Equal(GroupState.On, GameGroups.State(cpu, stored), "G-PROF: cpu on, the value names none of its off switches");
        Equal(ProfileWriteKind.None, GameGroups.Plan("g.exe", stored, new Dictionary<string, bool>()).Kind, "G-PROF: no change, no write");
        var w = GameGroups.Plan("g.exe", stored, new Dictionary<string, bool> { { "cpu", false } });
        Equal(ProfileWriteKind.Set, w.Kind, "G-PROF: a group change writes");
        Check(w.Value.Contains("present-noprimary") && !w.Value.Contains("present-cached"), "G-PROF: the untouched partial group keeps its exact tokens");
        Check(w.Value.Contains("x-future") && w.Value.Contains("release-two-phase-off"), "G-PROF: unknown and diagnostic names are kept");
        Check(w.Value.Contains("recording-bind-off") && w.Value.Contains("retire-handoff-off") && w.Value.Contains("deferred-replay-off")
            && w.Value.Contains("direct-entry-off"), "G-PROF: the changed group fully off");
        w = GameGroups.Plan("g.exe", "raytracing-tier-off", new Dictionary<string, bool> { { "rt", true } });
        Equal(ProfileWriteKind.Delete, w.Kind, "G-PROF: no names left removes the key (settings rule)");
        Equal(ProfileWriteKind.None, GameGroups.Plan("g.exe", "present-cached,present-noprimary", new Dictionary<string, bool> { { "present", true } }).Kind, "G-PROF: the same set in another order is no change");
        Throws<ArgumentException>(() => GameGroups.Apply("", new Dictionary<string, bool> { { "nope", true } }), "G-PROF: unknown group refused");
        // WU-015 origins.
        Equal(GroupState.On, GameGroups.Default(rt), "an Invert group's default state is on");
        Equal(GroupState.Off, GameGroups.Default(present), "an opt-in group's default state is off");
        Equal(ValueOrigin.DriverDefault, GameGroups.Origin(rt, null, "raytracing-tier-off"), "origin: no key is the driver default");
        Equal(ValueOrigin.Recommended, GameGroups.Origin(rt, "raytracing-tier-off", "raytracing-tier-off"), "origin: as the installer recommends");
        Equal(ValueOrigin.ThisGame, GameGroups.Origin(cpu, "deferred-replay-off,recording-bind-off,retire-handoff-off,direct-entry-off", "raytracing-tier-off"), "origin: set for this game");
        Equal(ValueOrigin.DriverDefault, GameGroups.Origin(cpu, "raytracing-tier-off", "raytracing-tier-off"), "origin: the group's default state is the driver default");
        Check(GameGroups.Hidden(stored).SequenceEqual(new[] { "release-two-phase-off", "x-future" }), "G-PROF: the support-only names: " + string.Join(",", GameGroups.Hidden(stored)));
        foreach (var g in GameGroups.All) foreach (var t in g.Tokens) Check(Profiles.Find(t) != null, "group token " + t + " is a catalog switch");
        Check(Profiles.Catalog.All(c => GameGroups.All.Count(g => g.Tokens.Contains(c.Token)) + (c.Token == ShaderModelCeiling.Off68 || c.Token == ShaderModelCeiling.Off67 ? 1 : 0) == 1),
            "every catalog switch is in exactly one group or the shader model choice");
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
        var upd = HomeStatus.Compute(new StatusInputs { Snapshot = WithDefaults(), UpdateAvailable = true, Work = GuideCause.CreatingReport });
        Check(upd.Items.Any(i => i.Action == "page:driver") && upd.Items.Any(i => i.Cause == GuideCause.CreatingReport), "Home: update and work items");
        var unread = HomeStatus.Compute(new StatusInputs { Snapshot = new RecoverySnapshot { DriverError = "unreadable", ReadFailed = true } });
        Check(!unread.StatusRead && !unread.Healthy && !unread.Causes.Any() && unread.Severity == "unknown" && unread.Items[0].Text == Strings.T("status.unreadable"),
            "497.4: a failed status read claims nothing: no cause, no all good");
        Check(!HomeStatus.Compute(new StatusInputs()).Healthy && !HomeStatus.Compute(new StatusInputs()).Causes.Any(), "497.4: no snapshot, no verdict");
        Check(ok.StatusRead && ok.Healthy == !ok.Causes.Any(c => Guide.Rank(c) <= 3) && !none.Healthy, "497.4: all good only after a read with nothing of rank 1-3");
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
        // The graphics settings: stored for all games and for witcher3, so reset-defaults and the game change carry them.
        foreach (var x in fixtures)
            x.GfxKeys = new List<GfxKey>
            {
                GKey(GraphicsSettings.GraphicsPath, 1, "FrameRateLimit", 60), GKey(GraphicsSettings.VulkanPath, 1, "WsiRoute", "gdi"),
                GKey(GraphicsSettings.AppPath(GraphicsSettings.GraphicsPath, "witcher3.exe"), 0, "VSync", 1),
            };
        var more = new Dictionary<string, Recovery.PlanArgs>
        {
            { "cu-mode", new Recovery.PlanArgs { Cu = 40 } }, { "reset-defaults", new Recovery.PlanArgs { Games = "reset" } },
            { "game-profile", new Recovery.PlanArgs { Image = "witcher3.exe", Value = "raytracing-tier-off,deferred-replay-off,shader-model-67-off", Gfx = "VSync=unset,Anisotropy=8,FrameRateLimit=144" } },
            { "tdr-delay", new Recovery.PlanArgs { Tdr = TdrSetting.ReleaseDefault } },
            { "graphics-defaults", new Recovery.PlanArgs { Gfx = "FrameRateLimit=unset,MaxFrameLatency=1,PerformanceOverlay=1,ReportAmdDriverVersion=1,VSync=0" } },
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
        Check(!PlainWords.Findings(new[] { "Nagi Control App 1.0.0.0-tester.11" }).Any(), "G-NOINT: the product name and a release version are fine");
        Check(!PlainWords.Findings(new[] { Strings.In("en", "help.about.text") }).Any(), "G-NOINT: the technical name Nagi (amdgpu-wddm) on About is fine");
        // The Nagi naming (owner, 2026-10-13): the window, the About card and the sign-in notice name the app Nagi
        // Control App, and the meaning of the name is one string that the name tip and the About line both read.
        foreach (var lang in Strings.Languages)
        {
            Check(Strings.In(lang, "notice.open-app").Contains("Nagi Control App") && Strings.In(lang, "help.guide.safe.3").Contains("Nagi Control App (recovery)"),
                "Nagi: " + lang + " names the app and its recovery shortcut as the Start menu shows them");
            Equal("Nagi's AMD Graphics Infrastructure", Strings.In(lang, "brand.expansion"), "Nagi: the meaning of the name is the same English phrase in " + lang);
            Check(Strings.In(lang, "guide.name").Contains("{0}") && Strings.In(lang, "help.about.name").Contains("{0}"), "Nagi: " + lang + " takes the phrase as the argument, not a copy of it");
            foreach (var t in Strings.Tables[lang].Entries.Values)
                if (t.Text.Contains("amdgpu-wddm Control")) Check(false, "Nagi: " + lang + " " + t.Id + " still names amdgpu-wddm Control");
        }
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
        foreach (var page in new[] { "home", "games", "graphics", "display", "performance", "driver", "system", "settings", "help" })
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
        // The guide character is a part of this application: every file her map names is in the repository at both
        // sizes and decodes as a picture of that size with an alpha channel, so a build never falls back to the
        // text-only panel without anyone noticing. A file that is present but truncated passed a check of its name
        // alone and then loaded as null in the window. build.ps1 also compares every file against the byte list of
        // the art's PROVENANCE.md and refuses a built exe whose resources are missing.
        var artDir = Path.Combine(root, @"tools\win\amdgpu_wddm_control\art\nagi");
        foreach (var file in Guide.ArtFile.Values.Distinct().OrderBy(f => f, StringComparer.Ordinal))
            foreach (var size in new[] { 128, 256 })
            {
                var path = Path.Combine(artDir, file + "@" + size + ".png");
                bool there = File.Exists(path);
                Check(there, "G-ART: the art file " + file + "@" + size + ".png is in the repository");
                if (!there) continue;
                string shape = "unreadable";
                try
                {
                    using (var s = File.OpenRead(path))
                    using (var img = Image.FromStream(s))
                        shape = img.Width + "x" + img.Height + (Image.IsAlphaPixelFormat(img.PixelFormat) ? " with alpha" : " without alpha");
                }
                catch (Exception e) { shape = e.GetType().Name; }
                Equal(size + "x" + size + " with alpha", shape, "G-ART: " + file + "@" + size + ".png decodes as a picture of its named size");
            }
        Func<string, string[], IEnumerable<string>> where = (pattern, except) => files.Where(kv => (except == null || !except.Contains(kv.Key)) && Regex.IsMatch(Regex.Replace(kv.Value, @"//[^\n]*", ""), pattern)).Select(kv => kv.Key);
        Equal("", string.Join(",", where(@"\b(WebRequest|HttpWebRequest|HttpClient|WebClient|TcpClient|UdpClient|Socket|ServicePointManager)\b", new[] { "UpdateCheck.cs" })), "G-SRC: network code only in UpdateCheck.cs");
        Equal("", string.Join(",", where(@"\b(ChangeDisplaySettings\w*|SetDisplayConfig)\b", new[] { "DisplayModes.cs" })), "G-SRC: display-mode writes only in DisplayModes.cs");
        Check(Regex.IsMatch(files["DisplayModes.cs"], @"CdsTest") && Regex.IsMatch(files["MainForm.Display.cs"], @"KeepDisplayDialog\.Ask"), "G-SRC: a mode is tested first and kept only on the person's word");
        // BenchmarkRun.cs ends one process: the game it started itself, and only when that game could not be put
        // into the job object that would otherwise have ended it (test/BenchmarkTests.cs decides that exact shape).
        Equal("", string.Join(",", where(@"\b(TerminateProcess|taskkill)\b|\.Kill\(", new[] { "RecoveryActions.cs", "BugReport.cs", "SystemTuningProcess.cs", "BenchmarkRun.cs", "DiagnosticWorker.cs" })), "G-SRC: no process kill outside the helper's accepted BD-060 escape, each helper's own timed-out child and the benchmark's own game");
        // A restart of our own graphics device is neither a disable nor an enable. The one click back from
        // Microsoft Basic Display after the driver's guard refused a start (RecoveryActions.EnableDriver) runs
        // pnputil /restart-device for that one device, the way the release installer already restarts the GPU's
        // audio function. Nothing else in this application may name a PnP tool, and that helper may not disable,
        // remove or enable any device.
        Equal("", string.Join(",", where(@"\b(SetupDiCallClassInstaller|DICS_DISABLE|DIF_PROPERTYCHANGE|CM_Disable_DevNode|pnputil|devcon)\b", new[] { "RecoveryActions.cs" })), "G-SRC: no PnP disable or enable");
        Check(Regex.IsMatch(files["RecoveryActions.cs"], @"/restart-device") && !Regex.IsMatch(files["RecoveryActions.cs"], @"/(remove|disable|enable|delete)-device"),
            "G-SRC: the helper restarts one device and never disables, removes or enables one");
        Equal("", string.Join(",", where(@"\b(EWX_FORCE\w*|Restart-Computer|shutdown(\.exe)?\s+/r)\b", null)), "G-SRC: no forced restart");
        Equal("", string.Join(",", where(@"(?i)\b(oauth|access_token|Authorization)\b", null)), "G-SRC: no sign-in or token code");
        foreach (var f in window)
        {
            var text = Regex.Replace(files[f], @"//[^\n]*", "");
            foreach (Match m in Regex.Matches(text, @"\bKmd\.(\w+)\(([^)]*)\)"))
                // Hwmon joined the list with the fan reader, Curve and Cpu with the Tuner: each reply is a
                // published snapshot answered with NoAdapterSynchronization alone, so none of them idles GPU
                // scheduling, and the fan reply touches no port of its own. Fan (the fan control's READ) is the same
                // kind of snapshot; its writes go through the elevated helper like every other change.
                Check(new[] { "Dpm", "Interop", "StartHealth", "CuMode", "VideoMemory", "Hwmon", "Curve", "Cpu", "Fan", "BoardMemoryQuery", "RefreshBoardCapabilities" }.Contains(m.Groups[1].Value) && m.Groups[2].Value.Trim().Length == 0,
                    "G-SRC: " + f + " calls only Level-One reads: " + m.Value);
            Check(!Regex.IsMatch(text, @"Registry\.LocalMachine[^;]*(SetValue|DeleteValue|CreateSubKey|DeleteSubKey)|OpenSubKey\([^)]*,\s*true\)"), "G-SRC: " + f + " writes no HKLM value itself");
            Check(!text.Contains("Process.GetProcessesByName(\"dwm\")") && !Regex.IsMatch(text, @"(?i)""dwm(\.exe)?""\s*\)\s*\.\s*Kill"), "G-SRC: " + f + " does not touch DWM");
        }
        foreach (var kv in files)
            foreach (var line in kv.Value.Split('\n'))
                if (line.Contains("\"CuModePending\"") && Regex.IsMatch(line, @"SetDword|SetValue|Dword\(|DeleteValue|Remove\(")) Check(false, "G-SRC: " + kv.Key + " writes or deletes CuModePending: " + line.Trim());
        Check(!Regex.IsMatch(files["MainForm.cs"] + files["MainForm.Actions.cs"], @"restart-device|RestartDevice"), "G-SRC: no device restart from the window");

        // G-SRC (BD-174): every background body is guarded. An exception that escapes a ThreadPool work item or a
        // plain thread ends the process at once, and on a machine with error reporting switched off it writes no
        // event anywhere: that is how the window vanished between two clicks with nothing to read afterwards. So
        // the body of every QueueUserWorkItem, Task.Run, StartNew and new Thread of this application runs through
        // Crash.Guard (nobody waits for it: recorded, reported to the person, swallowed, the window lives) or
        // Crash.Watched (a task waits for it: recorded and thrown on, and the waiter's continuation reports it).
        // Crash.cs itself holds the one guarded work item that the rest of the application goes through.
        // The check itself, on two bodies of its own: it sees the guard in one, refuses the other, and stops at the
        // bracket that closes the call. A bracket inside a string is no bracket.
        const string guardedBody = "Task.Run(() => Crash.Watched(\"x\", () => F(\"a)b\"))).ContinueWith(t => G());";
        const string bareBody = "Task.Run(() => F(\"a)b\")).ContinueWith(t => G());";
        Check(CallBody(guardedBody, guardedBody.IndexOf('(')).Contains("Crash.Watched("), "G-SRC: the guard check reads a guarded body as guarded");
        Check(!CallBody(bareBody, bareBody.IndexOf('(')).Contains("Crash."), "G-SRC: the guard check refuses a body with no guard");
        Check(!CallBody(guardedBody, guardedBody.IndexOf('(')).Contains("ContinueWith"), "G-SRC: the guard check stops at the bracket that closes the call");
        int guarded = 0;
        foreach (var kv in files.OrderBy(k => k.Key, StringComparer.Ordinal))
        {
            if (kv.Key == "Crash.cs") continue;
            var text = Regex.Replace(kv.Value, @"//[^\n]*", "");
            foreach (Match m in Regex.Matches(text, @"\b(QueueUserWorkItem|Task\.Run|Task\.Factory\.StartNew|new\s+Thread)\s*\("))
            {
                guarded++;
                var body = CallBody(text, text.IndexOf('(', m.Index));
                Check(body.Contains("Crash.Guard(") || body.Contains("Crash.Watched("),
                    "G-SRC: " + kv.Key + " line " + (text.Substring(0, m.Index).Count(c => c == '\n') + 1) +
                    ": the background body of " + Regex.Replace(m.Groups[1].Value, @"\s+", " ") + " runs through Crash.Guard or Crash.Watched");
            }
        }
        Check(guarded >= 12, "G-SRC: background bodies found and held to a guard: " + guarded);
    }

    // The text of a call, from its opening bracket to the bracket that closes it, brackets inside strings and
    // characters left out. A lambda given to the call is a part of that text, which is what the guard check reads.
    static string CallBody(string text, int open)
    {
        int depth = 0;
        for (int i = open; i < text.Length; i++)
        {
            char c = text[i];
            if (c == '"' || c == '\'')
            {
                char quote = c;
                for (i++; i < text.Length && text[i] != quote; i++) if (text[i] == '\\') i++;
                continue;
            }
            if (c == '(') depth++;
            else if (c == ')' && --depth == 0) return text.Substring(open, i - open + 1);
        }
        return text.Substring(open);
    }
}
