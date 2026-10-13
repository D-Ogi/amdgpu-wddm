// Automatic failed-start report. This worker never loads the driver DLL or
// sends an escape. Its parent kills it at the collection deadline if an OS
// provider blocks. Reports stay local and pass through the common redactor.
using System;
using System.Diagnostics;
using System.Diagnostics.Eventing.Reader;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Management;
using System.Reflection;
using System.Text;
using Microsoft.Win32;

namespace AmdgpuWddmControl
{
    public static class StartupDiagnostics
    {
        static readonly string[] Providers = { "Display", "Microsoft-Windows-DxgKrnl", "Microsoft-Windows-Kernel-PnP", "bc250kmd" };
        static readonly string[] Fields = { "Result", "Step", "SubStep", "Status", "Stage", "StartStage", "HealthFlags",
            "BootId", "Generation", "Epoch", "UnconfirmedStarts", "Version", "TimeUtc", "StopTimeUtc", "Detail", "LogLines", "Log" };

        // The worker's entry (--startup-diagnostics-worker). BD-142: this process is the GUI-subsystem exe started
        // with its output redirected, so it has no console. It never touches Console.OutputEncoding (whose setter
        // calls SetConsoleOutputCP and throws IOException "The handle is invalid" without a console: every worker of
        // tester.29 ended there with 0xE0434352 and an empty report). The text goes to the redirected standard
        // output as UTF-8 through RunWorker, which also turns any exception into report text and an exit code.
        public static int Worker()
        {
            var clock = Stopwatch.StartNew();
            using (var output = Console.OpenStandardOutput())
                return StartupDiagnosticData.RunWorker(output, Redactor.ForThisPc(), () => clock.ElapsedMilliseconds, Collect);
        }

        static void Collect(StartupDiagnosticData data)
        {
            data.Add("Automatic start/stop diagnostics; read-only; no GPU requests.\nUTC: " + DateTime.UtcNow.ToString("o") + "\n");
            data.Source("StartRecord", () => RegistryRecord(data, "StartRecord"));
            data.Source("StopRecord", () => RegistryRecord(data, "StopRecord"));
            DateTime? boot = null;
            try { boot = RecoveryProbe.BootTime(); }
            catch (Exception e) { data.Add("\nBoot time unavailable: " + StartupDiagnosticData.Describe(e)); }
            // The reader of this file is told to compare this number with the BootId of the records above, which
            // the driver writes from KUSER_SHARED_DATA. It therefore comes from that same page, not from the
            // registry copy that can lag a boot by one (BD-130), and the line names the source it used.
            string bootIdSource;
            var bootId = DriverCardProbe.SharedBootId(out bootIdSource);
            data.Add("\nCurrent boot UTC: " + (boot.HasValue ? boot.Value.ToString("o") : "unavailable") +
                "; boot ID: " + (bootId?.ToString(CultureInfo.InvariantCulture) ?? "unavailable") +
                " (read from " + bootIdSource + ")\n");
            data.Source("Device state", () => DeviceState(data));
            data.Source("System events, current boot", () => Events(data, boot));
            data.Source("SetupAPI device sections (last 1 MiB only)", () =>
                data.Add(StartupDiagnosticData.SetupSections(StartupDiagnosticData.Tail(
                    Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows), "INF", "setupapi.dev.log"), 1024 * 1024))));
            data.Source("Kept KMD logs (up to three files, last 512 KiB each)", () => KeptLogs(data));
            data.Add("\nCollection finished. Missing or truncated sources above are not proof of absence of a fault.\n");
        }

        static void RegistryRecord(StartupDiagnosticData data, string record)
        {
            using (var root = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64))
            using (var key = root.OpenSubKey(@"SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters\" + record))
            {
                if (key == null) { data.Add("Absent (older drivers may not write this record).\n"); return; }
                foreach (string name in Fields)
                {
                    if (!data.Active) break;
                    object value = key.GetValue(name, null, RegistryValueOptions.DoNotExpandEnvironmentNames);
                    if (value == null) continue;
                    string text = StartupDiagnosticData.Field(name, value);
                    data.Add(name + " = " + text + "\n");
                }
                data.Add("Record timestamps and boot identity must match this start before attributing a failure.\n");
            }
        }

        static void DeviceState(StartupDiagnosticData data)
        {
            var options = new EnumerationOptions { ReturnImmediately = true, Timeout = TimeSpan.FromSeconds(2) };
            using (var search = new ManagementObjectSearcher(new ManagementScope(@"root\cimv2"),
                new ObjectQuery("SELECT Name, Service, Status, ConfigManagerErrorCode FROM Win32_PnPEntity WHERE PNPDeviceID LIKE 'PCI\\\\VEN_1002&DEV_13FE%'"), options))
            using (var rows = search.Get())
            {
                int count = 0;
                foreach (ManagementObject row in rows)
                    using (row)
                    {
                        if (!data.Active || count++ >= 8) break;
                        data.Add("Name=" + row["Name"] + "; Service=" + row["Service"] + "; Status=" + row["Status"] +
                            "; ConfigManagerErrorCode=" + Convert.ToString(row["ConfigManagerErrorCode"], CultureInfo.InvariantCulture) + "\n");
                    }
                if (count == 0) data.Add("No matching device returned.\n");
            }
        }

        static void Events(StartupDiagnosticData data, DateTime? boot)
        {
            if (boot == null) { data.Add("Boot time unavailable; current-boot event selection omitted.\n"); return; }
            string providers = string.Join(" or ", Providers.Select(p => "@Name='" + p + "'"));
            string xpath = "*[System[Provider[" + providers + "] and TimeCreated[@SystemTime >= '" +
                boot.Value.ToUniversalTime().ToString("o", CultureInfo.InvariantCulture) + "']]]";
            int count = 0;
            using (var reader = new EventLogReader(new EventLogQuery("System", PathType.LogName, xpath) { ReverseDirection = true }))
                while (data.Active && count < 256)
                {
                    using (var entry = reader.ReadEvent(TimeSpan.FromMilliseconds(250)))
                    {
                        if (entry == null) break;
                        // XML keeps stable event IDs and data across OS languages; no message-DLL loading.
                        string xml = entry.ToXml();
                        if (entry.ProviderName == "Microsoft-Windows-Kernel-PnP" &&
                            xml.Replace("&amp;", "&").IndexOf(StartupDiagnosticData.DeviceId, StringComparison.OrdinalIgnoreCase) < 0 &&
                            xml.IndexOf("bc250kmd", StringComparison.OrdinalIgnoreCase) < 0) continue;
                        data.Add(xml + "\n");
                        count++;
                    }
                }
            data.Add("Events kept: " + count + " (maximum 256).\n");
        }

        static void KeptLogs(StartupDiagnosticData data)
        {
            const string path = @"C:\BC250\kmdlog";
            if (!Directory.Exists(path)) { data.Add("No kept KMD log directory.\n"); return; }
            StartupDiagnosticData.CheckPath(path);
            var files = new DirectoryInfo(path).EnumerateFiles("*.log").Take(128)
                .Where(f => (f.Attributes & FileAttributes.ReparsePoint) == 0)
                .OrderByDescending(f => f.LastWriteTimeUtc).Take(3).ToArray();
            foreach (var file in files)
            {
                if (!data.Active) break;
                data.Source("KMD log " + file.Name, () => data.Add(StartupDiagnosticData.Tail(file.FullName, 512 * 1024)));
            }
            data.Add("At most 128 directory entries inspected; retained logs may be from earlier starts.\n");
        }

        public static string Collect()
        {
            return DiagnosticWorker.Run(new ProcessStartInfo(Assembly.GetExecutingAssembly().Location,
                "--startup-diagnostics-worker"), StartupDiagnosticData.TimeoutMs);
        }

        public static string SaveAutomatic()
        {
            var report = new BugReport(Redactor.ForThisPc());
            report.AddStartupDiagnostics(Collect());
            string dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "amdgpu-wddm", "diagnostics");
            Directory.CreateDirectory(dir);
            string path = Path.Combine(dir, "start-" + DateTime.UtcNow.ToString("yyyyMMddTHHmmssfffZ") + ".zip");
            report.Write(path);
            return path;
        }
    }
}
