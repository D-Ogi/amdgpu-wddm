// "Create bug report": collect everything in memory, redact it, show the tester the file list, then write one zip to
// the Desktop. Sources: the KMD's log ring and software snapshots (no Level Two escape), the installed files with
// versions and SHA-256, the driver's registry settings, dxdiag /t, System and Application events of the last 24 hours
// from the display stack, and the D3D12 and Vulkan capability tools when they are present. No network.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Diagnostics.Eventing.Reader;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text;

namespace AmdgpuWddmControl
{
    public sealed class ReportEntry
    {
        public string Name, Description;
        public byte[] Data;
    }

    public sealed class BugReport
    {
        public readonly List<ReportEntry> Entries = new List<ReportEntry>();
        readonly Redactor _redact;

        public BugReport(Redactor redact) { _redact = redact; }

        void Add(string name, string description, string text)
        {
            Entries.Add(new ReportEntry { Name = name, Description = description, Data = Encoding.UTF8.GetBytes(_redact.Apply(text)) });
        }

        public static string DefaultPath(DateTime now)
        {
            var desktop = Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory);
            return Path.Combine(desktop, "amdgpu-wddm-report-" + now.ToString("yyyyMMdd-HHmmss") + ".zip");
        }

        public void Collect(bool dxdiag, bool caps, bool events, Action<string> progress)
        {
            progress("Reading the driver state");
            Add("driver-state.txt", "Driver snapshots: clocks, temperature, DPM, desktop composition, start health", DriverState());
            progress("Reading the driver log");
            Add("driver-log.txt", "The kernel driver's log ring", DriverLog());
            progress("Reading installed files");
            var inventory = Inventory.Read(true);
            Add("installed-files.txt", "Driver files with versions and SHA-256", Inventory.Report(inventory));
            Add("settings.txt", "Driver settings in the registry and D3D12 application profiles", Settings());
            Add("system.txt", "Windows version, test signing, this application's version", SystemInfo());
            if (events)
            {
                progress("Reading the event logs");
                Add("events.txt", "System and Application events of the last 24 hours from the display drivers", Events(TimeSpan.FromHours(24)));
            }
            if (dxdiag) { progress("Running dxdiag (up to 2 minutes)"); Add("dxdiag.txt", "DirectX diagnostic report (dxdiag /t)", DxDiag()); }
            if (caps)
            {
                progress("Running the capability checks");
                Add("d3d12-caps.json", "D3D12 and DXGI capabilities (amdgpu_wddm_d3d12caps)", D3d12Caps());
                Add("vulkan-summary.txt", "Vulkan devices (vulkaninfo --summary)", VulkanSummary());
            }
            progress("Ready");
        }

        public void Write(string path)
        {
            var temp = path + ".partial";
            using (var file = new FileStream(temp, FileMode.Create, FileAccess.Write))
            using (var zip = new ZipArchive(file, ZipArchiveMode.Create))
                foreach (var e in Entries)
                    using (var s = zip.CreateEntry(e.Name, CompressionLevel.Optimal).Open())
                        s.Write(e.Data, 0, e.Data.Length);
            if (File.Exists(path)) File.Delete(path);
            File.Move(temp, path);
        }

        // ---- sources -------------------------------------------------------------------------------------------

        public static string DriverState()
        {
            var w = new StringBuilder();
            var dpm = Kmd.Dpm();
            if (dpm.Value == null) w.AppendLine("dpm: " + dpm.Error + " (0x" + ((uint)dpm.Status).ToString("X8") + ")");
            else
            {
                var d = dpm.Value;
                w.AppendLine("kmd version: " + KmdReply.VersionText(d.Version) + " (0x" + d.Version.ToString("X8") + ")");
                w.AppendLine(string.Format("dpm: flags 0x{0:X} mode {1} requested {2} reason {3} throttle {4} max {5} cap {6} target {7} want {8} current {9} MHz {10} mV observed {11} MHz vid {12} temperature_mc {13} busy {14}/{15} permille thermal_events {16} errors {17} uptime_ms {18} generation {19}",
                    d.Flags, d.Mode, d.Requested, d.Reason, d.Throttle, d.MaxMHz, d.CapMHz, d.TargetMHz, d.WantMHz, d.CurrentMHz, d.CurrentMv,
                    d.ObservedMHz, d.ObservedVid, d.TemperatureMc, d.BusyPermille, d.BusyAvgPermille, d.ThermalEvents, d.Errors, d.UptimeMs, d.Generation));
            }
            var health = Kmd.StartHealth();
            w.AppendLine(health.Value == null ? "start health: " + health.Error :
                string.Format("start health: flags 0x{0:X} generation {1} completed {2} ready_age_ms {3}", health.Value.Flags, health.Value.Generation, health.Value.Completed, health.Value.ReadyAgeMs));
            var interop = Kmd.Interop();
            w.AppendLine(interop.Value == null ? "interop: " + interop.Error :
                string.Format("interop: flags 0x{0:X} requested {1} effective {2} reason {3} closed_reason {4} users {5} ({6})", interop.Value.Flags, interop.Value.Requested,
                    interop.Value.Effective, interop.Value.Reason, interop.Value.ClosedReason, interop.Value.Users, KmdReply.CompositionText(interop.Value)));
            var vram = Kmd.VideoMemory();
            w.AppendLine(vram.Value == null ? "video memory: " + vram.Error :
                string.Format("video memory: segments {0} local resident {1} committed {2} limit {3} aperture resident {4} limit {5} dedicated {6}", vram.Value.Segments,
                    vram.Value.LocalResident, vram.Value.LocalCommitted, vram.Value.LocalLimit, vram.Value.ApertureResident, vram.Value.ApertureLimit, vram.Value.Dedicated));
            return w.ToString();
        }

        public static string DriverLog()
        {
            var w = new StringBuilder();
            uint from = 0;
            int pages = 0, lines = 0;
            while (pages++ < 64)        // the ring is 1024 lines, 64 a page: 16 pages, the rest is a live stream
            {
                var page = Kmd.LogPage(from);
                if (page.Value == null) { w.AppendLine("log read stopped: " + page.Error + " (0x" + ((uint)page.Status).ToString("X8") + ")"); break; }
                if (pages == 1) w.AppendLine(string.Format("# {0} lines since the driver loaded, {1} lost to the wrap, {2} dropped above DISPATCH_LEVEL", page.Value.Total, page.Value.Lost, page.Value.Above));
                foreach (var l in page.Value.Lines) { w.AppendLine(string.Format("{0,6} {1,6}.{2:000} {3}", l.Sequence, l.Milliseconds / 1000, l.Milliseconds % 1000, l.Text)); lines++; }
                if (page.Value.Returned == 0 || page.Value.Next <= from) break;
                from = page.Value.Next;
            }
            w.AppendLine("# " + lines + " lines read");
            return w.ToString();
        }

        static string Settings()
        {
            var w = new StringBuilder();
            foreach (var key in new[] { DpmSettings.RegistryPath })
            {
                w.AppendLine("[HKLM\\" + key + "]");
                try { foreach (var kv in SettingsStore.ReadAll(key).OrderBy(k => k.Key)) w.AppendLine(kv.Key + " = " + kv.Value); }
                catch (Exception e) { w.AppendLine("unreadable: " + e.Message); }
            }
            w.AppendLine("[HKLM\\" + Profiles.RegistryPath + "]");
            try { foreach (var kv in SettingsStore.ReadProfiles()) w.AppendLine(kv.Key + " Experiment = " + kv.Value); }
            catch (Exception e) { w.AppendLine("unreadable: " + e.Message); }
            return w.ToString();
        }

        static string SystemInfo()
        {
            var w = new StringBuilder();
            w.AppendLine("os: " + Environment.OSVersion.VersionString + (Environment.Is64BitOperatingSystem ? " x64" : ""));
            try
            {
                using (var k = Microsoft.Win32.Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Microsoft\Windows NT\CurrentVersion"))
                    if (k != null) w.AppendLine("build: " + k.GetValue("CurrentBuild") + "." + k.GetValue("UBR") + " " + k.GetValue("DisplayVersion"));
            }
            catch (Exception) { }
            var ts = Kmd.TestSigning();
            w.AppendLine("test signing: " + (ts == null ? "unknown" : ts.Value ? "on" : "off"));
            w.AppendLine("control application: " + Program.VersionText);
            w.AppendLine("report time (UTC): " + DateTime.UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ"));
            return w.ToString();
        }

        public static readonly string[] SystemProviders =
        {
            "bc250kmd", "Display", "Microsoft-Windows-DxgKrnl", "Microsoft-Windows-Dwm-Core",
            "Microsoft-Windows-WER-SystemErrorReporting", "Microsoft-Windows-Kernel-Power", "Microsoft-Windows-Kernel-PnP",
        };

        // System: the providers above. Application: crash records (Application Error, Windows Error Reporting) that
        // name a module or process of the display stack.
        public static string Events(TimeSpan window)
        {
            var w = new StringBuilder();
            long ms = (long)window.TotalMilliseconds;
            string providers = string.Join(" or ", SystemProviders.Select(p => "@Name='" + p + "'"));
            Query(w, "System", "*[System[Provider[" + providers + "] and TimeCreated[timediff(@SystemTime) <= " + ms + "]]]", null);
            Query(w, "Application", "*[System[Provider[@Name='Application Error' or @Name='Windows Error Reporting'] and TimeCreated[timediff(@SystemTime) <= " + ms + "]]]",
                text => new[] { "amdgpu", "bc250", "dwm.exe", "d3d12", "dxgi", "vulkan", "zink" }.Any(k => text.IndexOf(k, StringComparison.OrdinalIgnoreCase) >= 0));
            return w.ToString();
        }

        static void Query(StringBuilder w, string log, string xpath, Func<string, bool> keep)
        {
            w.AppendLine("==== " + log);
            int n = 0;
            try
            {
                using (var reader = new EventLogReader(new EventLogQuery(log, PathType.LogName, xpath) { ReverseDirection = true }))
                    for (EventRecord r = reader.ReadEvent(); r != null && n < 2000; r = reader.ReadEvent())
                        using (r)
                        {
                            string text;
                            try { text = r.FormatDescription(); } catch (Exception) { text = null; }
                            if (text == null) text = string.Join(" | ", r.Properties.Select(p => Convert.ToString(p.Value)));
                            if (keep != null && !keep(text)) continue;
                            w.AppendLine(string.Format("{0:yyyy-MM-ddTHH:mm:ss.fffZ} {1} id {2} level {3}: {4}", r.TimeCreated.HasValue ? r.TimeCreated.Value.ToUniversalTime() : DateTime.MinValue,
                                r.ProviderName, r.Id, r.Level, text.Replace("\r", "").Replace("\n", " / ")));
                            n++;
                        }
            }
            catch (Exception e) { w.AppendLine("query failed: " + e.Message); }
            w.AppendLine("(" + n + " events)");
        }

        static string Run(string exe, string args, int timeoutMs, out int exitCode)
        {
            var psi = new ProcessStartInfo(exe, args)
            {
                UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true,
                StandardOutputEncoding = Encoding.UTF8,
            };
            using (var p = Process.Start(psi))
            {
                var stdout = p.StandardOutput.ReadToEndAsync();
                var stderr = p.StandardError.ReadToEndAsync();
                if (!p.WaitForExit(timeoutMs)) { try { p.Kill(); } catch (Exception) { } exitCode = -1; return "timed out after " + timeoutMs / 1000 + " s\n" + stderr.Result; }
                exitCode = p.ExitCode;
                return stdout.Result + (stderr.Result.Length > 0 ? "\n[stderr]\n" + stderr.Result : "");
            }
        }

        static string DxDiag()
        {
            var temp = Path.Combine(Path.GetTempPath(), "amdgpu-wddm-dxdiag-" + Guid.NewGuid().ToString("N") + ".txt");
            try
            {
                int code;
                var exe = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "dxdiag.exe");
                var output = Run(exe, "/t \"" + temp + "\"", 120000, out code);
                // dxdiag may return before the file is complete; wait for it to stop growing, bounded.
                long last = -1;
                for (int i = 0; i < 60; i++)
                {
                    long now = File.Exists(temp) ? new FileInfo(temp).Length : -1;
                    if (now > 0 && now == last) break;
                    last = now;
                    System.Threading.Thread.Sleep(1000);
                }
                return File.Exists(temp) ? File.ReadAllText(temp) : "dxdiag wrote no file (exit " + code + ")\n" + output;
            }
            catch (Exception e) { return "dxdiag failed: " + e.Message; }
            finally { try { File.Delete(temp); } catch (Exception) { } }
        }

        // A tool next to this program, else in the release's tools directory (HKLM\SOFTWARE\amdgpu-wddm\Release InstallDir).
        static string FindTool(string name)
        {
            var local = Path.Combine(Program.AppDirectory, name);
            if (File.Exists(local)) return local;
            try
            {
                using (var k = Microsoft.Win32.Registry.LocalMachine.OpenSubKey(@"SOFTWARE\amdgpu-wddm\Release"))
                {
                    var dir = k == null ? null : k.GetValue("InstallDir") as string;
                    if (!string.IsNullOrEmpty(dir) && File.Exists(Path.Combine(dir, "tools", name))) return Path.Combine(dir, "tools", name);
                }
            }
            catch (Exception) { }
            return null;
        }

        static string D3d12Caps()
        {
            var tool = FindTool("amdgpu_wddm_d3d12caps.exe");
            if (tool == null) return "{\"error\": \"amdgpu_wddm_d3d12caps.exe was not found next to the control application or in the release's tools directory\"}";
            try { int code; return Run(tool, "0", 60000, out code); }
            catch (Exception e) { return "{\"error\": \"" + e.Message.Replace("\"", "'") + "\"}"; }
        }

        static string VulkanSummary()
        {
            foreach (var candidate in new[] { FindTool("vulkaninfo.exe"), Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "vulkaninfo.exe") })
                if (candidate != null && File.Exists(candidate))
                {
                    try { int code; return Run(candidate, "--summary", 60000, out code); }
                    catch (Exception e) { return "vulkaninfo failed: " + e.Message; }
                }
            return "vulkaninfo.exe was not found (next to the control application or in System32).";
        }
    }
}
