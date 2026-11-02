// ROCm venv attach: discovers the payload and known venvs, runs attach-rocm-venv.ps1 -Check/-Attach/-Detach.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using Microsoft.Win32;

namespace AmdgpuWddmControl
{
    public sealed class RocmVenvEntry
    {
        public string Path;
        public string State;
        public string TheRock;
        public string AttachUtc;
        public List<string> Lines = new List<string>();
    }

    public sealed class RocmPayload
    {
        public string Directory;
        public string TheRock;
    }

    public sealed class RocmRunResult
    {
        public int ExitCode;
        public string Output;
        public string Error;
    }

    static class RocmAttach
    {
        const string ListDirBelow = @"amdgpu-wddm\rocm-venvs";
        const string PayloadRel = @"tools\hip\rocm";
        const string ScriptName = "attach-rocm-venv.ps1";
        const string ManifestName = "payload.json";
        const int CheckDeadline = 15000;
        const int ActionDeadline = 30000;
        const int MaxOutput = 64 * 1024;

        public static string ListDir
        {
            get { return System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), ListDirBelow); }
        }

        public static RocmPayload FindPayload()
        {
            foreach (var dir in PayloadCandidates())
            {
                var manifest = System.IO.Path.Combine(dir, ManifestName);
                if (!File.Exists(manifest)) continue;
                try
                {
                    var text = File.ReadAllText(manifest, Encoding.UTF8);
                    var therock = JsonField(text, "therock");
                    if (therock != null) return new RocmPayload { Directory = dir, TheRock = therock };
                }
                catch (Exception) { }
            }
            return null;
        }

        static List<string> PayloadCandidates()
        {
            var list = new List<string>();
            var beside = System.IO.Path.Combine(Program.AppDirectory, "rocm");
            if (Directory.Exists(beside)) list.Add(beside);

            try
            {
                using (var k = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\amdgpu-wddm\Release"))
                {
                    var dir = k == null ? null : k.GetValue("InstallDir") as string;
                    if (!string.IsNullOrEmpty(dir))
                    {
                        var candidate = System.IO.Path.Combine(dir, PayloadRel);
                        if (Directory.Exists(candidate)) list.Add(candidate);
                    }
                }
            }
            catch (Exception) { }
            return list;
        }

        public static List<RocmVenvEntry> DiscoverVenvs()
        {
            var entries = new List<RocmVenvEntry>();
            var listDir = ListDir;
            if (!Directory.Exists(listDir)) return entries;
            try
            {
                foreach (var file in Directory.GetFiles(listDir, "*.json"))
                {
                    try
                    {
                        var text = File.ReadAllText(file, Encoding.UTF8);
                        var venv = JsonField(text, "venv");
                        if (string.IsNullOrEmpty(venv)) continue;
                        if (entries.Any(e => string.Equals(e.Path, venv, StringComparison.OrdinalIgnoreCase))) continue;
                        entries.Add(new RocmVenvEntry
                        {
                            Path = venv,
                            TheRock = JsonField(text, "therock") ?? "",
                            AttachUtc = JsonField(text, "utc") ?? "",
                        });
                    }
                    catch (Exception) { }
                }
            }
            catch (Exception) { }
            return entries;
        }

        public static RocmRunResult Check(string payloadDir, string venvPath)
        {
            return Run(payloadDir, venvPath, "-Check", CheckDeadline);
        }

        public static RocmRunResult Attach(string payloadDir, string venvPath)
        {
            return Run(payloadDir, venvPath, null, ActionDeadline);
        }

        public static RocmRunResult Detach(string payloadDir, string venvPath)
        {
            return Run(payloadDir, venvPath, "-Detach", ActionDeadline);
        }

        static RocmRunResult Run(string payloadDir, string venvPath, string mode, int deadline)
        {
            var script = System.IO.Path.Combine(payloadDir, ScriptName);
            if (!File.Exists(script)) return new RocmRunResult { ExitCode = -1, Output = "", Error = "Script not found: " + script };

            var args = new StringBuilder();
            args.Append("-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File ");
            args.Append(Quote(script));
            args.Append(" -Venv ").Append(Quote(venvPath));
            args.Append(" -Payload ").Append(Quote(payloadDir));
            if (mode != null) args.Append(" ").Append(mode);

            var start = new ProcessStartInfo(
                System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System),
                    @"WindowsPowerShell\v1.0\powershell.exe"), args.ToString())
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                StandardOutputEncoding = Encoding.UTF8,
                StandardErrorEncoding = Encoding.UTF8,
                WorkingDirectory = payloadDir,
            };

            try
            {
                using (var process = Process.Start(start))
                {
                    if (process == null) return new RocmRunResult { ExitCode = -1, Error = "Process did not start" };
                    var outBuf = new StringBuilder();
                    var errBuf = new StringBuilder();
                    process.OutputDataReceived += (s, e) => { if (e.Data != null && outBuf.Length < MaxOutput) outBuf.AppendLine(e.Data); };
                    process.ErrorDataReceived += (s, e) => { if (e.Data != null && errBuf.Length < MaxOutput) errBuf.AppendLine(e.Data); };
                    process.BeginOutputReadLine();
                    process.BeginErrorReadLine();
                    if (!process.WaitForExit(deadline))
                    {
                        try { process.Kill(); } catch (Exception) { }
                        return new RocmRunResult { ExitCode = -1, Output = outBuf.ToString(), Error = "Timed out after " + deadline + " ms" };
                    }
                    process.WaitForExit();
                    return new RocmRunResult { ExitCode = process.ExitCode, Output = outBuf.ToString(), Error = errBuf.ToString() };
                }
            }
            catch (Exception ex)
            {
                return new RocmRunResult { ExitCode = -1, Error = ex.Message };
            }
        }

        public static string ParseState(string checkOutput)
        {
            if (string.IsNullOrEmpty(checkOutput)) return "unknown";
            var lines = checkOutput.Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries);
            var last = lines.LastOrDefault(l => l.StartsWith("ROCM-ATTACH ", StringComparison.Ordinal));
            if (last == null) return "unknown";
            if (last.Contains("CHECK attached")) return "attached";
            if (last.Contains("CHECK detached")) return "detached";
            if (last.Contains("CHECK REFUSED")) return "refused";
            return "unknown";
        }

        public static string ParseActionResult(string output)
        {
            if (string.IsNullOrEmpty(output)) return "";
            var lines = output.Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries);
            var last = lines.LastOrDefault(l => l.StartsWith("ROCM-ATTACH ", StringComparison.Ordinal));
            return last ?? "";
        }

        static string Quote(string value)
        {
            if (value.IndexOf('"') >= 0) throw new ArgumentException("path contains quotes");
            return "\"" + value + "\"";
        }

        static string JsonField(string json, string key)
        {
            var needle = "\"" + key + "\"";
            int i = json.IndexOf(needle, StringComparison.Ordinal);
            if (i < 0) return null;
            i = json.IndexOf(':', i + needle.Length);
            if (i < 0) return null;
            i = json.IndexOf('"', i + 1);
            if (i < 0) return null;
            int end = json.IndexOf('"', i + 1);
            if (end < 0) return null;
            return json.Substring(i + 1, end - i - 1);
        }
    }
}
