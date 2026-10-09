// Active graphics components are identified from DWM's loaded module and its
// SHA256 build manifest. Registry configuration alone is not runtime proof.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Globalization;
using System.Security.Cryptography;
using System.Text.RegularExpressions;
using System.Threading;
using System.Web.Script.Serialization;

namespace Bc250Mon
{
    public sealed class GraphicsPipelineProvider : IProvider
    {
        public sealed class BuildInfo
        {
            public string Renderer { get; set; }
            public string Execution { get; set; }
            public string Compiler { get; set; }
            public string Build { get; set; }
        }
        public const string SummaryPauseFileName = "graphics-summary.pause";
        readonly string _manifest, _summaryPausePath;
        string _lastSummaryOutput;
        DateTime _lastSummaryUtc;
        // The last counter block this session saw, and when. BD-097: from KMD 0.7.216.24 a requested summary
        // writes its block beside the log ring, so a page read cannot find it again.
        string _keptCounters;
        DateTime _keptCountersUtc;
        int _summaryRequested;
        string _cachedPath, _cachedHash;
        DateTime _cachedWrite;
        public GraphicsPipelineProvider(string dataDir)
        {
            _manifest = Path.Combine(dataDir, "graphics-modules.json");
            _summaryPausePath = Path.Combine(dataDir, SummaryPauseFileName);
        }
        public string Name { get { return "graphics"; } }
        public TimeSpan Period { get { return TimeSpan.FromSeconds(5); } }

        public void Poll(State state)
        {
            var panel = new Panel { Name = Name, Title = "Active graphics pipeline", Order = 12 };
            var dwm = Process.GetProcessesByName("dwm").OrderByDescending(p => p.SessionId).FirstOrDefault();
            if (dwm == null) panel.Rows.Add(new Row("DWM", "not running", Level.Warn));
            else using (dwm)
            {
                panel.Rows.Add(new Row("DWM", "PID " + dwm.Id + ", since " + dwm.StartTime.ToString("HH:mm:ss")));
                string path = null, zink = null;
                bool radv = false;
                try
                {
                    foreach (ProcessModule m in dwm.Modules)
                    {
                        string n = m.ModuleName;
                        if (n.Equals("bc250d3d.dll", StringComparison.OrdinalIgnoreCase)) path = m.FileName;
                        else if (n.Equals("bc250d3d_zink.dll", StringComparison.OrdinalIgnoreCase)) zink = m.FileName;
                        else if (n.Equals("amdgpu_wddm_radv.dll", StringComparison.OrdinalIgnoreCase) ||
                                 n.Equals("vulkan_radeon.dll", StringComparison.OrdinalIgnoreCase)) radv = true;
                    }
                }
                catch { panel.Rows.Add(new Row("Modules", "cannot inspect current DWM", Level.Warn)); }
                // The GPU desktop route (owner 2026-10-01): DWM maps the zink build of the UMD and draws through RADV.
                // The zink build changes with every Mesa update, so the module names, not a hash list, identify it.
                if (zink != null)
                {
                    panel.Rows.Add(new Row("D3D UMD", "bc250d3d_zink / " + new DirectoryInfo(Path.GetDirectoryName(zink)).Name));
                    panel.Rows.Add(radv ? new Row("Renderer", "Mesa zink on RADV (GPU)", Level.Good)
                                        : new Row("Renderer", "zink loaded, RADV not loaded", Level.Warn));
                }
                else if (path == null) panel.Rows.Add(new Row("Renderer", "not identified in DWM", Level.Warn));
                else
                {
                    DateTime write = File.GetLastWriteTimeUtc(path);
                    if (_cachedPath != path || _cachedWrite != write)
                    {
                        using (var file = File.OpenRead(path))
                        using (var sha = SHA256.Create())
                            _cachedHash = BitConverter.ToString(sha.ComputeHash(file)).Replace("-", "");
                        _cachedPath = path; _cachedWrite = write;
                    }
                    panel.Rows.Add(new Row("D3D UMD", "bc250d3d / " + new DirectoryInfo(Path.GetDirectoryName(path)).Name));
                    BuildInfo build = null;
                    if (File.Exists(_manifest))
                    {
                        var known = new JavaScriptSerializer().Deserialize<Dictionary<string, BuildInfo>>(File.ReadAllText(_manifest));
                        if (known != null) known.TryGetValue(_cachedHash, out build);
                    }
                    panel.Rows.Add(new Row("Build hash", _cachedHash.Substring(0, 12)));
                    if (build == null) panel.Rows.Add(new Row("Renderer", "unknown build; no CPU/GPU claim", Level.Warn));
                    else
                    {
                        panel.Rows.Add(new Row("Renderer", build.Renderer));
                        panel.Rows.Add(new Row("Draws", build.Execution, Level.Warn));
                        panel.Rows.Add(new Row("Shaders", build.Compiler));
                        panel.Rows.Add(new Row("UMD build", build.Build));
                    }
                }
            }
            var kmd = state.Take(0).Panels.FirstOrDefault(p => p.Name == "kmdinfo");
            if (kmd != null)
            {
                var version = kmd.Rows.FirstOrDefault(r => r.Label == "Version");
                if (version != null) panel.Rows.Add(new Row("KMD module", "bc250kmd " + version.Value.Split(' ')[0]));
                var mode = kmd.Rows.FirstOrDefault(r => r.Label == "Mode");
                panel.Rows.Add(new Row("KMD table", mode == null ? "unknown" :
                    mode.Value.Contains("FULL WDDM") ? "full WDDM" : "display-only"));
            }
            AddKernelSummary(panel);
            panel.Rows.Add(new Row("Sampled", DateTime.Now.ToString("HH:mm:ss") + " (pipeline, 5 s poll)"));
            state.SetPanel(panel);
        }

        // Opt-in A/B control: creating the marker stops the KMD log subprocess on the
        // next poll. An already running call is allowed to finish. Keep the last
        // counters visibly marked as cached.
        // Typed start health and native clocks run in separate unchanged providers.
        //
        // The schedule reads the ring with "log 0": GET_LOG pages, which the driver
        // answers with NoAdapterSynchronization alone and which touch no hardware. The
        // ring is 1024 lines and a page is 64, so a poll costs up to 16 such escapes,
        // each a 10 KB copy under the log's own spin lock. That is more pages than the
        // three a summary read took, and no GPU idle at all; if the lab ever shows the
        // copies themselves, the cheap next step is a cursor - remember the sequence
        // the last summary block started at and read "log <that>" instead of "log 0".
        // "log summary only" is the other form, and it left this schedule in 0.7.213
        // (C55). Its LOG_SUMMARY escape is a Level Two call, so dxgkrnl suspends the
        // GPU scheduler for it, and polling it every 5.4 s cost a running game a
        // 300 ms stall each time and about 2 % of its frame rate (trial 41). It now
        // goes out once per explicit operator request, through the "graphics.summary"
        // action. The counters below therefore come from whatever summary is still in
        // the ring, and the "KMD counters" row says how old that is.
        //
        // The CLI's last line counts the escapes of each kind, and a poll whose count
        // is not the one its form should produce, or which printed no count at all, is
        // a warning (BD-054: a CLI from before 0.7.184.1 sent all 16 pages as Level
        // Two calls, 280-420 ms per poll).
        internal const string SummaryArgs = "log summary only";
        internal const string PageArgs = "log 0";
        static readonly Regex EscapeCounts = new Regex(@"escapes: (\d+) without adapter synchronization, (\d+) with HardwareAccess");
        // Every ring line bc250kmd_cli prints is "<sequence> <seconds>.<ms> <text>", and every line the summary
        // itself writes begins "wddm summary:" (wddm.c WddmSummary). That is what dates the counter block found
        // inside a page read, against the newest line the same read brought back.
        static readonly Regex SummaryStamp = new Regex(@"(?m)^\s*\d+\s+(\d+)\.(\d{3}) wddm summary:");
        static readonly Regex AnyStamp = new Regex(@"(?m)^\s*\d+\s+(\d+)\.(\d{3}) ");

        // One LOG_SUMMARY on the next poll, because a person asked for it: the overlay's action table routes
        // mon.py, the buttons and the HTTP API here (Actions "graphics.summary"). Several requests between two
        // polls are one summary. Returns false when the pause marker exists, because then the panel reads nothing
        // at all and the request would be dropped with it: the caller says so instead of promising a summary.
        public bool RequestSummary()
        {
            if (File.Exists(_summaryPausePath)) return false;
            Interlocked.Exchange(ref _summaryRequested, 1);
            return true;
        }

        internal void AddKernelSummary(Panel panel)
        {
            bool paused = File.Exists(_summaryPausePath);
            bool summary = Interlocked.Exchange(ref _summaryRequested, 0) != 0 && !paused;
            string args = summary ? SummaryArgs : PageArgs;
            string output = _lastSummaryOutput, error;
            int exit = output == null ? -1 : 0;
            if (!paused)
            {
                exit = KmdInfoProvider.Run(KmdInfoProvider.CliPath, args, 5000, out output, out error);
                if (exit == 0)
                {
                    _lastSummaryOutput = output;
                    _lastSummaryUtc = DateTime.UtcNow;
                    // From KMD 0.7.216.24 a requested summary writes its block beside the log ring, not into it
                    // (BD-097, docs/design/kmd-log-ring.md): one poll's 320 lines used to rotate the ring in
                    // about 12 s. So a later page read finds no block to parse, and the counters of the last
                    // summary are kept here instead. An older driver puts the block in the ring, and then this
                    // keeps what the page read itself brought back, which is the same thing.
                    if (summary || SummaryStamp.IsMatch(output ?? ""))
                    {
                        _keptCounters = output;
                        _keptCountersUtc = DateTime.UtcNow;
                    }
                    AddEscapeCheck(panel, output, summary ? "1" : "0");
                    AddCounterAge(panel, output, summary, _keptCounters == null ? (DateTime?)null : _keptCountersUtc);
                }
                // A read that failed wrote no summary, so an operator's request is not spent. LOG_SUMMARY under a
                // running game is the slow case and the subprocess has 5 s, so a timeout here is the likely one:
                // without this the request is thrown away and the panel keeps the same old counters, with nothing
                // to tell that apart from a summary that was written.
                else if (summary) Interlocked.Exchange(ref _summaryRequested, 1);
            }
            else panel.Rows.Add(new Row("KMD counters", output == null ? "paused; no cached snapshot" :
                "paused; snapshot " + _lastSummaryUtc.ToLocalTime().ToString("HH:mm:ss"), Level.Warn));
            // Which text the counter rows below are read from: this read's own block when it holds one, and the
            // kept block of the last summary when it does not (BD-097: the block lives beside the ring now).
            if (exit == 0 && !summary && !SummaryStamp.IsMatch(output ?? "") && _keptCounters != null)
                output = _keptCounters;
            if (exit == 0)
            {
                string flip = Last(output, @"vidpn flip (open|closed): (\d+) hardware flips");
                var match = Regex.Match(flip, @"vidpn flip (open|closed): (\d+) hardware flips");
                panel.Rows.Add(new Row("Scanout", match.Success ?
                    (match.Groups[1].Value == "open" ? (match.Groups[2].Value == "0" ? "DCN flip enabled; no flips yet" : "DCN flip + hardware VSync") : "hardware flip disabled") : "not reported"));
                if (match.Success) panel.Rows.Add(new Row("HW flips", match.Groups[2].Value));
                string blit = Last(output, @"blit gate (open|closed), (\d+) blits");
                var b = Regex.Match(blit, @"blit gate (open|closed), (\d+) blits");
                panel.Rows.Add(new Row("Window Blt", b.Success ?
                    (b.Groups[1].Value == "open" ? "CPU copy enabled; " + b.Groups[2].Value + " blits" : "disabled") : "not reported"));
                string gfx = Last(output, @"node 0 hardware: (\d+) submitted, (\d+) completed");
                string paging = Last(output, @"node 1 \(paging, open\): (\d+) hardware submitted, (\d+) completed");
                var g = Regex.Match(gfx, @"(\d+) submitted, (\d+) completed");
                var p = Regex.Match(paging, @"(\d+) hardware submitted, (\d+) completed");
                panel.Rows.Add(new Row("GPU compute", g.Success ? g.Groups[2].Value + "/" + g.Groups[1].Value + " fences completed" : "not reported"));
                panel.Rows.Add(new Row("Paging", p.Success ? "SDMA " + p.Groups[2].Value + "/" + p.Groups[1].Value : "hardware path not reported"));
            }
            // Exit 2 is the CLI's bad usage: on a requested summary, one that does not know "only" yet; on a page
            // read, one that did not take the command at all.
            else if (!paused) panel.Rows.Add(new Row("KMD live", exit != 2 ? "log unavailable" : summary ?
                "summary unavailable: " + KmdInfoProvider.CliPath + " predates \"" + SummaryArgs + "\"" :
                "log unavailable: " + KmdInfoProvider.CliPath + " did not accept \"" + PageArgs + "\"", Level.Warn));
        }
        // `want` is the number of Level Two escapes this form of the read is allowed to cost: none for a "log 0"
        // page read, one for a requested summary.
        static void AddEscapeCheck(Panel panel, string output, string want)
        {
            var m = EscapeCounts.Match(output ?? "");
            if (!m.Success)
                panel.Rows.Add(new Row("KMD poll", "no escape counts: the CLI predates them; its log pages may idle the GPU", Level.Warn));
            else if (m.Groups[2].Value != want)
                panel.Rows.Add(new Row("KMD poll", m.Groups[2].Value + " Level Two escapes, expected " + want, Level.Warn));
            else if (want == "0")
                panel.Rows.Add(new Row("KMD poll", "no Level Two escape, " + m.Groups[1].Value + " without adapter synchronization", Level.Good));
            else
                panel.Rows.Add(new Row("KMD poll", "1 Level Two escape, " + m.Groups[1].Value + " without adapter synchronization", Level.Good));
        }
        // Where the counters come from. A page read never writes a summary, so its block is as old as the last
        // summary anybody asked for; an unmarked stale block would read as a sample of this poll.
        static void AddCounterAge(Panel panel, string output, bool summary, DateTime? kept)
        {
            if (summary) { panel.Rows.Add(new Row("KMD counters", "summary written by this poll, on request", Level.Good)); return; }
            var last = LastMatch(SummaryStamp, output);
            if (last == null)
            {
                // No block in the ring. From KMD 0.7.216.24 that is the normal state, because a requested
                // summary writes beside the ring, so the rows come from the block this session kept.
                panel.Rows.Add(kept == null ?
                    new Row("KMD counters", "no summary yet; the graphics.summary action writes one", Level.Warn) :
                    new Row("KMD counters", "kept from the summary of " + kept.Value.ToLocalTime().ToString("HH:mm:ss"),
                            Level.Info));
                return;
            }
            var newest = LastMatch(AnyStamp, output);
            long age = newest == null ? -1 : Milliseconds(newest) - Milliseconds(last);
            panel.Rows.Add(new Row("KMD counters", age < 0 ? "from the ring's last summary, age unknown" :
                "from the ring's last summary, " + (age / 1000) + " s before the newest log line",
                age < 0 || age > 600000 ? Level.Warn : Level.Info));
        }
        static Match LastMatch(Regex pattern, string text)
        {
            var matches = pattern.Matches(text ?? "");
            return matches.Count == 0 ? null : matches[matches.Count - 1];
        }
        // The driver's own clock for the line: seconds and milliseconds since this driver load.
        static long Milliseconds(Match stamp)
        {
            return long.Parse(stamp.Groups[1].Value, CultureInfo.InvariantCulture) * 1000 +
                   long.Parse(stamp.Groups[2].Value, CultureInfo.InvariantCulture);
        }
        static string Last(string text, string pattern)
        {
            var matches = Regex.Matches(text ?? "", pattern);
            return matches.Count == 0 ? "" : matches[matches.Count - 1].Value;
        }
    }
}
