// Host tests of the ray tracing benchmark card (src/Benchmark.cs): Valve's key-value text, the PAK directory, what
// makes a copy of the game usable, the command line of one pass, the result line of the game's log in several
// cultures of the host, the thermal stop and the history file. No game, no driver, no process.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using AmdgpuWddmControl;

static partial class UnitTests
{
    sealed class MemoryBenchFiles : IBenchFiles
    {
        public readonly Dictionary<string, byte[]> Files = new Dictionary<string, byte[]>(StringComparer.OrdinalIgnoreCase);
        public readonly HashSet<string> Folders = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        public readonly List<string> Reads = new List<string>();

        public MemoryBenchFiles Text(string path, string body) { Files[path] = new UTF8Encoding(false).GetBytes(body); Folder(Path.GetDirectoryName(path)); return this; }
        public MemoryBenchFiles Bytes(string path, byte[] body) { Files[path] = body; Folder(Path.GetDirectoryName(path)); return this; }

        // The folder and every folder above it, which is what a disk would say about such a path.
        public MemoryBenchFiles Folder(string path)
        {
            for (var at = path; !string.IsNullOrEmpty(at); )
            {
                var clean = at.TrimEnd('\\');
                if (clean.Length == 0 || !Folders.Add(clean)) break;        // a root, or a folder already known
                var up = Path.GetDirectoryName(clean);
                if (string.IsNullOrEmpty(up) || string.Equals(up.TrimEnd('\\'), clean, StringComparison.OrdinalIgnoreCase)) break;
                at = up;
            }
            return this;
        }

        public bool DirectoryExists(string path) { return path != null && Folders.Contains(path.TrimEnd('\\')); }
        public bool FileExists(string path) { Reads.Add(path); return path != null && Files.ContainsKey(path); }

        public Stream OpenRead(string path)
        {
            Reads.Add(path);
            byte[] b;
            return Files.TryGetValue(path ?? "", out b) ? new MemoryStream(b, false) : null;
        }

        public string ReadText(string path, int maxBytes)
        {
            Reads.Add(path);
            byte[] b;
            if (!Files.TryGetValue(path ?? "", out b) || b.Length > maxBytes) return null;
            return new UTF8Encoding(false).GetString(b);
        }
    }

    // A game data file that is there but cannot be opened: a locked, truncated or foreign pak0.pak.
    sealed class UnreadableFile : IBenchFiles
    {
        readonly IBenchFiles _inner;
        readonly string _path;

        public UnreadableFile(IBenchFiles inner, string path) { _inner = inner; _path = path; }

        public bool DirectoryExists(string path) { return _inner.DirectoryExists(path); }
        public bool FileExists(string path) { return _inner.FileExists(path); }
        public Stream OpenRead(string path) { return string.Equals(path, _path, StringComparison.OrdinalIgnoreCase) ? null : _inner.OpenRead(path); }
        public string ReadText(string path, int maxBytes) { return _inner.ReadText(path, maxBytes); }
    }

    // A PAK with the named entries, as Quake II RTX writes and reads one.
    static byte[] PakBytes(params string[] names)
    {
        int dirlen = names.Length * Pak.EntrySize;
        var b = new byte[12 + dirlen];
        Encoding.ASCII.GetBytes("PACK").CopyTo(b, 0);
        BitConverter.GetBytes(12).CopyTo(b, 4);
        BitConverter.GetBytes(dirlen).CopyTo(b, 8);
        for (int i = 0; i < names.Length; i++)
        {
            var name = Encoding.ASCII.GetBytes(names[i]);
            Array.Copy(name, 0, b, 12 + i * Pak.EntrySize, Math.Min(name.Length, Pak.NameSize));
            BitConverter.GetBytes(12).CopyTo(b, 12 + i * Pak.EntrySize + Pak.NameSize);
            BitConverter.GetBytes(0).CopyTo(b, 12 + i * Pak.EntrySize + Pak.NameSize + 4);
        }
        return b;
    }

    static void BenchmarkTests(string root)
    {
        BenchVdf();
        BenchPak();
        BenchInstall();
        BenchSteam();
        BenchCommandLine();
        BenchResultLine();
        BenchThermal();
        BenchCardLines();
        BenchHistoryFile();
        BenchSourceScan(root);
    }

    // Valve's key-value text, in both shapes libraryfolders.vdf has had, and every way it can be damaged.
    static void BenchVdf()
    {
        var modern = "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"C:\\\\Program Files (x86)\\\\Steam\"\n\t\t\"label\"\t\t\"\"\n\t\t\"apps\"\n\t\t{\n\t\t\t\"1089130\"\t\"3041986285\"\n\t\t}\n\t}\n\t\"1\"\n\t{\n\t\t\"path\"\t\t\"D:\\\\SteamLibrary\"\n\t}\n}\n";
        var tree = Vdf.Parse(modern);
        Check(tree != null, "benchmark: the library file parses");
        Equal(@"C:\Program Files (x86)\Steam", tree.Text("libraryfolders", "0", "path"), "benchmark: a path with an escaped separator");
        Equal(@"D:\SteamLibrary", tree.Text("libraryfolders", "1", "path"), "benchmark: the second library");
        Equal("3041986285", tree.Text("libraryfolders", "0", "apps", "1089130"), "benchmark: a nested block is read");
        Equal("", tree.Text("libraryfolders", "0", "label"), "benchmark: an empty value is a value");
        Equal(null, tree.Text("libraryfolders", "0"), "benchmark: a block has no text of its own");
        Equal(null, tree.Text("libraryfolders", "2", "path"), "benchmark: an absent key is absent");
        Check(tree.Child("LIBRARYFOLDERS") != null, "benchmark: keys compare without case");
        Check(tree.Child("libraryfolders").Order.SequenceEqual(new[] { "0", "1" }), "benchmark: the file's order of the libraries is kept");

        var old = "\"LibraryFolders\"\n{\n\t\"TimeNextStatsReport\"\t\"1\"\n\t\"1\"\t\t\"E:\\\\Games\\\\Steam\"\n}\n";
        Equal(@"E:\Games\Steam", Vdf.Parse(old).Text("LibraryFolders", "1"), "benchmark: the old shape names the library as a value");

        var comments = "// a comment\n\"a\"\n{\n\t// another\n\t\"b\" \"c\" // trailing\n}\n";
        Equal("c", Vdf.Parse(comments).Text("a", "b"), "benchmark: comments are skipped");
        Equal("line\nbreak\ttab", Vdf.Parse("\"a\" \"line\\nbreak\\ttab\"").Text("a"), "benchmark: the escapes of a value");
        Equal("a\"b", Vdf.Parse("\"k\" \"a\\\"b\"").Text("k"), "benchmark: an escaped quotation mark stays in the value");
        Equal("bare", Vdf.Parse("key bare").Text("key"), "benchmark: an unquoted token is read");
        Equal("last", Vdf.Parse("\"k\" \"first\"\n\"k\" \"last\"").Text("k"), "benchmark: a repeated key keeps the last value");

        Equal(null, Vdf.Parse(null), "benchmark: no text is not a tree");
        Equal(null, Vdf.Parse("\"a\"\n{\n\"b\" \"c\"\n"), "benchmark: a block that never closes is refused");
        Equal(null, Vdf.Parse("}"), "benchmark: a closing brace at the top is refused");
        Equal(null, Vdf.Parse("\"a\""), "benchmark: a key without a value is refused");
        Equal(null, Vdf.Parse("\"a\" \"unterminated"), "benchmark: an unterminated value is refused");
        Equal(null, Vdf.Parse("\"a\" \"two\nlines\""), "benchmark: a line break inside a value is refused");
        Equal(null, Vdf.Parse(new string('x', Vdf.MaxBytes + 1)), "benchmark: a file above the bound is refused");
        Equal(null, Vdf.Parse(string.Concat(Enumerable.Repeat("\"a\"\n{\n", Vdf.MaxDepth + 2)) + string.Concat(Enumerable.Repeat("}\n", Vdf.MaxDepth + 2))),
            "benchmark: a tree deeper than the bound is refused");
        Equal(null, Vdf.Parse(string.Concat(Enumerable.Range(0, Vdf.MaxNodes + 1).Select(i => "\"k" + i + "\" \"v\"\n"))), "benchmark: more nodes than the bound is refused");
    }

    // The PAK directory: the test scene is there, or it honestly is not.
    static void BenchPak()
    {
        using (var s = new MemoryStream(PakBytes("demos/q2demo1.dm2", "maps/demo1.bsp")))
            Check(Pak.Has(s, Q2Rtx.DemoEntry), "benchmark: the test scene is found in the directory");
        using (var s = new MemoryStream(PakBytes("maps/demo1.bsp")))
            Check(!Pak.Has(s, Q2Rtx.DemoEntry), "benchmark: a game data file without the test scene says so");
        using (var s = new MemoryStream(PakBytes("DEMOS\\Q2DEMO1.DM2")))
            Check(Pak.Has(s, Q2Rtx.DemoEntry), "benchmark: a back slash and upper case name the same entry");
        using (var s = new MemoryStream(PakBytes("demos/q2demo1.dm2")))
            Check(Pak.Entries(s).Count == 1, "benchmark: one entry, one name");
        Equal(null, Pak.Entries(null), "benchmark: no stream is not a game data file");
        using (var s = new MemoryStream(Encoding.ASCII.GetBytes("NOPEnot a pak at all")))
            Equal(null, Pak.Entries(s), "benchmark: another file is not a game data file");
        var torn = PakBytes("demos/q2demo1.dm2");
        using (var s = new MemoryStream(torn, 0, 40))
            Equal(null, Pak.Entries(s), "benchmark: a truncated game data file is refused, never read in part");
        var odd = PakBytes("demos/q2demo1.dm2");
        BitConverter.GetBytes(Pak.EntrySize + 1).CopyTo(odd, 8);
        using (var s = new MemoryStream(odd))
            Equal(null, Pak.Entries(s), "benchmark: a directory length that is not a whole number of entries is refused");
        var huge = PakBytes("demos/q2demo1.dm2");
        BitConverter.GetBytes((Pak.MaxEntries + 1) * Pak.EntrySize).CopyTo(huge, 8);
        using (var s = new MemoryStream(huge))
            Equal(null, Pak.Entries(s), "benchmark: more entries than the bound is refused");
        var inside = PakBytes("demos/q2demo1.dm2");
        BitConverter.GetBytes(4).CopyTo(inside, 4);
        using (var s = new MemoryStream(inside))
            Equal(null, Pak.Entries(s), "benchmark: a directory inside the header is refused");
    }

    const string Game = @"D:\Games\Quake II RTX";

    static MemoryBenchFiles ReadyGame()
    {
        return new MemoryBenchFiles()
            .Text(Path.Combine(Game, Q2Rtx.ExeName), "MZ")
            .Bytes(Path.Combine(Game, Q2Rtx.DataFolder, Q2Rtx.PakName), PakBytes("demos/q2demo1.dm2", "maps/demo1.bsp"));
    }

    // Every verdict the card can show about a folder, and the one it acts on.
    static void BenchInstall()
    {
        var files = ReadyGame();
        var ok = Q2Rtx.Inspect(Game, files);
        Equal(InstallVerdict.Ready, ok.Verdict, "benchmark: a whole copy of the game is ready");
        Check(ok.Ready && ok.ExePath == Path.Combine(Game, Q2Rtx.ExeName), "benchmark: the program of a ready copy");
        Equal(InstallVerdict.NotFound, Q2Rtx.Inspect(null, files).Verdict, "benchmark: no folder chosen is not found");
        Equal(InstallVerdict.NotFound, Q2Rtx.Inspect("", files).Verdict, "benchmark: an empty folder name is not found");
        Equal(InstallVerdict.NoFolder, Q2Rtx.Inspect(@"D:\Gone", files).Verdict, "benchmark: a folder that is not there");
        var noExe = new MemoryBenchFiles().Folder(Game);
        Equal(InstallVerdict.NoProgram, Q2Rtx.Inspect(Game, noExe).Verdict, "benchmark: a folder without the game program");
        var noPak = new MemoryBenchFiles().Text(Path.Combine(Game, Q2Rtx.ExeName), "MZ");
        Equal(InstallVerdict.NoData, Q2Rtx.Inspect(Game, noPak).Verdict, "benchmark: a program without the game data");
        var noDemo = new MemoryBenchFiles()
            .Text(Path.Combine(Game, Q2Rtx.ExeName), "MZ")
            .Bytes(Path.Combine(Game, Q2Rtx.DataFolder, Q2Rtx.PakName), PakBytes("maps/demo1.bsp"));
        Equal(InstallVerdict.NoDemo, Q2Rtx.Inspect(Game, noDemo).Verdict, "benchmark: game data without the test scene cannot run the test");
        // Game data that cannot be read is not game data without the test scene: a locked or damaged file must never
        // make the card tell the person their copy of the game lacks the demo.
        var locked = new UnreadableFile(ReadyGame(), Path.Combine(Game, Q2Rtx.DataFolder, Q2Rtx.PakName));
        Equal(InstallVerdict.NoData, Q2Rtx.Inspect(Game, locked).Verdict, "benchmark: game data that cannot be read is not a missing test scene");
        Check(!Q2Rtx.Inspect(Game, noDemo).Ready, "benchmark: only a ready copy may be measured");
        Equal(InstallVerdict.NotFound, Q2Rtx.Inspect(Game, null).Verdict, "benchmark: without a way to read files nothing is found");
        Check(Q2Rtx.AllowedLink(Q2Rtx.StoreUrl) && Q2Rtx.AllowedLink(Q2Rtx.ReleasesUrl), "benchmark: the two pages of the game may be opened");
        Check(!Q2Rtx.AllowedLink("https://example.com/") && !Q2Rtx.AllowedLink(null), "benchmark: no other address may be opened");
        Check(Q2Rtx.StoreUrl.Contains("1089130") && Q2Rtx.SteamAppId == "1089130", "benchmark: the Steam application is 1089130");
    }

    // The Steam walk: the install, every library of the file, the manifest of this application only.
    static void BenchSteam()
    {
        const string steam = @"C:\Program Files (x86)\Steam", second = @"D:\SteamLibrary";
        var files = ReadyGame()
            .Folder(steam)
            .Text(Path.Combine(steam, "steamapps", "libraryfolders.vdf"),
                "\"libraryfolders\"\n{\n\"0\"\n{\n\"path\" \"" + steam.Replace("\\", "\\\\") + "\"\n}\n\"1\"\n{\n\"path\" \"" + second.Replace("\\", "\\\\") + "\"\n}\n}\n")
            .Text(Path.Combine(second, "steamapps", "appmanifest_1089130.acf"),
                "\"AppState\"\n{\n\"appid\" \"1089130\"\n\"name\" \"Quake II RTX\"\n\"installdir\" \"Quake II RTX\"\n}\n")
            .Folder(Path.Combine(second, "steamapps", "common", "Quake II RTX"));
        var libraries = Q2Rtx.SteamLibraries(steam, files);
        Check(libraries.Count == 2 && libraries[0] == steam && libraries[1] == second, "benchmark: the install and every library of the file: " + string.Join(" | ", libraries));
        Equal(Path.Combine(second, "steamapps", "common", "Quake II RTX"), Q2Rtx.SteamInstallFolder(steam, files), "benchmark: the install folder from the manifest");

        Check(Q2Rtx.SteamLibraries(null, files).Count == 0 && Q2Rtx.SteamLibraries("", files).Count == 0, "benchmark: no Steam, no libraries");
        Equal(1, Q2Rtx.SteamLibraries(@"C:\Steam", files).Count, "benchmark: a Steam without a library file still has its own folder");
        Equal(null, Q2Rtx.SteamInstallFolder(@"C:\Steam", files), "benchmark: a Steam without the application is not an install");

        // A manifest of another application in this file's place, a folder the manifest names but Steam removed, and
        // an install folder that tries to leave the common folder: none of them becomes an install.
        var wrongApp = ReadyGame().Folder(steam)
            .Text(Path.Combine(steam, "steamapps", "appmanifest_1089130.acf"), "\"AppState\"\n{\n\"appid\" \"292030\"\n\"installdir\" \"The Witcher 3\"\n}\n")
            .Folder(Path.Combine(steam, "steamapps", "common", "The Witcher 3"));
        Equal(null, Q2Rtx.SteamInstallFolder(steam, wrongApp), "benchmark: a manifest of another application is not this game");
        var missing = ReadyGame().Folder(steam)
            .Text(Path.Combine(steam, "steamapps", "appmanifest_1089130.acf"), "\"AppState\"\n{\n\"appid\" \"1089130\"\n\"installdir\" \"Quake II RTX\"\n}\n");
        Equal(null, Q2Rtx.SteamInstallFolder(steam, missing), "benchmark: a manifest whose folder is gone is not an install");
        foreach (var bad in new[] { null, "", "   ", "..", ".", @"..\..\Windows", "a/b", @"C:\Windows", "a|b", new string('x', 200) })
            Check(!Q2Rtx.PlainFolderName(bad), "benchmark: an install folder name that is not one folder is refused: " + (bad ?? "null"));
        Check(Q2Rtx.PlainFolderName("Quake II RTX") && Q2Rtx.PlainFolderName(" Quake II RTX "), "benchmark: a plain install folder name is taken");
        var escape = ReadyGame().Folder(steam)
            .Text(Path.Combine(steam, "steamapps", "appmanifest_1089130.acf"), "\"AppState\"\n{\n\"appid\" \"1089130\"\n\"installdir\" \"..\\\\..\\\\Windows\"\n}\n");
        Equal(null, Q2Rtx.SteamInstallFolder(steam, escape), "benchmark: a manifest cannot point the card out of the common folder");
        var damaged = ReadyGame().Folder(steam).Text(Path.Combine(steam, "steamapps", "libraryfolders.vdf"), "\"libraryfolders\"\n{\n\"0\"\n");
        Equal(1, Q2Rtx.SteamLibraries(steam, damaged).Count, "benchmark: a damaged library file leaves the install itself");
    }

    // The command line of one pass: every setting the lab harness measures with, the game folder only read.
    static void BenchCommandLine()
    {
        const string home = @"C:\Users\x\AppData\Local\amdgpu-wddm\benchmarks\q2rtx-home";
        var line = BenchPlan.Arguments(Game, home, RtApi.Pipeline);
        foreach (var want in new[]
        {
            "+set basedir \"" + Game + "\"", "+set homedir \"" + home + "\"", "+set logfile 1", "+set logfile_flush 1",
            "+set logfile_name q2rtx-bench-pipeline", "+set vid_fullscreen 0", "+set vid_geometry 1280x720+0+0",
            "+set vid_modelist desktop", "+set ray_tracing_api pipeline", "+set drs_enable 0", "+set flt_fsr_enable 0",
            "+set viewsize 100", "+set scr_fps 1", "+set vid_vsync 0", "+set s_enable 0", "+set timedemo 1",
            "+set nextserver quit", "+demo q2demo1",
        })
            Check(line.Contains(want), "benchmark: the command line carries " + want + ": " + line);
        Check(line.EndsWith("+demo q2demo1", StringComparison.Ordinal), "benchmark: the demo is the last word, after every setting");
        Check(BenchPlan.Arguments(Game, home, RtApi.Query).Contains("+set ray_tracing_api query"), "benchmark: the second pass asks for the other way");
        Check(BenchPlan.Arguments(Game, home, RtApi.Query).Contains("+set logfile_name q2rtx-bench-query"), "benchmark: each pass writes its own log");
        // Nothing of the game is set but these: no file of the game is written and no stored setting is changed.
        Equal(18, Regex.Matches(line, @"\+set ").Count + Regex.Matches(line, @"\+demo ").Count, "benchmark: the command line has only the named statements: " + line);
        Equal(1280, BenchPlan.Width, "benchmark: the picture is 1280 wide");
        Equal(720, BenchPlan.Height, "benchmark: the picture is 720 high");
        Equal(180000, BenchPlan.PassBoundMs, "benchmark: a pass is bounded to three minutes");
        Equal(2000, BenchPlan.SampleMs, "benchmark: the chip is read every two seconds, as the Performance page does");
        Equal(5000, BenchPlan.ExitBoundMs, "benchmark: a stopped pass waits five seconds for its game to be gone");
        Equal(2000, BenchPlan.FileTimeSlackMs, "benchmark: the result log may be two seconds older than the start of the pass, and no more");
        Check(!new PassRecord().Alive, "benchmark: a pass that was never run has no game left behind");
        Check(BenchPlan.Passes.SequenceEqual(new[] { RtApi.Pipeline, RtApi.Query }), "benchmark: the pipeline pass runs first");
        Equal(Path.Combine(home, "baseq2", "logs", "q2rtx-bench-query.log"), BenchPlan.LogPath(home, RtApi.Query),
            "benchmark: the log is written in our own home directory, never in the game");
        Check(!BenchPlan.LogPath(home, RtApi.Query).StartsWith(Game, StringComparison.OrdinalIgnoreCase), "benchmark: no pass writes inside the game folder");
        Check(BenchPlan.LogName(RtApi.Pipeline).All(c => char.IsLetterOrDigit(c) || c == '-'),
            "benchmark: a log name the game does not have to clean up: " + BenchPlan.LogName(RtApi.Pipeline));
        Throws<ArgumentException>(() => BenchPlan.Arguments(null, home, RtApi.Query), "benchmark: no game folder, no command line");
        Throws<ArgumentException>(() => BenchPlan.Arguments(Game, "", RtApi.Query), "benchmark: no home directory, no command line");
        Throws<ArgumentException>(() => BenchPlan.Arguments("D:\\a\"b", home, RtApi.Query), "benchmark: a quotation mark in a path is refused");
        Equal("+set basedir \"D:\\Games\"", BenchPlan.Arguments(@"D:\Games\", home, RtApi.Query).Split(new[] { " +set homedir" }, StringSplitOptions.None)[0],
            "benchmark: a trailing separator is trimmed off the game folder");
    }

    // The result line of the log. The game prints it with C's printf, so it always carries a dot, whatever the
    // language of Windows; the card parses it that way in every culture.
    static void BenchResultLine()
    {
        var before = Thread.CurrentThread.CurrentCulture;
        try
        {
            foreach (var name in new[] { "en-US", "pl-PL", "de-DE", "ja-JP", "ko-KR" })
            {
                Thread.CurrentThread.CurrentCulture = CultureInfo.GetCultureInfo(name);
                var r = BenchLog.Parse("[2026-10-11 08:12] 1550 frames, 25.43 seconds: 60.940002 fps");
                Check(r != null && r.Frames == 1550, name + ": benchmark: the frame count of the result line");
                Check(r != null && Math.Abs(r.Seconds - 25.43) < 1e-9, name + ": benchmark: the seconds of the result line");
                Check(r != null && Math.Abs(r.Fps - 60.940002) < 1e-6, name + ": benchmark: the pictures a second of the result line");
                // The game writes the seconds with "%3.2f", which pads a short run with spaces.
                var pad = BenchLog.Parse("240 frames,  4.00 seconds: 60.000000 fps");
                Check(pad != null && pad.Frames == 240 && Math.Abs(pad.Fps - 60) < 1e-9, name + ": benchmark: a padded result line");
                Check(BenchText.Fps(60.94) == 60.94.ToString("0.0", CultureInfo.CurrentCulture), name + ": benchmark: the window shows the number in the reader's own culture");
            }
        }
        finally { Thread.CurrentThread.CurrentCulture = before; }

        Equal(null, BenchLog.Parse(null), "benchmark: no line, no result");
        Equal(null, BenchLog.Parse("Logging console to q2rtx-bench-pipeline.log"), "benchmark: an ordinary log line is not a result");
        Equal(null, BenchLog.Parse("1550 frames, 25,43 seconds: 60,94 fps"), "benchmark: a comma is not this game's decimal mark");
        Equal(null, BenchLog.Parse("0 frames, 1.00 seconds: 0.000000 fps"), "benchmark: a run of no frames is not a result");
        Equal(null, BenchLog.Parse("10 frames, 0.00 seconds: 0.000000 fps"), "benchmark: a run of no time is not a result");
        Equal(null, BenchLog.Last(""), "benchmark: an empty log has no result");
        Equal(null, BenchLog.Last(null), "benchmark: no log, no result");
        var log = "start\n100 frames, 2.00 seconds: 50.000000 fps\nagain\n120 frames, 2.00 seconds: 60.000000 fps\nquit\n";
        var last = BenchLog.Last(log);
        Check(last != null && last.Frames == 120, "benchmark: a log of two timed runs gives the last one");
    }

    // The thermal stop: 89 C at once, 87 C held for 10 s, and a reading the driver cannot give
    // is never a temperature.
    static void BenchThermal()
    {
        Equal(89000, ThermalGuard.StopMc, "benchmark: the chip stops a pass at 89 C");
        Equal(87000, ThermalGuard.HoldMc, "benchmark: 87 C starts the hold");
        Equal(10000, ThermalGuard.HoldMs, "benchmark: the hold is ten seconds");

        var at89 = new ThermalGuard();
        Check(at89.Hot(0, 89000), "benchmark: 89 C stops the pass at once");
        var cool = new ThermalGuard();
        Check(!cool.Hot(0, 70000) && !cool.Hot(60000, 86999), "benchmark: a cool chip never stops a pass");
        Check(cool.HotSinceMs == null, "benchmark: a cool chip holds nothing");

        var hold = new ThermalGuard();
        Check(!hold.Hot(0, 87000), "benchmark: 87 C alone does not stop the pass");
        Check(hold.HotSinceMs == 0, "benchmark: 87 C starts the hold at that sample");
        Check(!hold.Hot(8000, 87500), "benchmark: eight seconds at 87 C is not yet ten");
        Check(hold.Hot(10000, 87500), "benchmark: ten seconds at 87 C or hotter stops the pass");

        var cleared = new ThermalGuard();
        Check(!cleared.Hot(0, 88000) && !cleared.Hot(4000, 80000), "benchmark: a chip that cooled down clears the hold");
        Check(cleared.HotSinceMs == null && !cleared.Hot(14000, 88000), "benchmark: the hold starts again from the new hot sample");

        var unreadable = new ThermalGuard();
        Check(!unreadable.Hot(0, null) && !unreadable.Hot(60000, null) && unreadable.HotSinceMs == null,
            "benchmark: a reading the driver cannot give never stops a pass by itself");
        var kept = new ThermalGuard();
        Check(!kept.Hot(0, 88000) && !kept.Hot(4000, null), "benchmark: a reading that is missing does not clear the hold");
        Check(kept.HotSinceMs == 0 && kept.Hot(10000, null), "benchmark: the hold runs out even while the readings are missing");
    }

    // The two lines the card writes about a run: the state line above the results, and the list of earlier runs
    // (b23 lab round, F2, F3 and F4).
    static void BenchCardLines()
    {
        var now = new DateTime(2026, 10, 11, 8, 0, 0, DateTimeKind.Utc);
        var done = BenchRun(Recovery.Stamp(now), PassOutcome.Done, 61.4, PassOutcome.Done, 62.4);
        var stopped = BenchRun(Recovery.Stamp(now.AddMinutes(10)), PassOutcome.Cancelled, null, PassOutcome.Cancelled, null);
        var hot = BenchRun(Recovery.Stamp(now.AddMinutes(20)), PassOutcome.TooHot, null, PassOutcome.TooHot, null);
        var starting = new RunRecord { Utc = Recovery.Stamp(now.AddMinutes(30)), Driver = "1.0.0.0-tester.11" };

        // While a run goes on, the stage of that run decides, whatever is saved on this computer.
        Equal(BenchState.Starting, BenchState.Id(BenchProgress.Starting, null, starting, true), "F2: a run that starts says so");
        Equal(BenchState.Running, BenchState.Id(BenchProgress.Running, "pipeline", starting, true), "F2: a run that measures names its way");
        Equal(BenchState.Starting, BenchState.Id(BenchProgress.Running, null, starting, true), "F2: between the passes the line is the one of a start");
        Equal(BenchState.Reading, BenchState.Id(BenchProgress.Reading, null, starting, true), "F2: a run whose result is read says so");

        // Nothing saved and nothing run: the only line that may say "not run on this computer yet".
        Equal(BenchState.Idle, BenchState.Id(null, null, null, false), "F2: no run at all is the idle line");
        Equal(BenchState.Idle, BenchState.Id(BenchProgress.Idle, null, null, false), "F2: an idle run with no history is the idle line");
        Equal(BenchState.Idle, BenchState.Id(BenchProgress.Finished, null, starting, true), "F2: a run without a single pass is the idle line");

        // F2: a saved run on the card after a new start of the application is never "not run yet".
        Equal(BenchState.Saved, BenchState.Id(null, null, done, false), "F2: a run read back from the history says it is the newest saved one");
        Equal(BenchState.Saved, BenchState.Id(null, null, hot, false), "F2: a saved run stopped by the heat says it is the newest saved one");
        Equal(BenchState.Done, BenchState.Id(BenchProgress.Finished, null, done, true), "F2: a run of this session that finished says it is finished");

        // F3: a run the person stopped says that, in this session and after a restart.
        Equal(BenchState.Stopped, BenchState.Id(BenchProgress.Finished, null, stopped, true), "F3: a run the person stopped says so, not that it finished");
        Equal(BenchState.Stopped, BenchState.Id(null, null, stopped, false), "F3: a saved run the person stopped says so");
        var half = BenchRun(Recovery.Stamp(now), PassOutcome.Done, 61.4, PassOutcome.Cancelled, null);
        Equal(BenchState.Stopped, BenchState.Id(BenchProgress.Finished, null, half, true), "F3: a run stopped in its second way says so");

        // F4: "Earlier runs" never repeats the run the card shows in full above it.
        var history = new List<RunRecord> { hot, stopped, done };
        Equal(2, BenchHistory.Earlier(history, hot).Count, "F4: the run shown above is not in the list of earlier runs");
        Equal(stopped.Utc, BenchHistory.Earlier(history, hot)[0].Utc, "F4: the list starts at the run before the one shown");
        Equal(3, BenchHistory.Earlier(history, starting).Count, "F4: a run that is not saved yet hides none of the saved ones");
        Equal(3, BenchHistory.Earlier(history, null).Count, "F4: with no run shown the list holds every saved run");
        Equal(0, BenchHistory.Earlier(null, done).Count, "F4: no history is an empty list");
        Equal(0, BenchHistory.Earlier(new List<RunRecord> { done }, done).Count, "F4: one saved run shown above leaves no earlier run");
        // The same run read back from the file is another object with the same time.
        var read = BenchHistory.Parse(BenchHistory.Render(history));
        Equal(2, BenchHistory.Earlier(read, BenchHistory.Earlier(read, null)[0]).Count, "F4: a run is named by its time, not by its object");
    }

    static RunRecord BenchRun(string utc, string pipelineOutcome, double? pipelineFps, string queryOutcome, double? queryFps)
    {
        return new RunRecord
        {
            Utc = utc, Driver = "1.0.0.0-tester.11", Passes = new List<PassRecord>
            {
                new PassRecord { Api = "pipeline", Outcome = pipelineOutcome, Fps = pipelineFps, Frames = 1550, Seconds = 25.43, MaxTempC = 79, MeanClockMHz = 1948 },
                new PassRecord { Api = "query", Outcome = queryOutcome, Fps = queryFps, Frames = 1550, Seconds = 23.35, MaxTempC = 81, MeanClockMHz = 1951 },
            },
        };
    }

    // The history: a round trip, the newest ten, and every damaged file read as no history.
    static void BenchHistoryFile()
    {
        var now = new DateTime(2026, 10, 11, 8, 0, 0, DateTimeKind.Utc);
        var one = BenchRun(Recovery.Stamp(now), PassOutcome.Done, 60.94, PassOutcome.Done, 66.39);
        var again = BenchHistory.Parse(BenchHistory.Render(new List<RunRecord> { one }));
        Equal(1, again.Count, "benchmark: the history is read back");
        Equal(one.Utc, again[0].Utc, "benchmark: the time of a run survives the round trip");
        Equal("1.0.0.0-tester.11", again[0].Driver, "benchmark: the driver release of a run survives");
        Check(again[0].Pass(RtApi.Pipeline).Fps == 60.94 && again[0].Pass(RtApi.Query).Fps == 66.39, "benchmark: both numbers survive the round trip");
        Check(again[0].Pass(RtApi.Pipeline).MaxTempC == 79 && again[0].Pass(RtApi.Query).MeanClockMHz == 1951, "benchmark: temperature and clock survive the round trip");
        Equal(1280, again[0].Width, "benchmark: the picture size survives the round trip");

        var stopped = BenchHistory.Parse(BenchHistory.Render(new List<RunRecord> { BenchRun(Recovery.Stamp(now), PassOutcome.TooHot, null, PassOutcome.TooHot, null) }));
        Equal(PassOutcome.TooHot, stopped[0].Pass(RtApi.Query).Outcome, "benchmark: a pass stopped by the heat is kept as that");
        Equal(null, stopped[0].Pass(RtApi.Query).Fps, "benchmark: a pass without a number keeps none");

        var left = BenchRun(Recovery.Stamp(now), PassOutcome.TimedOut, null, PassOutcome.TimedOut, null);
        left.Passes[0].Alive = true;
        Check(!BenchHistory.Parse(BenchHistory.Render(new List<RunRecord> { left }))[0].Pass(RtApi.Pipeline).Alive,
            "benchmark: a game left behind is a fact of the run that made it, never of the history");

        var many = new List<RunRecord>();
        for (int i = 0; i < 25; i++) many.Add(BenchRun(Recovery.Stamp(now.AddMinutes(-i)), PassOutcome.Done, 60 + i, PassOutcome.Done, 66));
        Equal(BenchHistory.Keep, BenchHistory.Parse(BenchHistory.Render(many)).Count, "benchmark: the file keeps ten runs");
        var grown = many;
        for (int i = 0; i < 5; i++) grown = BenchHistory.Add(grown, BenchRun(Recovery.Stamp(now.AddMinutes(i + 1)), PassOutcome.Done, 61, PassOutcome.Done, 67));
        Equal(BenchHistory.Keep, grown.Count, "benchmark: adding a run never grows the history past ten");
        Equal(Recovery.Stamp(now.AddMinutes(5)), grown[0].Utc, "benchmark: the newest run is first");
        Equal(1, BenchHistory.Add(null, one).Count, "benchmark: the first run of a computer makes the history");
        Equal(0, BenchHistory.Add(null, null).Count, "benchmark: nothing added, nothing kept");

        foreach (var bad in new[]
        {
            null, "", "not json at all", "[]", "{}", "{\"schema\":2,\"runs\":[]}", "{\"schema\":1}", "{\"schema\":1,\"runs\":{}}",
            "{\"schema\":1,\"runs\":[{\"utc\":\"not a time\",\"width\":1280,\"height\":720,\"passes\":[{\"api\":\"query\",\"outcome\":\"done\",\"fps\":60}]}]}",
            "{\"schema\":1,\"runs\":[{\"utc\":\"2026-10-11T08:00:00.000Z\",\"width\":0,\"height\":720,\"passes\":[{\"api\":\"query\",\"outcome\":\"done\",\"fps\":60}]}]}",
            "{\"schema\":1,\"runs\":[{\"utc\":\"2026-10-11T08:00:00.000Z\",\"width\":1280,\"height\":720,\"passes\":[]}]}",
            "{\"schema\":1,\"runs\":[{\"utc\":\"2026-10-11T08:00:00.000Z\",\"width\":1280,\"height\":720,\"passes\":[{\"api\":\"other\",\"outcome\":\"done\",\"fps\":60}]}]}",
            "{\"schema\":1,\"runs\":[{\"utc\":\"2026-10-11T08:00:00.000Z\",\"width\":1280,\"height\":720,\"passes\":[{\"api\":\"query\",\"outcome\":\"invented\",\"fps\":60}]}]}",
            "{\"schema\":1,\"runs\":[{\"utc\":\"2026-10-11T08:00:00.000Z\",\"width\":1280,\"height\":720,\"passes\":[{\"api\":\"query\",\"outcome\":\"done\"}]}]}",
        })
            Equal(0, BenchHistory.Parse(bad).Count, "benchmark: a damaged history is no history: " + (bad ?? "null"));
        Equal(0, BenchHistory.Parse(new string('x', BenchHistory.MaxBytes + 1)).Count, "benchmark: a history above the bound is no history");
        // A damaged run between sound ones is dropped, and the sound ones are still read.
        var mixed = "{\"schema\":1,\"runs\":[{\"utc\":\"bad\"},{\"utc\":\"2026-10-11T08:00:00.000Z\",\"width\":1280,\"height\":720," +
            "\"passes\":[{\"api\":\"pipeline\",\"outcome\":\"done\",\"fps\":60.94},{\"api\":\"query\",\"outcome\":\"cancelled\"}]}]}";
        var kept = BenchHistory.Parse(mixed);
        Equal(1, kept.Count, "benchmark: a damaged run is dropped and the sound one is kept");
        Equal(PassOutcome.Cancelled, kept[0].Pass(RtApi.Query).Outcome, "benchmark: the sound run keeps its reasons");

        // The file itself, in a directory of this test's own.
        var dir = Path.Combine(Path.GetTempPath(), "amdgpu-wddm-bench-test-" + Guid.NewGuid().ToString("N"));
        var over = Environment.GetEnvironmentVariable("AMDGPU_WDDM_CONTROL_STATE");
        try
        {
            Environment.SetEnvironmentVariable("AMDGPU_WDDM_CONTROL_STATE", dir);
            Equal(0, BenchPaths.ReadHistory().Count, "benchmark: no file is no history");
            var saved = BenchPaths.SaveRun(one);
            Equal(1, saved.Count, "benchmark: the first run is saved");
            Equal(1, BenchPaths.ReadHistory().Count, "benchmark: the saved run is read again");
            Check(File.Exists(BenchPaths.HistoryFile) && BenchPaths.HistoryFile.EndsWith(@"benchmarks\q2rtx-history.json", StringComparison.OrdinalIgnoreCase),
                "benchmark: the history file is amdgpu-wddm\\benchmarks\\q2rtx-history.json: " + BenchPaths.HistoryFile);
            Check(BenchPaths.Home.EndsWith(@"benchmarks\q2rtx-home", StringComparison.OrdinalIgnoreCase), "benchmark: the game's home directory is ours: " + BenchPaths.Home);
            File.WriteAllText(BenchPaths.HistoryFile, "{ this is not json");
            Equal(0, BenchPaths.ReadHistory().Count, "benchmark: a damaged file on disk is no history");
            Equal(1, BenchPaths.SaveRun(one).Count, "benchmark: a damaged file does not stop a new run from being saved");
            Equal(1, BenchPaths.ReadHistory().Count, "benchmark: the new run replaced the damaged file");
            Check(!Directory.GetFiles(Path.GetDirectoryName(BenchPaths.HistoryFile)).Any(f => f.EndsWith(".tmp", StringComparison.OrdinalIgnoreCase)),
                "benchmark: saving leaves no temporary file behind");
        }
        finally
        {
            Environment.SetEnvironmentVariable("AMDGPU_WDDM_CONTROL_STATE", over);
            try { Directory.Delete(dir, true); } catch (Exception) { }
        }

        Check(Math.Abs(BenchReference.LowFps - 57.8) < 1e-9 && Math.Abs(BenchReference.HighFps - 69.7) < 1e-9,
            "benchmark: our own board's number is the measured band of 57.8 to 69.7 pictures a second");
        Check(BenchReference.LowFps < BenchReference.HighFps && Strings.Has("bench.reference.why"),
            "benchmark: the band is a band, and the card says why the number moves inside it");
        Check(BenchReference.Width == 1280 && BenchReference.Height == 720, "benchmark: our own board's numbers belong to 1280 by 720");
        Check(PassOutcome.All.Length == 6 && PassOutcome.Known(PassOutcome.Done) && !PassOutcome.Known("invented") && !PassOutcome.Known(null),
            "benchmark: the reasons a pass can have are a closed list");

        // F8: a pass that was stopped before its game started has no temperature and no clock of its own. The one
        // reading taken before a start belongs to the decision to start, not to the pass; only the pass that was
        // refused because the chip was too hot keeps it, because there the reading is the reason.
        Check(!PassTelemetry.Keep(PassOutcome.Cancelled, false), "F8: a pass stopped before its game ran reports no reading");
        Check(!PassTelemetry.Keep(PassOutcome.Failed, false), "F8: a pass whose game never started reports no reading");
        Check(!PassTelemetry.Keep(PassOutcome.NoResult, false), "F8: a pass with no game and no result reports no reading");
        Check(PassTelemetry.Keep(PassOutcome.TooHot, false), "F8: the reading that refused a start is the reason, so it is kept");
        Check(PassTelemetry.Keep(PassOutcome.Cancelled, true), "F8: a pass stopped while its game ran keeps what it measured");
        Check(PassTelemetry.Keep(PassOutcome.Done, true) && PassTelemetry.Keep(PassOutcome.TimedOut, true),
            "F8: a pass that ran keeps what it measured, whatever ended it");
    }

    // What the card and the runner may not do, by a scan of the sources.
    static void BenchSourceScan(string root)
    {
        var dir = Path.Combine(root, @"tools\win\amdgpu_wddm_control\src");
        var runner = File.ReadAllText(Path.Combine(dir, "BenchmarkRun.cs"));
        var card = File.ReadAllText(Path.Combine(dir, "MainForm.Bench.cs"));
        var pure = File.ReadAllText(Path.Combine(dir, "Benchmark.cs"));
        var perf = File.ReadAllText(Path.Combine(dir, "MainForm.Pages.cs"));

        // A pass ends by closing its own job object, which holds only the processes this program started. The card
        // and the rules therefore end no process at all.
        foreach (var kv in new[] { new[] { "MainForm.Bench.cs", card }, new[] { "Benchmark.cs", pure } })
        {
            var text = Regex.Replace(kv[1], @"//[^\n]*", "");
            Check(!Regex.IsMatch(text, @"\.Kill\s*\(") && !text.Contains("TerminateProcess") && !text.Contains("taskkill") && !text.Contains("GetProcessesByName"),
                "benchmark: " + kv[0] + " ends no process");
        }
        var runnerCode = Regex.Replace(runner, @"//[^\n]*", "");
        Check(!runnerCode.Contains("TerminateProcess") && !runnerCode.Contains("taskkill") && !runnerCode.Contains("GetProcessesByName"),
            "benchmark: the runner ends no process by name");
        Check(runner.Contains("JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE") && runner.Contains("const uint KillOnJobClose = 0x2000;") &&
            runner.Contains("AssignProcessToJobObject") && Regex.Matches(runner, @"TerminateJobObject\(").Count == 2,
            "benchmark: a pass ends through the job object it created, and nothing else");
        // The job is the only way a pass can end, so it exists before the game and the game must join it: a pass
        // with no job is never started, and a game that could not join one is ended at once by its own handle
        // (G-SRC names BenchmarkRun.cs for that one ending). Without this a thermal stop would be a silent no-op.
        Check(runnerCode.Contains("if (job == null) { record.Outcome = PassOutcome.Failed; store(); return record; }") &&
            runnerCode.IndexOf("BenchJob.Create()") < runnerCode.IndexOf("Process.Start(psi)"),
            "benchmark: a pass with no job object never starts the game");
        Check(Regex.IsMatch(runnerCode, @"if \(!job\.Take\(game\)\)\s*\{\s*try \{ game\.Kill\(\); \}") &&
            Regex.Matches(runnerCode, @"\.Kill\s*\(").Count == 1,
            "benchmark: a game that could not join its job object is ended at once, and that is the one process the runner ends");
        Check(runnerCode.Contains("if (!game.WaitForExit(BenchPlan.ExitBoundMs)) record.Alive = true;") &&
            runnerCode.Contains("record.Outcome == PassOutcome.TimedOut") && runnerCode.Contains("if (record.Alive ||"),
            "benchmark: a stopped pass waits for its game, and a game still there or a pass out of time ends the run");
        // A stale log is never read as a measurement: the pass refuses a log it could not remove, and reads only a
        // log written after it started the game.
        Check(runnerCode.Contains("if (File.Exists(log)) { record.Outcome = PassOutcome.Failed; store(); return record; }") &&
            runnerCode.Contains("info.LastWriteTimeUtc < notBefore.AddMilliseconds(-BenchPlan.FileTimeSlackMs)"),
            "benchmark: only a log this pass wrote is read as this pass's result");
        Check(Regex.Matches(runner, @"Process\.Start\(").Count == 1 && runner.Contains("WorkingDirectory = install.Folder"),
            "benchmark: one child process per pass, started in the game folder");
        Check(runner.Contains("BenchPlan.Arguments(install.Folder, home, api)"), "benchmark: the runner takes the command line from the plan, never its own");
        Check(runner.Contains("Kmd.Dpm()"), "benchmark: the thermal stop reads the driver's own snapshot");
        Check(runnerCode.Contains("if (d.Has(DpmState.FlagClock) && d.ObservedMHz != 0) clockMHz = d.ObservedMHz;"),
            "benchmark: the average clock counts one source, the clock the driver measured");

        // The card is on the Performance page, draws our own board's numbers only on a board the app identified, and
        // searches no disk while a build gate draws it.
        Check(perf.Contains("p.Controls.Add(BuildBenchmarkCard(width));"), "benchmark: the card is built on the Performance page");
        // Two texts: the band our own board drew, and the line that says why a run lands anywhere inside it.
        Check(card.Contains("if (UmaSetting.Visible(_uma))") && Regex.Matches(card, @"bench\.reference").Count == 2,
            "benchmark: our own board's numbers are shown only on a board the app identified as a BC-250");
        // The search is part of the window's reading (WindowReading.cs), which a gate run asks for with gate set.
        var reading = File.ReadAllText(Path.Combine(dir, "WindowReading.cs"));
        int gate = reading.IndexOf("if (!gate)", StringComparison.Ordinal), find = reading.IndexOf("BenchLocator.Find(", StringComparison.Ordinal);
        Check(File.ReadAllText(Path.Combine(dir, "MainForm.cs")).Contains("WindowReading.Read(_prefs, _smoke || ReadOnlyProbe)") && gate > 0 && find > gate &&
            !card.Contains("BenchLocator.Find("), "benchmark: a build gate searches no folder of the PC that builds it");
        Check(Regex.IsMatch(card, @"BenchRunner\.Start\(_bench, InstalledVersion, OnBenchmarkProgress\)"), "benchmark: the card starts the one runner");
        Check(card.Contains("BeginInvoke") && !Regex.IsMatch(Regex.Replace(card, @"//[^\n]*", ""), @"WaitForExit|Thread\.Sleep|\.Wait\(\)"),
            "benchmark: the window never waits for the game on its own thread");
        Check(!Regex.IsMatch(Regex.Replace(card, @"//[^\n]*", ""), @"\bKmd\.\w+\("), "benchmark: the card reads the driver through the page's snapshot, not by itself");
        // The thermal stop is promised only where the chip's temperature can be read at all: the stop watches the
        // one reading the Performance page shows, so without it only the time bound of each way is left.
        Check(card.Contains("Strings.T(BenchTempRead ? \"bench.note\" : \"bench.note.notemp\")") &&
            card.Contains("_snap.Dpm.Has(DpmState.FlagTemperature)") && Strings.Has("bench.note") && Strings.Has("bench.note.notemp"),
            "benchmark: the card promises the thermal stop only on a board whose temperature it can read");
        Check(Regex.IsMatch(card, @"bench\.reference"", BenchText\.Reference") && card.Contains("BenchReference.Release"),
            "benchmark: our own board's numbers name the release they were measured with");
        var run = File.ReadAllText(Path.Combine(root, @"tools\win\amdgpu_wddm_control\src\BenchmarkRun.cs"));
        Check(run.Contains("bool keep = PassTelemetry.Keep(record.Outcome, gameStarted);") && run.Contains("gameStarted = true;"),
            "F8: the run stores a reading only for a pass whose game ran");

        // Every text the card asks for by its whole name exists (a name the card builds out of parts ends in a dot
        // here and is checked by the families below). G-NOINT and the em dash are checked over every table in
        // GuiTests.PlainWordsInTables, so the card's words go through that gate with all the others.
        var ids = Regex.Matches(card + pure, @"Strings\.T\(""(?<id>[a-z0-9.-]+)""").Cast<Match>()
            .Select(m => m.Groups["id"].Value).Where(id => !id.EndsWith(".", StringComparison.Ordinal)).Distinct().ToList();
        Check(ids.Count > 20, "benchmark: the card's texts were found in its source: " + ids.Count);
        foreach (var id in ids) Check(Strings.Has(id), "benchmark: the card's text " + id + " exists");
        foreach (var api in BenchPlan.Passes)
        {
            var name = BenchPlan.ApiName(api);
            Check(Strings.Has("bench.pass." + name) && Strings.Has("bench.pass." + name + ".note"), "benchmark: both names of the " + name + " pass exist");
        }
        // Every verdict but Ready has plain words of its own in the card (the window itself is not in this test
        // program, so the card's own switch is read from its source).
        foreach (var verdict in Enum.GetValues(typeof(InstallVerdict)).Cast<InstallVerdict>().Where(v => v != InstallVerdict.Ready))
        {
            if (verdict == InstallVerdict.NotFound) { Check(Strings.Has("bench.game.none"), "benchmark: the plain words for NotFound exist"); continue; }
            var m = Regex.Match(card, @"case InstallVerdict\." + verdict + @": return ""(?<id>[a-z0-9.-]+)"";");
            Check(m.Success && Strings.Has(m.Groups["id"].Value), "benchmark: the plain words for " + verdict + " exist");
        }
        Check(Strings.Has("search.perf.benchmark") && Strings.Has("help.setting.benchmark"), "benchmark: the search entry and its explanation exist");
        Check(SettingsSearch.Index.Any(e => e.Id == "perf.benchmark" && e.Page == "performance" && !e.ComingLater), "benchmark: the card is searchable on the Performance page");
        Equal("perf.benchmark", SettingsSearch.Find("benchmark", "en").First().Entry.Id, "benchmark: the word benchmark leads to the card");
        Equal("graphics.fps", SettingsSearch.Find("FPS limit", "en").First().Entry.Id, "benchmark: the new entry did not take the frame rate limit's place");
        foreach (var lang in Strings.Languages)
            Check(SettingsSearch.Find(Strings.In(lang, "search.perf.benchmark"), lang).Any(h => h.Entry.Id == "perf.benchmark"),
                "benchmark: the " + lang + " name of the card finds it");
    }
}
