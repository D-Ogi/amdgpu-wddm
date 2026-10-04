// One run of the install engine (installer\install.ps1 or installer\prepare-offline.ps1) as a hidden Windows
// PowerShell child: its own invocation id, its events read as complete lines while it runs, its terminal result
// believed only when bound to that id, and the cancel file created only while the engine offers a safe point
// (docs/gui/interfaces-setup.md sections 1 to 3). The exit code alone is never the outcome.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Security.AccessControl;
using System.Security.Principal;
using System.Text;

namespace AmdgpuWddmSetup
{
    public sealed class EngineClient : IDisposable
    {
        public readonly string Invocation, RunDir, EventsFile, ResultFile, CancelFile, OutputFile;
        public readonly EngineRun Run;
        public EngineResult Result { get; private set; }
        public string ResultProblem { get; private set; }
        public int? ExitCode { get; private set; }
        public bool CancelRequested { get; private set; }
        public string CommandLineText { get; private set; }

        readonly object _gate = new object();
        Process _process;
        long _offset;
        readonly List<byte> _partial = new List<byte>();
        readonly StringBuilder _output = new StringBuilder();

        public static string PowerShell
        {
            get { return Path.Combine(Environment.SystemDirectory, "WindowsPowerShell", "v1.0", "powershell.exe"); }
        }

        // runRoot: the folder for this run's files. An elevated window keeps them in a folder only administrators and
        // SYSTEM can write, so that no unelevated program can plant a result or a cancel file.
        public EngineClient(string runRoot, bool protect)
        {
            Invocation = Guid.NewGuid().ToString();
            RunDir = Path.Combine(runRoot, DateTime.UtcNow.ToString("yyyyMMdd'T'HHmmss'Z'", System.Globalization.CultureInfo.InvariantCulture) + "-" + Invocation.Substring(0, 8));
            var dir = Directory.CreateDirectory(RunDir);
            if (protect) Protect(dir);
            EventsFile = Path.Combine(RunDir, "events.jsonl");
            ResultFile = Path.Combine(RunDir, "result.json");
            CancelFile = EventsFile + ".cancel";
            OutputFile = Path.Combine(RunDir, "engine-output.txt");
            Run = new EngineRun(Invocation);
        }

        static void Protect(DirectoryInfo dir)
        {
            var sec = new DirectorySecurity();
            sec.SetAccessRuleProtection(true, false);
            foreach (var sid in new[] { WellKnownSidType.BuiltinAdministratorsSid, WellKnownSidType.LocalSystemSid })
                sec.AddAccessRule(new FileSystemAccessRule(new SecurityIdentifier(sid, null), FileSystemRights.FullControl,
                    InheritanceFlags.ContainerInherit | InheritanceFlags.ObjectInherit, PropagationFlags.None, AccessControlType.Allow));
            dir.SetAccessControl(sec);
        }

        // script: the engine script's full path; args: its arguments after the GUI ones.
        public void Start(string script, IEnumerable<string> args)
        {
            var all = new List<string> { "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", script, "-Gui",
                "-InvocationId", Invocation, "-EventsFile", EventsFile, "-ResultFile", ResultFile };
            all.AddRange(args);
            CommandLineText = CommandLine.Join(all);
            var psi = new ProcessStartInfo(PowerShell, CommandLineText)
            {
                UseShellExecute = false, CreateNoWindow = true, WindowStyle = ProcessWindowStyle.Hidden,
                RedirectStandardOutput = true, RedirectStandardError = true, RedirectStandardInput = true,
                WorkingDirectory = Path.GetDirectoryName(script),
                StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8,
            };
            _process = new Process { StartInfo = psi, EnableRaisingEvents = true };
            _process.OutputDataReceived += (s, e) => { if (e.Data != null) lock (_gate) _output.AppendLine(e.Data); };
            _process.ErrorDataReceived += (s, e) => { if (e.Data != null) lock (_gate) _output.AppendLine("stderr: " + e.Data); };
            _process.Start();
            _process.StandardInput.Close();
            _process.BeginOutputReadLine();
            _process.BeginErrorReadLine();
        }

        public bool Running { get { return _process != null && !_process.HasExited; } }

        public bool Finished { get { return ExitCode.HasValue; } }

        // Reads what the engine wrote since the last call. Returns true when the model changed or the run ended.
        public bool Poll()
        {
            if (_process == null || Finished) return false;
            bool exited = _process.HasExited;
            bool changed = ReadEvents();
            if (!exited) return changed;
            _process.WaitForExit();
            ReadEvents();
            ExitCode = _process.ExitCode;
            string problem;
            Result = EngineResult.Read(SafeRead(ResultFile), Invocation, out problem);
            ResultProblem = problem;
            lock (_gate) try { File.WriteAllText(OutputFile, _output.ToString(), new UTF8Encoding(false)); } catch (Exception) { }
            return true;
        }

        bool ReadEvents()
        {
            if (!File.Exists(EventsFile)) return false;
            byte[] chunk;
            try
            {
                using (var fs = new FileStream(EventsFile, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                {
                    if (fs.Length <= _offset) return false;
                    fs.Seek(_offset, SeekOrigin.Begin);
                    chunk = new byte[fs.Length - _offset];
                    int read = 0;
                    while (read < chunk.Length) { int n = fs.Read(chunk, read, chunk.Length - read); if (n <= 0) break; read += n; }
                    if (read < chunk.Length) Array.Resize(ref chunk, read);
                    _offset += read;
                }
            }
            catch (IOException) { return false; }
            catch (UnauthorizedAccessException) { return false; }
            bool changed = false;
            foreach (var b in chunk)
            {
                if (b != (byte)'\n') { _partial.Add(b); continue; }
                var line = Encoding.UTF8.GetString(_partial.ToArray()).TrimEnd('\r');
                _partial.Clear();
                if (line.Length == 0) continue;
                Run.Apply(line);
                changed = true;
            }
            return changed;
        }

        // Asks the engine to stop at its next safe point. Only while it offers one; returns whether the request was made.
        public bool RequestCancel()
        {
            if (!Run.CancelAvailable || Finished) return false;
            try { File.WriteAllText(CancelFile, ""); CancelRequested = true; return true; }
            catch (Exception) { return false; }
        }

        public string Output { get { lock (_gate) return _output.ToString(); } }

        static string SafeRead(string path) { try { return File.Exists(path) ? File.ReadAllText(path, Encoding.UTF8) : null; } catch (Exception) { return null; } }

        public void Dispose() { if (_process != null) _process.Dispose(); }
    }
}
