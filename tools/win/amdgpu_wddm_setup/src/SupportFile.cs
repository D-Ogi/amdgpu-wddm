// The setup's support file (WU-067): a zip the user saves and decides whether to send. It holds the engine's events,
// terminal results, console output and log of every run of this window, and a summary, all in English and passed
// through the Redactor (no user or computer name). Nothing is uploaded.
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text;

namespace AmdgpuWddmSetup
{
    public static class SupportFile
    {
        const long MaxLog = 4 * 1024 * 1024;

        public static void Write(string zipPath, IList<EngineClient> runs, string summary, Redactor redactor)
        {
            var tmp = zipPath + ".partial";
            if (File.Exists(tmp)) File.Delete(tmp);
            using (var zip = ZipFile.Open(tmp, ZipArchiveMode.Create))
            {
                Add(zip, "setup.txt", summary, redactor);
                for (int i = 0; i < runs.Count; i++)
                {
                    var r = runs[i];
                    var p = "run-" + (i + 1) + "-";
                    Add(zip, p + "command.txt", r.CommandLineText + Environment.NewLine + "exit code: " + (r.ExitCode.HasValue ? r.ExitCode.Value.ToString() : "-") +
                        Environment.NewLine + "result: " + (r.Result != null ? "bound to this run" : "none (" + r.ResultProblem + ")") + Environment.NewLine, redactor);
                    AddFile(zip, p + "events.jsonl", r.EventsFile, redactor);
                    AddFile(zip, p + "result.json", r.ResultFile, redactor);
                    Add(zip, p + "engine-output.txt", r.Output, redactor);
                    if (r.Result != null && !string.IsNullOrEmpty(r.Result.Log)) AddFile(zip, p + "engine-log.txt", r.Result.Log, redactor);
                }
            }
            if (File.Exists(zipPath)) File.Delete(zipPath);
            File.Move(tmp, zipPath);
        }

        static void AddFile(ZipArchive zip, string name, string path, Redactor redactor)
        {
            string text;
            try
            {
                if (string.IsNullOrEmpty(path) || !File.Exists(path)) return;
                using (var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                {
                    var len = (int)Math.Min(fs.Length, MaxLog);
                    if (fs.Length > MaxLog) fs.Seek(-len, SeekOrigin.End);
                    var buf = new byte[len];
                    int read = 0;
                    while (read < len) { int n = fs.Read(buf, read, len - read); if (n <= 0) break; read += n; }
                    text = (fs.Length > MaxLog ? "(first " + (fs.Length - MaxLog) + " bytes left out)\r\n" : "") + Encoding.UTF8.GetString(buf, 0, read);
                }
            }
            catch (Exception e) { text = "cannot read " + path + ": " + e.Message; }
            Add(zip, name, text, redactor);
        }

        static void Add(ZipArchive zip, string name, string text, Redactor redactor)
        {
            var entry = zip.CreateEntry(name, CompressionLevel.Optimal);
            using (var w = new StreamWriter(entry.Open(), new UTF8Encoding(false))) w.Write(redactor.Apply(text ?? ""));
        }
    }
}
