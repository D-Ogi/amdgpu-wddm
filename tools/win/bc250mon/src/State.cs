// The single source of truth of the monitor. Providers, the HTTP API and the UI only talk to each other
// through this object, so any of them can be replaced or added without touching the others.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;

namespace Bc250Mon
{
    public enum Level { Info, Good, Warn, Error }

    public sealed class Row
    {
        public string Label, Value;
        public Level Level;
        public Row(string label, string value, Level level = Level.Info) { Label = label; Value = value; Level = level; }
    }

    public sealed class Panel
    {
        public string Name, Title;
        public int Order;                       // providers use 0-99, remote panels default to 100+
        public List<Row> Rows = new List<Row>();
    }

    public sealed class LogEntry
    {
        public DateTime Time;
        public Level Level;
        public string Source, Text;
    }

    // One published GPU telemetry sample (TelemetryProvider). Not a panel: a panel change repaints the whole
    // window, this line only its own strip. A null value means not available, and Note says why.
    public sealed class Telemetry
    {
        static readonly CultureInfo Inv = CultureInfo.InvariantCulture;
        public DateTime Time;
        public double? TemperatureC;
        public Level TemperatureLevel;
        public double? LoadPercent;
        public int LoadSamples;
        public uint? GfxMHz;
        public ulong? VramUsedBytes, VramTotalBytes, ApertureUsedBytes, ApertureTotalBytes;
        // The case fan, from the board's own hardware monitor (docs/design/fan.md). Null means no reading: the
        // gate is closed, the reader is offline, the sample is stale or the driver is older than 0.7.212.1.
        // FanStopped is the one case worth a colour: a duty output runs and no tachometer turns.
        public uint? FanRpm, FanDutyPercent;
        public double? FanApuC;
        public bool FanStopped;
        public string TemperatureSource, LoadSource, ClockSource, VramTotalSource, Note;
        public uint KmdVersion;

        public static long? Megabytes(ulong? bytes) { return bytes.HasValue ? (long?)((bytes.Value + (1ul << 19)) >> 20) : null; }
        public string Temperature { get { return TemperatureC.HasValue ? TemperatureC.Value.ToString("0.0", Inv) + " C" : "n/a"; } }
        public string Load { get { return LoadPercent.HasValue ? LoadPercent.Value.ToString("0", Inv) + " %" : "n/a"; } }
        public string Clock { get { return GfxMHz.HasValue ? GfxMHz.Value.ToString(Inv) + " MHz" : "n/a MHz"; } }
        public string Vram
        {
            get
            {
                if (!VramUsedBytes.HasValue) return "n/a";
                string used = Megabytes(VramUsedBytes).Value.ToString(Inv);
                return (VramTotalBytes.HasValue ? used + "/" + Megabytes(VramTotalBytes).Value.ToString(Inv) : used) + " MB";
            }
        }
        public string Fan
        {
            get
            {
                if (FanStopped) return "stopped";
                if (!FanRpm.HasValue) return "n/a";
                return FanRpm.Value.ToString(Inv) + " rpm" +
                       (FanDutyPercent.HasValue ? " (" + FanDutyPercent.Value.ToString(Inv) + " %)" : "");
            }
        }
        public string Text
        {
            get { return "Tctl " + Temperature + "  load " + Load + "  GFX " + Clock + "  VRAM " + Vram + "  fan " + Fan; }
        }
    }

    public sealed class State
    {
        readonly object _lock = new object();
        readonly Dictionary<string, Panel> _panels = new Dictionary<string, Panel>();
        readonly LinkedList<LogEntry> _log = new LinkedList<LogEntry>();
        readonly string _logDir, _stopFile;
        string _status = "monitor started";
        Level _statusLevel = Level.Info;
        bool _stop;
        Telemetry _telemetry;

        public const int LogCapacity = 300;
        public const string StopFileName = "STOP";       // MeasurementGuardProvider lists it among the markers
        public event Action Changed;            // raised on any change, on the caller's thread
        public event Action TelemetryChanged;   // raised when the telemetry line would look different, not on Changed

        public State(string dataDir)
        {
            _logDir = Path.Combine(dataDir, "log");
            _stopFile = Path.Combine(dataDir, StopFileName);
            Directory.CreateDirectory(_logDir);
            _stop = File.Exists(_stopFile);
        }

        public void SetPanel(Panel p)
        {
            lock (_lock) _panels[p.Name] = p;
            Raise();
        }

        public void RemovePanel(string name)
        {
            lock (_lock) _panels.Remove(name);
            Raise();
        }

        // Every sample is kept for the API; the screen hears only about one that changes what it shows.
        public Telemetry Telemetry { get { lock (_lock) return _telemetry; } }
        public void SetTelemetry(Telemetry t)
        {
            bool changed;
            lock (_lock)
            {
                changed = _telemetry == null || _telemetry.Text != t.Text || _telemetry.TemperatureLevel != t.TemperatureLevel;
                _telemetry = t;
            }
            var h = TelemetryChanged;
            if (changed && h != null) h();
        }

        public void SetStatus(string text, Level level)
        {
            lock (_lock) { _status = text; _statusLevel = level; }
            Log("status", level, text);
        }

        // The stop flag is the owner's brake: test scripts must poll it (HTTP /flags or the STOP file) and end.
        public bool StopRequested
        {
            get { lock (_lock) return _stop; }
            set
            {
                lock (_lock) _stop = value;
                try { if (value) File.WriteAllText(_stopFile, DateTime.Now.ToString("s")); else File.Delete(_stopFile); } catch { }
                Log("control", value ? Level.Error : Level.Good, value ? "STOP requested" : "stop cleared");
            }
        }

        public void Log(string source, Level level, string text)
        {
            var e = new LogEntry { Time = DateTime.Now, Level = level, Source = source, Text = text };
            lock (_lock)
            {
                _log.AddLast(e);
                while (_log.Count > LogCapacity) _log.RemoveFirst();
                try
                {
                    // One flushed line per event: the file must be useful after a hard hang.
                    File.AppendAllText(Path.Combine(_logDir, e.Time.ToString("yyyyMMdd") + ".log"),
                        string.Format("{0:HH:mm:ss.fff}\t{1}\t{2}\t{3}\r\n", e.Time, level, source, text));
                }
                catch { }
            }
            Raise();
        }

        public sealed class Snapshot
        {
            public string Status;
            public Level StatusLevel;
            public bool Stop;
            public List<Panel> Panels;
            public List<LogEntry> Log;
        }

        public Snapshot Take(int logLines)
        {
            lock (_lock)
                return new Snapshot
                {
                    Status = _status, StatusLevel = _statusLevel, Stop = _stop,
                    Panels = _panels.Values.OrderBy(p => p.Order).ThenBy(p => p.Name).ToList(),
                    Log = _log.Skip(Math.Max(0, _log.Count - logLines)).ToList(),
                };
        }

        void Raise() { var h = Changed; if (h != null) h(); }
    }
}
