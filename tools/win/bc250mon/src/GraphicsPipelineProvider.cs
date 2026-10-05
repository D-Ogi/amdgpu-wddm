// Active graphics components are identified from DWM's loaded module and its
// SHA256 build manifest. Registry configuration alone is not runtime proof.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text.RegularExpressions;
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

        // Opt-in A/B control: creating the marker stops only the synchronized
        // LOG_SUMMARY subprocess on the next poll. An already running call is
        // allowed to finish. Keep the last counters visibly marked as cached.
        // Typed start health and native clocks run in separate unchanged providers.
        //
        // "log summary only" reads the summary's own lines, not the whole ring. Its
        // LOG_SUMMARY escape is a Level Two call, which idles the GPU under a running
        // game; every page after it must go without adapter synchronization. The
        // CLI's last line counts both kinds, and more than one Level Two escape per
        // poll, or no count at all, is shown as a warning (BD-054: a CLI from before
        // 0.7.184.1 at CliPath sent all 16 pages as Level Two, 280-420 ms per poll).
        internal const string SummaryArgs = "log summary only";
        static readonly Regex EscapeCounts = new Regex(@"escapes: (\d+) without adapter synchronization, (\d+) with HardwareAccess");
        internal void AddKernelSummary(Panel panel)
        {
            bool paused = File.Exists(_summaryPausePath);
            string output = _lastSummaryOutput, error;
            int exit = output == null ? -1 : 0;
            if (!paused)
            {
                exit = KmdInfoProvider.Run(KmdInfoProvider.CliPath, SummaryArgs, 5000, out output, out error);
                if (exit == 0)
                {
                    _lastSummaryOutput = output;
                    _lastSummaryUtc = DateTime.UtcNow;
                    AddEscapeCheck(panel, output);
                }
            }
            else panel.Rows.Add(new Row("KMD counters", output == null ? "paused; no cached snapshot" :
                "paused; snapshot " + _lastSummaryUtc.ToLocalTime().ToString("HH:mm:ss"), Level.Warn));
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
            // Exit 2 is the CLI's bad usage: one that does not know "only" yet.
            else if (!paused) panel.Rows.Add(new Row("KMD live", exit == 2 ?
                "summary unavailable: " + KmdInfoProvider.CliPath + " predates \"" + SummaryArgs + "\"" :
                "summary unavailable", Level.Warn));
        }
        static void AddEscapeCheck(Panel panel, string output)
        {
            var m = EscapeCounts.Match(output ?? "");
            if (!m.Success)
                panel.Rows.Add(new Row("KMD poll", "no escape counts: the CLI predates them; its log pages may idle the GPU", Level.Warn));
            else if (m.Groups[2].Value != "1")
                panel.Rows.Add(new Row("KMD poll", m.Groups[2].Value + " Level Two escapes per poll, expected 1 (the summary)", Level.Warn));
            else
                panel.Rows.Add(new Row("KMD poll", "1 Level Two escape, " + m.Groups[1].Value + " without adapter synchronization", Level.Good));
        }
        static string Last(string text, string pattern)
        {
            var matches = Regex.Matches(text ?? "", pattern);
            return matches.Count == 0 ? "" : matches[matches.Count - 1].Value;
        }
    }
}
