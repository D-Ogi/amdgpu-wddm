// The single fixed PowerShell entry point. Reads and writes have bounded output and deadlines; no user-supplied code.
using System;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text;
using System.Threading.Tasks;

namespace AmdgpuWddmControl
{
    static class SystemTuningProcess
    {
        public const int Failed = 1, Usage = 2, Timeout = 3;

        static string Quote(string value)
        {
            // The fixed script path has no quote or trailing slash. Reject rather than silently rewriting a path.
            if (value.IndexOf('"') >= 0 || value.EndsWith("\\", StringComparison.Ordinal)) throw new ArgumentException("path");
            return "\"" + value + "\"";
        }

        static async Task<string> ReadBounded(StreamReader reader)
        {
            var text = new StringBuilder();
            var block = new char[4096];
            int count;
            while ((count = await reader.ReadAsync(block, 0, block.Length).ConfigureAwait(false)) != 0)
            {
                if (text.Length + count > SystemTuning.MaxJson) throw new IOException("output limit");
                text.Append(block, 0, count);
            }
            return text.ToString();
        }

        static SystemTuningSnapshot Invoke(string action, string item, int days, string scope)
        {
            SystemTuning.Arguments(action, item, days, scope);
            string file = Path.Combine(Program.AppDirectory, "system-tuning", "system-tuning.ps1");
            if (!File.Exists(file)) throw new IOException("backend missing");
            string command = "-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File " + Quote(file) + " -Scope " + scope + " -Action " + action;
            if (item != null) command += " -Item " + item; // Exact catalog ID, validated above; never free text.
            if (days != 0) command += " -PauseDays " + days.ToString(CultureInfo.InvariantCulture);
            var start = new ProcessStartInfo(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System),
                @"WindowsPowerShell\v1.0\powershell.exe"), command)
            {
                UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true,
                StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8,
                WorkingDirectory = Path.GetDirectoryName(file),
            };
            using (var process = Process.Start(start))
            {
                if (process == null) throw new IOException("backend start failed");
                var output = ReadBounded(process.StandardOutput);
                var error = ReadBounded(process.StandardError);
                var watch = Stopwatch.StartNew();
                int deadline = action == "List" ? 20000 : 120000;
                while (!process.WaitForExit(100))
                {
                    if (watch.ElapsedMilliseconds >= deadline || output.IsFaulted || error.IsFaulted)
                    {
                        try { process.Kill(); } catch (InvalidOperationException) { }
                        throw new TimeoutException("backend did not finish");
                    }
                }
                if (!Task.WaitAll(new Task[] { output, error }, 2000)) throw new TimeoutException("backend output incomplete");
                if (process.ExitCode != 0) throw new IOException("backend refused");
                return SystemTuning.Parse(output.Result, scope, action);
            }
        }

        public static SystemTuningSnapshot Read()
        {
            var machine = Invoke("List", null, 0, "Machine");
            var user = Invoke("List", null, 0, "User");
            machine.Ok = machine.Ok && user.Ok;
            machine.Items.AddRange(user.Items);
            return machine;
        }

        public static int ChangeUser(string action, string item)
        {
            try { return Invoke(action, item, 0, "User").Ok ? 0 : Failed; }
            catch (TimeoutException) { return Timeout; }
            catch (Exception) { return Failed; }
        }

        public static int Dispatch(string[] args)
        {
            string action, item, scope; int days;
            if (!SystemTuning.TryArguments(args, out action, out item, out days, out scope) || action == "List" || scope != "Machine") return Usage;
            if (!Program.IsElevated()) return Program.NotElevated;
            try { return Invoke(action, item, days, scope).Ok ? 0 : Failed; }
            catch (TimeoutException) { return Timeout; }
            catch (Exception) { return Failed; }
        }
    }
}
