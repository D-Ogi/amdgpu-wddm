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
        readonly TextWriter _writer;
        readonly Redactor _redactor;
        readonly Func<long> _elapsed;
        public int Bytes { get; private set; }
        public bool Active { get { return Bytes < Limit - 4096 && _elapsed() < TimeoutMs - 1500; } }

        public StartupDiagnosticData(TextWriter writer, Redactor redactor, Func<long> elapsed)
        { _writer = writer; _redactor = redactor; _elapsed = elapsed; }

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
            // Limit both the regex input and the encoded output. Replacements can grow text.
            string clean = _redactor.Apply(Fit(text, 512 * 1024));
            clean = Fit(clean, Limit - 4096 - Bytes);
            _writer.Write(clean);
            _writer.Flush();
            Bytes += Encoding.UTF8.GetByteCount(clean);
        }

        public void Source(string name, Action action)
        {
            if (!Active) return;
            Add("\n=== " + name + " ===\n");
            try { action(); }
            catch (Exception e) { Add("Unavailable: " + e.GetType().Name + ": " + e.Message + "\n"); }
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
