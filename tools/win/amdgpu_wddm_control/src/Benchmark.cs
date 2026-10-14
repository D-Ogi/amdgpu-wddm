// The ray tracing benchmark card's rules (owner, 2026-10-11: "chce karte benchmarku Q2 RTX"). Pure functions over
// text, bytes and streams, so the host tests decide every case without a game, a driver or a process: Valve's
// key-value text, the PAK directory, what makes a copy of the game usable, the command line of one pass, the result
// line of the game's log, the thermal stop and the short history. The runner is src/BenchmarkRun.cs and the card is
// src/MainForm.Bench.cs.
//
// The package never ships and never downloads the game. The card measures a copy the person already has: the free
// Quake II RTX of NVIDIA, from Steam (app 440900) or from a folder the person chooses.
//
// Why the test is two passes. Quake II RTX traces its rays through one of two Vulkan contracts, chosen by its own
// cvar `ray_tracing_api`: a ray tracing pipeline, or a ray query inside an ordinary shader. The picture is the same;
// the speed is not, and on this driver the two differ by a few pictures a second. One run of the card measures both,
// in that order, so a tester reports one number for each.
//
// Where the game writes. Quake II RTX reads `homedir` from its command line before it opens any file (its
// src/windows/system.c Sys_Init takes the value the early `+set` left behind, because CVAR_NOSET only locks after
// Com_Init; its src/common/files.c setup_base_gamedir then makes `<homedir>/baseq2` the one directory it writes).
// Every pass therefore gets our own home directory and the game folder is only read: a Steam copy, a read-only
// folder and a folder owned by another account all work, and no pass leaves a file behind in the game.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;

namespace AmdgpuWddmControl
{
    // ---- Valve's key-value text: libraryfolders.vdf and appmanifest_<id>.acf are the same format -------------------

    public sealed class VdfNode
    {
        public string Value;                 // the text of a leaf; null for a block
        public readonly List<string> Order = new List<string>();
        public readonly Dictionary<string, VdfNode> Children = new Dictionary<string, VdfNode>(StringComparer.OrdinalIgnoreCase);

        public VdfNode Child(params string[] path)
        {
            var at = this;
            foreach (var key in path)
            {
                VdfNode next;
                if (at == null || !at.Children.TryGetValue(key, out next)) return null;
                at = next;
            }
            return at;
        }

        // The text of a leaf below this node; null when the path is absent or names a block.
        public string Text(params string[] path)
        {
            var node = Child(path);
            return node == null ? null : node.Value;
        }
    }

    public static class Vdf
    {
        public const int MaxBytes = 4 << 20, MaxDepth = 32, MaxNodes = 20000, MaxToken = 4096;

        // The whole text as one unnamed root block, or null when it is not this format. A damaged file is never read
        // in part: a partial tree would make the card name a folder that Steam does not have.
        public static VdfNode Parse(string text)
        {
            if (text == null || text.Length > MaxBytes) return null;
            int at = 0, nodes = 0;
            var root = new VdfNode();
            if (!Block(text, ref at, root, 0, ref nodes, true)) return null;
            return root;
        }

        static bool Block(string text, ref int at, VdfNode into, int depth, ref int nodes, bool top)
        {
            if (depth > MaxDepth) return false;
            while (true)
            {
                SkipBlank(text, ref at);
                if (at >= text.Length) return top;
                if (text[at] == '}') { if (top) return false; at++; return true; }
                string key;
                if (!Token(text, ref at, out key)) return false;
                SkipBlank(text, ref at);
                if (at >= text.Length) return false;
                if (++nodes > MaxNodes) return false;
                var child = new VdfNode();
                if (text[at] == '{')
                {
                    at++;
                    if (!Block(text, ref at, child, depth + 1, ref nodes, false)) return false;
                }
                else
                {
                    string value;
                    if (!Token(text, ref at, out value)) return false;
                    child.Value = value;
                }
                if (!into.Children.ContainsKey(key)) into.Order.Add(key);
                into.Children[key] = child;
            }
        }

        static void SkipBlank(string text, ref int at)
        {
            while (at < text.Length)
            {
                char c = text[at];
                if (c == '/' && at + 1 < text.Length && text[at + 1] == '/')
                {
                    while (at < text.Length && text[at] != '\n') at++;
                }
                else if (char.IsWhiteSpace(c)) at++;
                else return;
            }
        }

        // A quoted token (with Valve's four escapes) or a bare one up to the next space, brace or quote.
        static bool Token(string text, ref int at, out string token)
        {
            token = null;
            if (at >= text.Length) return false;
            var w = new StringBuilder();
            if (text[at] == '"')
            {
                at++;
                while (true)
                {
                    if (at >= text.Length) return false;
                    char c = text[at++];
                    if (c == '"') break;
                    if (c == '\n') return false;
                    if (c == '\\')
                    {
                        if (at >= text.Length) return false;
                        char e = text[at++];
                        w.Append(e == 'n' ? '\n' : e == 't' ? '\t' : e);
                    }
                    else w.Append(c);
                    if (w.Length > MaxToken) return false;
                }
            }
            else
            {
                while (at < text.Length && !char.IsWhiteSpace(text[at]) && text[at] != '{' && text[at] != '}' && text[at] != '"')
                {
                    w.Append(text[at++]);
                    if (w.Length > MaxToken) return false;
                }
                if (w.Length == 0) return false;
            }
            token = w.ToString();
            return true;
        }
    }

    // ---- the PAK directory (id Software's format, as Quake II RTX still reads it) ----------------------------------

    public static class Pak
    {
        public const int EntrySize = 64, NameSize = 56, MaxEntries = 65536;

        // Every entry name with forward slashes, or null when the stream is not a PAK. Only the directory is read:
        // the card never unpacks or writes anything of the game.
        public static List<string> Entries(Stream s)
        {
            if (s == null) return null;
            try
            {
                var head = Read(s, 0, 12);
                if (head == null || head[0] != (byte)'P' || head[1] != (byte)'A' || head[2] != (byte)'C' || head[3] != (byte)'K') return null;
                int offset = BitConverter.ToInt32(head, 4), length = BitConverter.ToInt32(head, 8);
                if (offset < 12 || length < 0 || length % EntrySize != 0 || length / EntrySize > MaxEntries) return null;
                if (!s.CanSeek || offset + (long)length > s.Length) return null;
                var table = Read(s, offset, length);
                if (table == null) return null;
                var names = new List<string>(length / EntrySize);
                for (int i = 0; i < length; i += EntrySize)
                {
                    int end = i;
                    while (end < i + NameSize && table[end] != 0) end++;
                    var name = Encoding.ASCII.GetString(table, i, end - i).Replace('\\', '/').Trim();
                    if (name.Length > 0) names.Add(name);
                }
                return names;
            }
            catch (Exception) { return null; }
        }

        public static bool Has(Stream s, string name)
        {
            var names = Entries(s);
            return names != null && names.Any(n => string.Equals(n, name, StringComparison.OrdinalIgnoreCase));
        }

        static byte[] Read(Stream s, long at, int bytes)
        {
            if (!s.CanSeek) return null;
            s.Position = at;
            var b = new byte[bytes];
            int got = 0;
            while (got < bytes)
            {
                int n = s.Read(b, got, bytes - got);
                if (n <= 0) return null;
                got += n;
            }
            return b;
        }
    }

    // ---- what the card needs of a copy of the game ----------------------------------------------------------------

    public enum InstallVerdict
    {
        NotFound,          // no Steam copy and no folder chosen
        NoFolder,          // the chosen folder is gone
        NoProgram,         // no q2rtx.exe in it
        NoData,            // no baseq2\pak0.pak, or one that cannot be read
        NoDemo,            // pak0.pak without the test scene (demos/q2demo1.dm2)
        Ready,
    }

    public sealed class InstallState
    {
        public string Folder, ExePath, PakPath;
        public InstallVerdict Verdict = InstallVerdict.NotFound;
        public bool FromSteam;
        public bool Ready { get { return Verdict == InstallVerdict.Ready; } }
    }

    // The few file operations the rules need, so the tests run against a dictionary and the window against the disk.
    public interface IBenchFiles
    {
        bool DirectoryExists(string path);
        bool FileExists(string path);
        Stream OpenRead(string path);                      // null when it cannot be read
        string ReadText(string path, int maxBytes);        // null when it cannot be read or is larger
    }

    public static class Q2Rtx
    {
        public const string SteamAppId = "440900";
        public const string ExeName = "q2rtx.exe";
        public const string DataFolder = "baseq2";
        public const string PakName = "pak0.pak";
        public const string DemoEntry = "demos/q2demo1.dm2";
        public const string DemoName = "q2demo1";
        public const string StoreUrl = "https://store.steampowered.com/app/440900/";
        public const string ReleasesUrl = "https://github.com/NVIDIA/Q2RTX/releases";
        public const int MaxLibraries = 32, MaxVdfBytes = Vdf.MaxBytes;

        // Only these two addresses are ever opened from the card (the same rule the update check keeps for its own).
        public static bool AllowedLink(string url) { return url == StoreUrl || url == ReleasesUrl; }

        public static InstallState Inspect(string folder, IBenchFiles files)
        {
            var s = new InstallState { Folder = folder };
            if (string.IsNullOrEmpty(folder) || files == null) return s;
            if (!files.DirectoryExists(folder)) { s.Verdict = InstallVerdict.NoFolder; return s; }
            s.ExePath = Path.Combine(folder, ExeName);
            s.PakPath = Path.Combine(folder, DataFolder, PakName);
            if (!files.FileExists(s.ExePath)) { s.Verdict = InstallVerdict.NoProgram; return s; }
            if (!files.FileExists(s.PakPath)) { s.Verdict = InstallVerdict.NoData; return s; }
            // A file that is there but cannot be opened is "no game data", not "no test scene": a locked or damaged
            // pak0.pak must never make the card tell the person their copy of the game lacks the demo.
            using (var pak = files.OpenRead(s.PakPath))
            {
                if (pak == null) { s.Verdict = InstallVerdict.NoData; return s; }
                s.Verdict = Pak.Has(pak, DemoEntry) ? InstallVerdict.Ready : InstallVerdict.NoDemo;
            }
            return s;
        }

        // Every Steam library, the install itself first. libraryfolders.vdf has had two shapes: a leaf per index
        // ("1" "D:\\SteamLibrary") and a block per index with a "path" key. Both are read; anything else is skipped.
        public static List<string> SteamLibraries(string steamPath, IBenchFiles files)
        {
            var list = new List<string>();
            if (string.IsNullOrEmpty(steamPath) || files == null) return list;
            Add(list, steamPath);
            var text = files.ReadText(Path.Combine(steamPath, "steamapps", "libraryfolders.vdf"), MaxVdfBytes);
            var root = Vdf.Parse(text);
            var folders = root == null ? null : root.Child("libraryfolders");
            if (folders == null) return list;
            foreach (var key in folders.Order)
            {
                if (list.Count >= MaxLibraries) break;
                var node = folders.Child(key);
                if (node == null) continue;
                Add(list, node.Value ?? node.Text("path"));
            }
            return list;
        }

        // The install folder of app 440900 in the first library whose manifest names it; null when no library has it.
        public static string SteamInstallFolder(string steamPath, IBenchFiles files)
        {
            foreach (var library in SteamLibraries(steamPath, files))
            {
                var apps = Path.Combine(library, "steamapps");
                var text = files.ReadText(Path.Combine(apps, "appmanifest_" + SteamAppId + ".acf"), MaxVdfBytes);
                var root = Vdf.Parse(text);
                var state = root == null ? null : root.Child("AppState");
                if (state == null) continue;
                var appid = state.Text("appid");
                if (appid != null && appid.Trim() != SteamAppId) continue;      // a manifest of another app
                var dir = state.Text("installdir");
                if (!PlainFolderName(dir)) continue;
                var folder = Path.Combine(apps, "common", dir.Trim());
                if (files.DirectoryExists(folder)) return folder;
            }
            return null;
        }

        // An "installdir" is one folder name: no separator, no drive, no "..", nothing a path could escape with.
        public static bool PlainFolderName(string name)
        {
            if (string.IsNullOrEmpty(name)) return false;
            var t = name.Trim();
            if (t.Length == 0 || t.Length > 128 || t == "." || t == "..") return false;
            if (t.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0) return false;
            return true;
        }

        static void Add(List<string> list, string path)
        {
            if (string.IsNullOrEmpty(path)) return;
            var clean = path.Trim().Replace('/', '\\').TrimEnd('\\');
            if (clean.Length == 0 || clean.IndexOfAny(Path.GetInvalidPathChars()) >= 0) return;
            if (!list.Any(p => string.Equals(p, clean, StringComparison.OrdinalIgnoreCase))) list.Add(clean);
        }
    }

    // ---- one pass -------------------------------------------------------------------------------------------------

    public enum RtApi { Pipeline, Query }

    public static class BenchPlan
    {
        public const int Width = 1280, Height = 720;
        public const int PassBoundMs = 180000;             // the bound of one pass
        // How often the chip is read while a pass runs. Two seconds is the interval the Performance page and the lab
        // harness both use, so a pass is watched exactly as closely as a lab run and no closer: a read of the driver
        // beside a running game costs the game time (BD-054), and the 10 s hold of the thermal rule needs no more.
        public const int SampleMs = 2000;
        // How long a pass waits for its game to be gone after it asked the job object to end it.
        public const int ExitBoundMs = 5000;
        // The slack the result log's write time is allowed against the start of the pass: file times are coarse on
        // some file systems, and this is the whole of the allowance.
        public const int FileTimeSlackMs = 2000;
        public static readonly RtApi[] Passes = { RtApi.Pipeline, RtApi.Query };

        public static string ApiName(RtApi api) { return api == RtApi.Query ? "query" : "pipeline"; }
        public static string LogName(RtApi api) { return "q2rtx-bench-" + ApiName(api); }

        // Where the game writes that log: its one writable directory, which the command line below points at us.
        public static string LogPath(string homeDir, RtApi api)
        {
            return Path.Combine(homeDir, Q2Rtx.DataFolder, "logs", LogName(api) + ".log");
        }

        // The command line of one pass. Everything is a command-line setting, so the card writes no file of the game
        // and changes nothing the person set in it: `basedir` is the game folder (read only), `homedir` is ours (the
        // only directory the game writes), and the settings are the ones the lab harness measures with
        // (scratch/pathtrace/lab/pt-run.ps1): windowed 1280x720, no dynamic resolution, no upscaler, full view, no
        // sound, the frame counter on, no vertical sync, one timed run of the shareware demo, then quit.
        public static string Arguments(string folder, string homeDir, RtApi api)
        {
            if (string.IsNullOrEmpty(folder) || string.IsNullOrEmpty(homeDir)) throw new ArgumentException("the benchmark needs a game folder and a home directory");
            if (folder.IndexOf('"') >= 0 || homeDir.IndexOf('"') >= 0) throw new ArgumentException("a quotation mark in a path is refused");
            var sets = new List<string>
            {
                "basedir " + Quote(folder), "homedir " + Quote(homeDir),
                "logfile 1", "logfile_flush 1", "logfile_name " + LogName(api),
                "vid_fullscreen 0", "vid_geometry " + Width + "x" + Height + "+0+0", "vid_modelist desktop",
                "ray_tracing_api " + ApiName(api), "drs_enable 0", "flt_fsr_enable 0", "viewsize 100",
                "scr_fps 1", "vid_vsync 0", "s_enable 0", "timedemo 1", "nextserver quit",
            };
            return string.Join(" ", sets.Select(s => "+set " + s)) + " +demo " + Q2Rtx.DemoName;
        }

        static string Quote(string path) { return "\"" + path.TrimEnd('\\') + "\""; }
    }

    public sealed class PassResult
    {
        public uint Frames;
        public double Seconds, Fps;
    }

    public static class BenchLog
    {
        public const int MaxBytes = 4 << 20;

        // The game prints its result with C's printf in the C locale, so the numbers always carry a dot, whatever the
        // language of Windows: "631 frames, 10.27 seconds: 61.417168 fps", the line the demo q2demo1 wrote on unit A.
        // The default log line prefix is a time stamp, so the pattern is searched for, never anchored.
        static readonly Regex Grammar = new Regex(@"(?<frames>\d{1,9}) frames,\s*(?<sec>\d{1,9}(?:\.\d{1,9})?) seconds:\s*(?<fps>\d{1,9}(?:\.\d{1,9})?) fps",
            RegexOptions.CultureInvariant);

        public static PassResult Parse(string line)
        {
            var m = Grammar.Match(line ?? "");
            if (!m.Success) return null;
            uint frames;
            double seconds, fps;
            if (!uint.TryParse(m.Groups["frames"].Value, NumberStyles.None, CultureInfo.InvariantCulture, out frames) ||
                !double.TryParse(m.Groups["sec"].Value, NumberStyles.AllowDecimalPoint, CultureInfo.InvariantCulture, out seconds) ||
                !double.TryParse(m.Groups["fps"].Value, NumberStyles.AllowDecimalPoint, CultureInfo.InvariantCulture, out fps)) return null;
            if (frames == 0 || seconds <= 0 || fps <= 0 || fps > 100000) return null;
            return new PassResult { Frames = frames, Seconds = seconds, Fps = fps };
        }

        // The last result of a whole log: a log of several timed runs ends with the one that finished last.
        public static PassResult Last(string log)
        {
            PassResult last = null;
            foreach (Match m in Grammar.Matches(log ?? ""))
            {
                var r = Parse(m.Value);
                if (r != null) last = r;
            }
            return last;
        }
    }

    // The thermal stop of a pass (owner, 2026-10-04): at 89 C at once, or at 87 C held for 10 s. A sample the driver
    // cannot give is never a temperature: it neither stops a pass nor clears a hold, which is the rule the lab
    // harness keeps as well.
    public sealed class ThermalGuard
    {
        public const int StopMc = 89000, HoldMc = 87000, HoldMs = 10000;

        long? _hotSince;

        public long? HotSinceMs { get { return _hotSince; } }

        public bool Hot(long nowMs, int? milliCelsius)
        {
            if (milliCelsius == null) return _hotSince != null && nowMs - _hotSince.Value >= HoldMs;
            if (milliCelsius.Value >= HoldMc) { if (_hotSince == null) _hotSince = nowMs; }
            else _hotSince = null;
            if (milliCelsius.Value >= StopMc) return true;
            return _hotSince != null && nowMs - _hotSince.Value >= HoldMs;
        }
    }

    // ---- the result of one pass and of one run, and the short history ---------------------------------------------

    public static class PassOutcome
    {
        public const string Done = "done", NoResult = "no-result", TimedOut = "timeout", TooHot = "hot", Cancelled = "cancelled", Failed = "failed";
        public static readonly string[] All = { Done, NoResult, TimedOut, TooHot, Cancelled, Failed };
        public static bool Known(string outcome) { return outcome != null && All.Contains(outcome); }
    }

    public sealed class PassRecord
    {
        public string Api = BenchPlan.ApiName(RtApi.Pipeline);
        public string Outcome = PassOutcome.Failed;
        public double? Fps, Seconds;
        public uint? Frames;
        public int? MaxTempC;
        public uint? MeanClockMHz;
        // The game was still there after the pass asked the job object to end it. A fact of the run only: it ends
        // the run (BenchmarkRun.Run) and is never written to the history, where it would mean nothing later.
        public bool Alive;
    }

    public sealed class RunRecord
    {
        public string Utc, Driver;
        public int Width = BenchPlan.Width, Height = BenchPlan.Height;
        public List<PassRecord> Passes = new List<PassRecord>();

        public PassRecord Pass(RtApi api)
        {
            var name = BenchPlan.ApiName(api);
            return Passes.FirstOrDefault(p => string.Equals(p.Api, name, StringComparison.Ordinal));
        }
    }

    // %LOCALAPPDATA%\amdgpu-wddm\benchmarks\q2rtx-history.json, newest run first, the newest 10 kept. A missing,
    // damaged or unknown-schema file is "no history" (the reader rule of docs/gui/interfaces.md): it never becomes a
    // result and never stops a new run from being saved.
    public static class BenchHistory
    {
        public const int Schema = 1, Keep = 10, MaxBytes = 1 << 20, MaxPasses = 8;
        public const string FileName = "q2rtx-history.json";
        public const string FolderName = "benchmarks";

        // The runs for the "Earlier runs" list: the saved runs except the one the card already shows in full above
        // it, which is the newest saved run as soon as a run has been saved (b23 lab round, F4). A run is named by
        // its time, so the run that is shown is the one with that time.
        public static List<RunRecord> Earlier(IList<RunRecord> history, RunRecord shown)
        {
            var list = new List<RunRecord>();
            if (history == null) return list;
            foreach (var r in history)
            {
                if (r == null) continue;
                if (shown != null && (r == shown || r.Utc != null && r.Utc == shown.Utc)) continue;
                list.Add(r);
            }
            return list;
        }

        public static List<RunRecord> Parse(string json)
        {
            var runs = new List<RunRecord>();
            if (string.IsNullOrEmpty(json) || json.Length > MaxBytes) return runs;
            object root;
            try { root = new System.Web.Script.Serialization.JavaScriptSerializer { MaxJsonLength = MaxBytes }.DeserializeObject(json); }
            catch (Exception) { return runs; }
            var map = root as IDictionary<string, object>;
            if (map == null) return runs;
            object v;
            if (!map.TryGetValue("schema", out v) || Int(v) != Schema) return runs;
            if (!map.TryGetValue("runs", out v)) return runs;
            var list = v as object[];
            if (list == null) return runs;
            foreach (var item in list)
            {
                var r = item as IDictionary<string, object>;
                if (r == null) continue;
                var run = new RunRecord { Utc = Str(r, "utc"), Driver = Str(r, "driver") };
                if (Recovery.Utc(run.Utc) == null) continue;              // a record without a readable time is no record
                int? w = Int(Get(r, "width")), h = Int(Get(r, "height"));
                if (w == null || h == null || w <= 0 || h <= 0 || w > 32768 || h > 32768) continue;
                run.Width = w.Value; run.Height = h.Value;
                var passes = Get(r, "passes") as object[];
                if (passes == null || passes.Length == 0 || passes.Length > MaxPasses) continue;
                bool bad = false;
                foreach (var p in passes)
                {
                    var d = p as IDictionary<string, object>;
                    if (d == null) { bad = true; break; }
                    var api = Str(d, "api");
                    var outcome = Str(d, "outcome");
                    if (api != BenchPlan.ApiName(RtApi.Pipeline) && api != BenchPlan.ApiName(RtApi.Query)) { bad = true; break; }
                    if (!PassOutcome.Known(outcome)) { bad = true; break; }
                    var record = new PassRecord { Api = api, Outcome = outcome, Fps = Num(Get(d, "fps")), Seconds = Num(Get(d, "seconds")) };
                    var frames = Int(Get(d, "frames"));
                    record.Frames = frames != null && frames >= 0 ? (uint?)frames.Value : null;
                    record.MaxTempC = Int(Get(d, "max_temp_c"));
                    var clock = Int(Get(d, "mean_clock_mhz"));
                    record.MeanClockMHz = clock != null && clock > 0 ? (uint?)clock.Value : null;
                    if (record.Fps != null && (record.Fps <= 0 || record.Fps > 100000)) record.Fps = null;
                    if (record.Outcome == PassOutcome.Done && record.Fps == null) { bad = true; break; }
                    run.Passes.Add(record);
                }
                if (bad) continue;
                runs.Add(run);
                if (runs.Count >= Keep) break;
            }
            return runs;
        }

        public static string Render(List<RunRecord> runs)
        {
            var body = new List<object>();
            foreach (var r in (runs ?? new List<RunRecord>()).Take(Keep))
            {
                var passes = new List<object>();
                foreach (var p in r.Passes.Take(MaxPasses))
                {
                    var d = new Dictionary<string, object> { { "api", p.Api }, { "outcome", p.Outcome } };
                    if (p.Fps != null) d["fps"] = Round(p.Fps.Value, 2);
                    if (p.Frames != null) d["frames"] = p.Frames.Value;
                    if (p.Seconds != null) d["seconds"] = Round(p.Seconds.Value, 2);
                    if (p.MaxTempC != null) d["max_temp_c"] = p.MaxTempC.Value;
                    if (p.MeanClockMHz != null) d["mean_clock_mhz"] = p.MeanClockMHz.Value;
                    passes.Add(d);
                }
                body.Add(new Dictionary<string, object>
                {
                    { "utc", r.Utc }, { "driver", r.Driver ?? "" }, { "width", r.Width }, { "height", r.Height }, { "passes", passes },
                });
            }
            return new System.Web.Script.Serialization.JavaScriptSerializer { MaxJsonLength = MaxBytes }
                .Serialize(new Dictionary<string, object> { { "schema", Schema }, { "runs", body } });
        }

        public static List<RunRecord> Add(List<RunRecord> runs, RunRecord run)
        {
            var list = new List<RunRecord>();
            if (run != null) list.Add(run);
            if (runs != null) list.AddRange(runs);
            return list.Take(Keep).ToList();
        }

        static double Round(double v, int digits) { return Math.Round(v, digits, MidpointRounding.AwayFromZero); }
        static object Get(IDictionary<string, object> d, string name) { object v; return d.TryGetValue(name, out v) ? v : null; }
        static string Str(IDictionary<string, object> d, string name) { return Get(d, name) as string; }

        static int? Int(object v)
        {
            if (v is int) return (int)v;
            if (v is long) { long l = (long)v; return l >= int.MinValue && l <= int.MaxValue ? (int?)l : null; }
            if (v is double) { double x = (double)v; return x >= int.MinValue && x <= int.MaxValue ? (int?)(int)x : null; }
            if (v is decimal) return (int)(decimal)v;
            return null;
        }

        static double? Num(object v)
        {
            if (v is double) return (double)v;
            if (v is int) return (int)v;
            if (v is long) return (long)v;
            if (v is decimal) return (double)(decimal)v;
            return null;
        }
    }

    // Our own directories: the game's one writable directory and the history file. The game folder is only read.
    public static class BenchPaths
    {
        // AMDGPU_WDDM_CONTROL_STATE moves the directory for the build's gates, in a lab build only (BD-167).
        static string Root
        {
            get
            {
#if AMDGPU_WDDM_LAB_FLAVOUR
                var over = Environment.GetEnvironmentVariable("AMDGPU_WDDM_CONTROL_STATE");
                var local = string.IsNullOrEmpty(over)
                    ? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "amdgpu-wddm")
                    : over;
#else
                var local = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "amdgpu-wddm");
#endif
                return Path.Combine(local, BenchHistory.FolderName);
            }
        }

        public static string HistoryFile { get { return Path.Combine(Root, BenchHistory.FileName); } }
        public static string Home { get { return Path.Combine(Root, "q2rtx-home"); } }

        public static List<RunRecord> ReadHistory()
        {
            try { return BenchHistory.Parse(File.Exists(HistoryFile) ? File.ReadAllText(HistoryFile, Encoding.UTF8) : null); }
            catch (Exception) { return new List<RunRecord>(); }
        }

        // The newest run first, the newest 10 kept. A failed write loses the history and nothing else.
        public static List<RunRecord> SaveRun(RunRecord run)
        {
            var list = BenchHistory.Add(ReadHistory(), run);
            try
            {
                Directory.CreateDirectory(Root);
                var temp = HistoryFile + "." + Guid.NewGuid().ToString("N").Substring(0, 8) + ".tmp";
                File.WriteAllText(temp, BenchHistory.Render(list), new UTF8Encoding(false));
                if (File.Exists(HistoryFile)) File.Replace(temp, HistoryFile, null, true); else File.Move(temp, HistoryFile);
            }
            catch (Exception) { }
            return list;
        }
    }

    public sealed class LocalBenchFiles : IBenchFiles
    {
        public bool DirectoryExists(string path) { try { return Directory.Exists(path); } catch (Exception) { return false; } }
        public bool FileExists(string path) { try { return File.Exists(path); } catch (Exception) { return false; } }

        public Stream OpenRead(string path)
        {
            try { return new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete); }
            catch (Exception) { return null; }
        }

        public string ReadText(string path, int maxBytes)
        {
            try
            {
                var info = new FileInfo(path);
                if (!info.Exists || info.Length > maxBytes) return null;
                using (var s = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                using (var r = new StreamReader(s, Encoding.UTF8)) return r.ReadToEnd();
            }
            catch (Exception) { return null; }
        }
    }

    // Where the copy of the game is: the folder the person chose, else Steam's.
    public static class BenchLocator
    {
        public const string SteamKey = @"Software\Valve\Steam";

        public static string SteamPath()
        {
            try
            {
                using (var k = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(SteamKey))
                {
                    var v = k == null ? null : k.GetValue("SteamPath") as string;
                    return string.IsNullOrEmpty(v) ? null : v.Replace('/', '\\');
                }
            }
            catch (Exception) { return null; }
        }

        public static InstallState Find(string chosen, IBenchFiles files)
        {
            if (!string.IsNullOrEmpty(chosen)) return Q2Rtx.Inspect(chosen, files);
            var steam = Q2Rtx.SteamInstallFolder(SteamPath(), files);
            if (steam == null) return new InstallState();
            var state = Q2Rtx.Inspect(steam, files);
            state.FromSteam = true;
            return state;
        }
    }

    public sealed class BenchProgress
    {
        public const string Idle = "idle", Starting = "starting", Running = "running", Reading = "reading", Finished = "finished";

        public string Stage = Idle;
        public bool Busy;
        public string Pass;                 // the API name of the pass that runs; null between passes
        public int ElapsedMs;
        public RunRecord Run;               // the passes decided so far; complete when Stage is Finished
    }

    // Whether the temperature and the clock sampled around a pass belong to that pass.
    //
    // A pass reads the chip once before it starts its game, to refuse a start on a hot chip. Where the pass then
    // ends before the game runs, that reading says nothing about the pass, and the card showed "74 C, 1300 MHz"
    // for a way that never ran (b23 lab round 2, F8). The one exception is the pass that was refused because the
    // chip was too hot: there the reading is the reason, so it is kept and shown.
    public static class PassTelemetry
    {
        public static bool Keep(string outcome, bool gameStarted)
        {
            return gameStarted || outcome == PassOutcome.TooHot;
        }
    }

    // The state line of the card: which string tells the person where the test stands. The card shows one run in
    // full, the run that goes on or the newest saved one, and this line says what happened to that run; it never
    // says that nothing has run while a saved run is on the card (b23 lab round, F2 and F3).
    public static class BenchState
    {
        public const string Idle = "bench.state.idle", Starting = "bench.state.starting", Running = "bench.state.running",
            Reading = "bench.state.reading", Done = "bench.state.done", Stopped = "bench.state.stopped", Saved = "bench.state.saved";

        // stage: the stage of this session's run (BenchProgress.Stage), or null where this session started none.
        // shown: the run the card shows, which is this session's when thisSession is true and a saved one otherwise.
        // Running needs the name of the pass; without one the line is the one of a run that is starting.
        public static string Id(string stage, string pass, RunRecord shown, bool thisSession)
        {
            if (stage == BenchProgress.Starting) return Starting;
            if (stage == BenchProgress.Running) return pass != null ? Running : Starting;
            if (stage == BenchProgress.Reading) return Reading;
            if (shown == null || shown.Passes == null || shown.Passes.Count == 0) return Idle;
            if (shown.Passes.Any(p => p != null && p.Outcome == PassOutcome.Cancelled)) return Stopped;
            return thisSession ? Done : Saved;
        }
    }

    // The numbers of a run in the window's words. Culture-aware on the way out (a Polish reader sees 60,9), always
    // culture-invariant on the way in (BenchLog).
    public static class BenchText
    {
        public static string Fps(double fps) { return fps.ToString("0.0", CultureInfo.CurrentCulture); }
        public static string Seconds(double seconds) { return seconds.ToString("0.0", CultureInfo.CurrentCulture); }
        public static string Reference(double fps) { return fps.ToString("0.0", CultureInfo.CurrentCulture); }
    }

    // What this release measured on the project's own BC-250, for comparison only. The card shows it on a board it
    // has positively identified as a BC-250 (the same identification the board memory card uses) and nowhere else:
    // another card has other hardware and the number would mean nothing there.
    //
    // A band, not two single numbers. Twenty passes of the two ways over two lab rounds gave 57.8 to 69.7 pictures
    // a second, with the two samples of the validation suite inside it. The number of a pass follows the clock the
    // chip reached while the scene ran (1220 to 1378 MHz at a ceiling of 2000), and ten seconds is a short scene
    // for a chip that raises its clock under load, so the two ways lie inside one band instead of having a number
    // each. Two single numbers promised a precision this board does not have: three of eight passes of the first
    // way came out above the one that was printed (b23 lab round 2, F6).
    public static class BenchReference
    {
        public const double LowFps = 57.8, HighFps = 69.7;
        public const int Width = BenchPlan.Width, Height = BenchPlan.Height;
        public const string Release = "0.7.216.100-tester.27";
    }
}
