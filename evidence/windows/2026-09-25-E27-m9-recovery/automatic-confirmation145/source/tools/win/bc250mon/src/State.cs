// The single source of truth of the monitor. Providers, the HTTP API and the UI only talk to each other
// through this object, so any of them can be replaced or added without touching the others.
using System;
using System.Collections.Generic;
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

    public sealed class State
    {
        readonly object _lock = new object();
        readonly Dictionary<string, Panel> _panels = new Dictionary<string, Panel>();
        readonly LinkedList<LogEntry> _log = new LinkedList<LogEntry>();
        readonly string _logDir, _stopFile;
        string _status = "monitor started";
        Level _statusLevel = Level.Info;
        bool _stop;

        public const int LogCapacity = 300;
        public event Action Changed;            // raised on any change, on the caller's thread

        public State(string dataDir)
        {
            _logDir = Path.Combine(dataDir, "log");
            _stopFile = Path.Combine(dataDir, "STOP");
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
