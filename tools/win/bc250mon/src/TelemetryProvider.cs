// The GPU telemetry line (owner's request, 2026-09-30): Tctl, GPU load, GFX clock and VRAM in use, cheap enough
// to stay on the screen during a game. The desktop is composed on the CPU by DWM, so repaints are the expensive
// part: a 230x22 heartbeat window repainted every 250 ms ran through trials 139/140 with DWM at 28 % of the
// machine against 4 % without it. This line is therefore sampled four times a second but published at most
// every two seconds, OverlayForm repaints only its strip and only when the text changed, and it does so in the
// same repaint as the header clock, so the line never costs DWM a frame of its own.
//
// Sources, all read-only; none sends an SMU message on the overlay's behalf while the governor runs:
//   Tctl, GFX clock  the KMD clock governor's snapshot (BC250_ESCAPE_RUN_DPM READ via bc250control.dll Bc250Dpm):
//                    the Tctl of its last 25 ms tick and the SMU clock readback of the last second. Without the
//                    escape (KMD before 0.7.175), without a running governor, or with the flags saying the values
//                    are old, the typed clock read (BC250_ESCAPE_RUN_CLOCK) GpuProvider makes anyway stands in.
//   load             the governor's GRBM_STATUS.GUI_ACTIVE share, KMD 0.7.177 and later (FLAG_HW_BUSY): the mean
//                    of its ~100 ms average over the window's samples. Before 0.7.177 the only busy figure is the
//                    ring's submit-to-fence share, which read 7 % while ETW saw 91 % (trial 139), so the line
//                    says n/a instead of showing it.
//   VRAM             dxgkrnl's segment statistics (D3DKMTQueryStatistics via Bc250VideoMemory): bytes resident in
//                    the memory (non-aperture) segments over their commit limit, as Task Manager counts
//                    dedicated memory. Standard WDDM: it does not depend on what the KMD reports about itself.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Globalization;

namespace Bc250Mon
{
    // Samples of one publishing window, and what they add up to.
    public sealed class TelemetryWindow
    {
        public const uint FlagRunning = 1, GrbmBusyVersion = 0x000700B1;
        long _busySum;
        int _busySamples, _samples;
        DpmSnapshot _last;
        string _dpmError;

        public void Add(DpmSnapshot s)
        {
            _dpmError = null;
            // A stopped governor leaves its last tick behind: old values with fresh-looking flags.
            if ((s.Flags & FlagRunning) == 0) { _dpmError = "DPM governor not running"; return; }
            _samples++;
            _last = s;
            if ((s.Flags & DpmSnapshot.FlagHwBusy) != 0) { _busySum += Math.Min(1000u, s.BusyAvgPermille); _busySamples++; }
        }

        public void Fail(string error) { _dpmError = error; }

        public Telemetry Close(DateTime now, ITelemetrySource source)
        {
            var t = new Telemetry { Time = now };
            var notes = new List<string>();
            bool dpm = _samples > 0;
            if (dpm)
            {
                t.KmdVersion = _last.Version;
                if ((_last.Flags & DpmSnapshot.FlagTemperature) != 0) { t.TemperatureC = _last.TemperatureMc / 1000.0; t.TemperatureSource = "dpm"; }
                if ((_last.Flags & DpmSnapshot.FlagClock) != 0) { t.GfxMHz = _last.ObservedMHz; t.ClockSource = "dpm"; }
            }
            if (_busySamples > 0)
            {
                t.LoadPercent = _busySum / 10.0 / _busySamples;
                t.LoadSamples = _busySamples;
                t.LoadSource = "grbm";
            }
            else if (dpm && _last.Version < GrbmBusyVersion)
                notes.Add("load n/a: KMD 0x" + _last.Version.ToString("X8") + " has no GRBM busy share (0.7.177)");
            else if (dpm) notes.Add("load n/a: no GRBM busy samples");
            else notes.Add("load n/a: " + (_dpmError ?? "no DPM snapshot"));

            if (!t.TemperatureC.HasValue || !t.GfxMHz.HasValue)
            {
                try
                {
                    ClockSnapshot c = source.ReadClock();
                    if (!t.TemperatureC.HasValue) { t.TemperatureC = c.TemperatureMc / 1000.0; t.TemperatureSource = "clock"; }
                    if (!t.GfxMHz.HasValue) { t.GfxMHz = c.ObservedMHz; t.ClockSource = "clock"; }
                }
                catch (Exception e) { notes.Add("Tctl/clock n/a: " + e.Message); }
            }

            try
            {
                VideoMemorySnapshot m = source.ReadVideoMemory();
                t.VramUsedBytes = m.LocalResident;
                if (m.LocalLimit != 0 && m.LocalLimit != ulong.MaxValue) { t.VramTotalBytes = m.LocalLimit; t.VramTotalSource = "commit-limit"; }
                else if (m.DedicatedVideoMemory != 0) { t.VramTotalBytes = m.DedicatedVideoMemory; t.VramTotalSource = "dedicated"; }
                t.ApertureUsedBytes = m.ApertureResident;
                if (m.ApertureLimit != 0 && m.ApertureLimit != ulong.MaxValue) t.ApertureTotalBytes = m.ApertureLimit;
            }
            catch (Exception e) { notes.Add("VRAM n/a: " + e.Message); }

            t.TemperatureLevel = !t.TemperatureC.HasValue ? Level.Info : t.TemperatureC >= GpuProvider.ErrorC ? Level.Error :
                                 t.TemperatureC >= GpuProvider.WarnC ? Level.Warn : Level.Good;
            t.Note = notes.Count > 0 ? string.Join("; ", notes) : null;
            _busySum = 0; _busySamples = 0; _samples = 0;
            return t;
        }
    }

    public sealed class TelemetryProvider : IProvider
    {
        public static readonly TimeSpan SamplePeriod = TimeSpan.FromMilliseconds(250), PublishPeriod = TimeSpan.FromSeconds(2);
        readonly ITelemetrySource _source;
        readonly Func<TimeSpan> _clock;
        readonly TelemetryWindow _window = new TelemetryWindow();
        TimeSpan? _due;
        string _lastNote;

        public TelemetryProvider(ITelemetrySource source) : this(source, Stopwatch.StartNew()) { }
        TelemetryProvider(ITelemetrySource source, Stopwatch watch) : this(source, () => watch.Elapsed) { }
        public TelemetryProvider(ITelemetrySource source, Func<TimeSpan> clock) { _source = source; _clock = clock; }
        public string Name { get { return "telemetry"; } }
        public TimeSpan Period { get { return SamplePeriod; } }

        public void Poll(State state)
        {
            try { _window.Add(_source.ReadDpm()); }
            catch (Exception e) { _window.Fail(e.Message); }
            TimeSpan now = _clock();
            if (_due.HasValue && now < _due.Value) return;
            _due = now + PublishPeriod;
            Telemetry t = _window.Close(DateTime.Now, _source);
            state.SetTelemetry(t);
            // Logged on change only: an old KMD would otherwise say the same thing every two seconds forever.
            if (t.Note != _lastNote && t.Note != null) state.Log(Name, Level.Info, t.Note);
            _lastNote = t.Note;
        }

        // GET /telemetry and the "telemetry" member of GET /state; mon.py telemetry prints it.
        public static Dictionary<string, object> Describe(Telemetry t, DateTime now)
        {
            if (t == null) return new Dictionary<string, object> { { "available", false } };
            return new Dictionary<string, object>
            {
                { "available", true }, { "time", t.Time.ToString("o", CultureInfo.InvariantCulture) },
                { "ageSeconds", Math.Round((now - t.Time).TotalSeconds, 1) }, { "text", t.Text },
                { "temperatureC", t.TemperatureC.HasValue ? (object)Math.Round(t.TemperatureC.Value, 1) : null },
                { "temperatureLevel", t.TemperatureLevel.ToString() }, { "temperatureSource", t.TemperatureSource },
                { "loadPercent", t.LoadPercent.HasValue ? (object)Math.Round(t.LoadPercent.Value, 1) : null },
                { "loadSource", t.LoadSource }, { "loadSamples", t.LoadSamples },
                { "gfxMHz", t.GfxMHz }, { "clockSource", t.ClockSource },
                { "vramUsedMB", Telemetry.Megabytes(t.VramUsedBytes) }, { "vramTotalMB", Telemetry.Megabytes(t.VramTotalBytes) },
                { "vramUsedBytes", t.VramUsedBytes }, { "vramTotalBytes", t.VramTotalBytes }, { "vramTotalSource", t.VramTotalSource },
                { "apertureUsedMB", Telemetry.Megabytes(t.ApertureUsedBytes) }, { "apertureTotalMB", Telemetry.Megabytes(t.ApertureTotalBytes) },
                { "kmdVersion", t.KmdVersion != 0 ? "0x" + t.KmdVersion.ToString("X8") : null }, { "note", t.Note },
            };
        }
    }

    // The line itself, in OverlayForm's 96-dpi layout units, right under the title. Everything it draws stays
    // inside Bounds (it clips to it), which is what OverlayForm invalidates when the line changes.
    public static class TelemetryLine
    {
        public const int Top = 36, Height = 18, Bottom = Top + Height + 2;

        public static RectangleF Bounds(float x, float width) { return new RectangleF(x, Top, width, Height); }

        static List<KeyValuePair<string, Color>> Segments(Telemetry t, Func<Level, Color> color, Color dim)
        {
            var s = new List<KeyValuePair<string, Color>> { new KeyValuePair<string, Color>("GPU", dim) };
            if (t == null) { s.Add(new KeyValuePair<string, Color>("waiting for the first sample", dim)); return s; }
            s.Add(new KeyValuePair<string, Color>(t.Temperature, t.TemperatureC.HasValue ? color(t.TemperatureLevel) : dim));
            s.Add(new KeyValuePair<string, Color>("load " + t.Load, t.LoadPercent.HasValue ? color(Level.Info) : dim));
            s.Add(new KeyValuePair<string, Color>(t.Clock, t.GfxMHz.HasValue ? color(Level.Info) : dim));
            s.Add(new KeyValuePair<string, Color>("VRAM " + t.Vram, t.VramUsedBytes.HasValue ? color(Level.Info) : dim));
            return s;
        }

        // Consolas is monospaced: segments sit on a character grid, two columns apart.
        static float Advance(Graphics g, Font font)
        {
            return g.MeasureString("0000000000", font, PointF.Empty, StringFormat.GenericTypographic).Width / 10f;
        }

        public static float Width(Graphics g, Font font, Telemetry t)
        {
            int columns = -2;
            foreach (var s in Segments(t, l => Color.Empty, Color.Empty)) columns += s.Key.Length + 2;
            return columns * Advance(g, font);
        }

        public static void Paint(Graphics g, Font font, Telemetry t, float x, float width, Func<Level, Color> color, Color dim)
        {
            float advance = Advance(g, font);
            GraphicsState saved = g.Save();
            g.SetClip(Bounds(x, width), CombineMode.Intersect);
            int column = 0;
            foreach (var s in Segments(t, color, dim))
            {
                using (var b = new SolidBrush(s.Value)) g.DrawString(s.Key, font, b, x + column * advance, Top);
                column += s.Key.Length + 2;
            }
            g.Restore(saved);
        }
    }
}
