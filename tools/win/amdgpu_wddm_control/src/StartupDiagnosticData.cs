// Bounded text collection shared by the isolated worker and offline tests.
using System;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;

namespace AmdgpuWddmControl
{
    public sealed class StartupDiagnosticData
    {
        public const int Limit = 4 * 1024 * 1024;
        public const int TimeoutMs = 20000;
        public const string DeviceId = @"PCI\VEN_1002&DEV_13FE";
        // The worker's own exit codes (RunWorker). 0: the collection ran to its end; a source that failed is a line
        // of the text, not an exit code. WorkerFaulted: an exception outside every source stopped the collection, and
        // its type, message and stack are the last lines of the text. WorkerOutputLost: the parent's end of the
        // output pipe went away, so nothing after that point reached the report.
        public const int WorkerFaulted = 3, WorkerOutputLost = 4;
        readonly TextWriter _writer;
        readonly Redactor _redactor;
        readonly Func<long> _elapsed;
        public int Bytes { get; private set; }
        // The output could not be written (the parent closed its end of the pipe): nothing more is collected.
        public bool OutputLost { get; private set; }
        public bool Active { get { return !OutputLost && Bytes < Limit - 4096 && _elapsed() < TimeoutMs - 1500; } }

        public StartupDiagnosticData(TextWriter writer, Redactor redactor, Func<long> elapsed)
        { _writer = writer; _redactor = redactor; _elapsed = elapsed; }

        // The worker process's whole run (BD-142). The text goes to output as UTF-8 through a writer of its own:
        // the worker is a GUI-subsystem process without a console, so Console.OutputEncoding, which calls
        // SetConsoleOutputCP, throws IOException 0x80070006 there (tester.29: every report's worker ended with
        // 0xE0434352 before it wrote one line). Every exception of collect ends here as text and an exit code, never
        // as an unhandled exception: a source that throws is already a line of its own (Source), and anything
        // outside the sources is written with its type, message and stack before the worker exits.
        public static int RunWorker(Stream output, Redactor redactor, Func<long> elapsed, Action<StartupDiagnosticData> collect)
        {
            StreamWriter writer;
            try { writer = new StreamWriter(output, new UTF8Encoding(false), 4096, true); }
            catch (Exception) { return WorkerOutputLost; }      // no output to write to (null, or not writable)
            var data = new StartupDiagnosticData(writer, redactor, elapsed);
            try
            {
                collect(data);
                return data.OutputLost ? WorkerOutputLost : 0;
            }
            catch (Exception e)
            {
                // Past the deadline and the 4 KiB reserve too: these lines say why the text ends here. The parent
                // keeps the text up to 1 KiB under the limit (DiagnosticWorker.Run).
                try { data.Write(Fit("\n=== The collection stopped ===\nAn error outside every source above ended the collection here.\n" + Describe(e), 2048), 1024); }
                catch (Exception) { }
                return data.OutputLost ? WorkerOutputLost : WorkerFaulted;
            }
            finally
            {
                try { writer.Dispose(); } catch (Exception) { }
            }
        }

        // An exception as the report shows it: the short type name first (as every "Unavailable:" line has always
        // started), then the full type, the HRESULT and the stack, and the same for each inner exception.
        public static string Describe(Exception e)
        {
            var w = new StringBuilder();
            for (int depth = 0; e != null && depth < 4; e = e.InnerException, depth++)
            {
                w.Append(depth == 0 ? "" : "Inner exception: ").Append(e.GetType().Name).Append(": ").Append(e.Message).Append('\n');
                w.Append("  type ").Append(e.GetType().FullName).Append(", HRESULT 0x")
                    .Append(e.HResult.ToString("X8", System.Globalization.CultureInfo.InvariantCulture)).Append('\n');
                if (!string.IsNullOrEmpty(e.StackTrace))
                    foreach (var line in e.StackTrace.Split(new[] { "\r\n", "\n" }, StringSplitOptions.RemoveEmptyEntries)) w.Append("  ").Append(line.Trim()).Append('\n');
            }
            return w.ToString();
        }

        public static bool Needed(bool present, bool installed, uint problem, bool confirmed)
        { return (present || installed) && (problem != 0 || !confirmed); }

        // Which REG_DWORDs of the start and stop records read as a hexadecimal word, and which as a plain number.
        // Stage, StartStage, BootId, HealthFlags, LogLines and UnconfirmedStarts are counts and small numbers: the
        // driver's own log, the installer's reader (diagnostics.ps1) and the lab arms all print them in decimal,
        // and the report prints the number of THIS boot in decimal a few lines under the records, which the reader
        // is told to compare with the record's BootId. A hexadecimal Stage = 0x0000004F against an expected 79
        // made that comparison a conversion by hand. Status is an NTSTATUS and Version the escape ABI word, and
        // both are only ever quoted as hexadecimal words, so those two keep it.
        static readonly string[] HexFields = { "Status", "Version" };

        // One value of a record as the report shows it.
        public static string Field(string name, object value)
        {
            if (value is string[]) return string.Join("\n", (string[])value);
            if (value is int)
            {
                uint number = (uint)(int)value;
                return Array.IndexOf(HexFields, name) >= 0
                    ? "0x" + number.ToString("X8", System.Globalization.CultureInfo.InvariantCulture)
                    : number.ToString(System.Globalization.CultureInfo.InvariantCulture);
            }
            return Convert.ToString(value, System.Globalization.CultureInfo.InvariantCulture);
        }

        // Trim by encoded bytes, preserving whole surrogate pairs.
        public static string Fit(string text, int bytes)
        {
            if (bytes <= 0 || string.IsNullOrEmpty(text)) return "";
            if (Encoding.UTF8.GetByteCount(text) <= bytes) return text;
            int low = 0, high = Math.Min(text.Length, bytes);
            while (low < high)
            {
                int mid = low + (high - low + 1) / 2;
                if (Encoding.UTF8.GetByteCount(text.Substring(0, mid)) <= bytes) low = mid; else high = mid - 1;
            }
            if (low > 0 && char.IsHighSurrogate(text[low - 1])) low--;
            return text.Substring(0, low);
        }

        public void Add(string text)
        {
            if (!Active) return;
            Write(text, 4096);
        }

        // Writes past the collection deadline too (the last lines of a stopped collection), within the byte limit
        // less reserve. A failed write of the pipe (IOException, a closed writer) marks the output lost instead of
        // throwing: a source's failure line must not become a second exception in its own catch.
        void Write(string text, int reserve)
        {
            if (OutputLost) return;
            // Limit both the regex input and the encoded output. Replacements can grow text.
            string clean = _redactor.Apply(Fit(text, 512 * 1024));
            clean = Fit(clean, Limit - reserve - Bytes);
            if (clean.Length == 0) return;
            try
            {
                _writer.Write(clean);
                _writer.Flush();
            }
            catch (IOException) { OutputLost = true; return; }
            catch (ObjectDisposedException) { OutputLost = true; return; }
            Bytes += Encoding.UTF8.GetByteCount(clean);
        }

        // One source: its header, then what it adds. An exception of the source ends that source only: the report
        // names it with its type, message and stack, and the next source runs.
        public void Source(string name, Action action)
        {
            if (!Active) return;
            Add("\n=== " + name + " ===\n");
            try { action(); }
            catch (Exception e) { Add("Unavailable: " + Describe(e)); }
        }

        public static string Tail(string path, int limit)
        {
            // Do not follow a diagnostic path redirected to another location.
            CheckPath(path);
            using (var f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
            {
                long size = f.Length;
                int count = (int)Math.Min(size, limit);
                var data = new byte[count];
                f.Seek(size - count, SeekOrigin.Begin);
                int got = 0, n;
                while (got < count && (n = f.Read(data, got, count - got)) > 0) got += n;
                // SetupAPI is UTF-16 on some systems. Detect from its BOM, even after seeking to its tail.
                f.Position = 0;
                int a = f.ReadByte(), b = f.ReadByte();
                Encoding encoding = a == 255 && b == 254 ? Encoding.Unicode : a == 254 && b == 255 ? Encoding.BigEndianUnicode : Encoding.UTF8;
                int offset = encoding != Encoding.UTF8 && ((size - count) & 1) != 0 ? 1 : 0;
                string text = encoding.GetString(data, offset, Math.Max(0, got - offset));
                return text.TrimStart('\uFEFF');
            }
        }

        public static void CheckPath(string path)
        {
            for (string p = Path.GetFullPath(path); !string.IsNullOrEmpty(p); p = Path.GetDirectoryName(p))
                if ((File.GetAttributes(p) & FileAttributes.ReparsePoint) != 0) throw new IOException("Redirected path omitted.");
        }

        public static string SetupSections(string text)
        {
            var result = new StringBuilder();
            var section = new StringBuilder();
            bool haveHeader = false;
            using (var reader = new StringReader(text))
                for (string line; (line = reader.ReadLine()) != null; )
                {
                    if (Regex.IsMatch(line, @"^>>>\s+\[", RegexOptions.CultureInvariant))
                    {
                        if (haveHeader && section.ToString().IndexOf(DeviceId, StringComparison.OrdinalIgnoreCase) >= 0) result.Append(section);
                        section.Clear(); haveHeader = true;
                    }
                    if (haveHeader) section.AppendLine(line);
                }
            if (haveHeader && section.ToString().IndexOf(DeviceId, StringComparison.OrdinalIgnoreCase) >= 0) result.Append(section);
            return result.Length == 0 ? "No matching device sections in the bounded tail.\n" : result.ToString();
        }
    }
}
